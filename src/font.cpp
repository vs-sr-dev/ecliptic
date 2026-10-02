// Palm OS Font Manager with the device's own fonts: the ROM's 'afti' table maps FontIDs to
// 'afnx' resources (Latin Locale Module, Boot). Fonts are FontType v1 or extended v2 with one
// bitmap strike per density (72 and 144 dpi on the Zodiac); text on the 1.5x screen is sampled
// from the densest strike. Metrics are returned in the active coordinate system.
#include "font.h"
#include "palmos.h"
#include <map>

uint16_t win_coord_system();   // sys.cpp

namespace {

struct Font {
    bool valid = false, be = false;
    int first = 0, last = 0, maxWidth = 0, rectWidth = 0, rectHeight = 0, ascent = 0, descent = 0, leading = 0;
    int rowWords = 0;
    uint32_t loc = 0, ow = 0;        // location table, offset/width table
    int strikeDensity = 72;
    uint32_t strike = 0;             // densest bitmap strike
};

std::map<int, uint32_t> defined;   // FontID -> FontType* (FntDefineFont)
std::map<uint32_t, Font> cache;    // FontType* -> parsed
int cur_font = 0;

uint16_t rd16(uint32_t a, bool be) { uint16_t v = r16(a); return be ? (uint16_t)(v >> 8 | v << 8) : v; }
uint32_t rd32(uint32_t a, bool be) { uint32_t v = r32(a); return be ? __builtin_bswap32(v) : v; }

const Font& parse(uint32_t p) {
    auto it = cache.find(p);
    if (it != cache.end()) return it->second;
    Font& f = cache[p];
    if (!p) return f;
    uint16_t type = r16(p);
    f.be = (type & 0xF000) != 0x9000;   // 68k fonts are big endian
    type = rd16(p, f.be);
    if ((type & 0xF000) != 0x9000) return f;
    auto h = [&](int i) { return (int)(int16_t)rd16(p + i * 2, f.be); };
    f.first = h(1); f.last = h(2); f.maxWidth = h(3); f.rectWidth = h(6); f.rectHeight = h(7);
    f.ascent = h(9); f.descent = h(10); f.leading = h(11); f.rowWords = h(12);
    f.ow = p + 16 + h(8) * 2;
    int n = f.last - f.first + 1;
    if (type & 0x0200) {   // fontExtendedFlag: version, densityCount, {density, offset}[]
        int count = h(14);
        uint32_t d = p + 32;
        for (int i = 0; i < count; i++) {
            int dens = (int16_t)rd16(d + i * 8, f.be);
            uint32_t off = rd32(d + i * 8 + 4, f.be);
            if (dens >= f.strikeDensity || !f.strike) { f.strikeDensity = dens; f.strike = p + off; }
        }
        f.loc = d + count * 8;
    } else {
        f.strike = p + 26;
        f.loc = f.strike + (uint32_t)f.rowWords * 2 * f.rectHeight;
    }
    (void)n;
    f.valid = f.strike != 0;
    return f;
}

// FontType* for a FontID: fonts defined by the application, then the ROM table.
uint32_t font_ptr(int id) {
    auto d = defined.find(id);
    if (d != defined.end()) return d->second;
    static std::map<int, uint32_t> rom;
    auto r = rom.find(id);
    if (r != rom.end()) return r->second;
    uint32_t ptr = 0;
    for (Db* db : dm::all()) {   // 'afti' #32000: UInt16 count, then {UInt32 type, UInt16 id, pad}
        for (auto& rec : db->recs) {
            if (rec.type != fourcc("afti") || !rec.h) continue;
            uint32_t t = heap::deref(rec.h);
            if (id < 0 || id >= (int)r16(t)) continue;
            uint32_t rtype = r32(t + 4 + id * 8);
            uint16_t rid = r16(t + 8 + id * 8);
            for (Db* db2 : dm::all())
                for (auto& rec2 : db2->recs)
                    if (rec2.type == rtype && rec2.id == rid && rec2.h) ptr = heap::deref(rec2.h);
        }
    }
    if (!ptr && id != 0) ptr = font_ptr(0);
    rom[id] = ptr;
    return ptr;
}

const Font& cur() { return parse(font_ptr(cur_font)); }

// Glyph index of a character: the offset/width table holds {Int8 offset, Int8 width} byte pairs,
// 0xFFFF for a missing character, which then uses the glyph after lastChar.
int glyph(const Font& f, uint8_t c) {
    if (c < f.first || c > f.last || r16(f.ow + (c - f.first) * 2) == 0xFFFF) return f.last + 1 - f.first;
    return c - f.first;
}
int advance(const Font& f, uint8_t c) { return r8(f.ow + glyph(f, c) * 2 + 1); }   // 1x units
void glyph_span(const Font& f, uint8_t c, int* x0, int* x1) {
    int k = glyph(f, c);
    *x0 = (int16_t)rd16(f.loc + k * 2, f.be);
    *x1 = (int16_t)rd16(f.loc + k * 2 + 2, f.be);
}

int scale(int v1x) { return v1x * win_coord_system() / 72; }

}  // namespace

namespace font {

int width1x(const std::string& s) {
    const Font& f = cur();
    if (!f.valid) return (int)s.size() * 5;
    int w = 0;
    for (uint8_t c : s) w += advance(f, c);
    return w;
}
int height1x() { const Font& f = cur(); return f.valid ? f.rectHeight : 11; }

// Draws text with its top-left corner at native pixel (nx, ny).
void draw(Surface* s, int nx, int ny, const std::string& text, uint16_t fg, uint16_t bg, bool paint_bg) {
    const Font& f = cur();
    if (!s || !f.valid) return;
    const int native = 108, sd = f.strikeDensity;
    int rowBytes = f.rowWords * 2 * sd / 72;
    int h = f.rectHeight * native / 72;
    int pen1x = 0;
    const Rect& c = s->clip;
    for (uint8_t ch : text) {
        int x0, x1;
        glyph_span(f, ch, &x0, &x1);
        int adv = advance(f, ch);
        int gx = nx + pen1x * native / 72, gw = (pen1x + adv) * native / 72 - pen1x * native / 72;
        int iw = (x1 - x0) * native / 72;
        for (int y = 0; y < h; y++) {
            int ty = ny + y;
            if (ty < c.y || ty >= c.y + c.h || ty < 0 || ty >= s->h) continue;
            int sy = y * sd / native;
            uint32_t row = f.strike + (uint32_t)sy * rowBytes;
            for (int x = 0; x < gw; x++) {
                int tx = gx + x;
                if (tx < c.x || tx >= c.x + c.w || tx < 0 || tx >= s->w) continue;
                bool on = false;
                if (x < iw) {
                    int sx = x0 * sd / 72 + x * sd / native;
                    on = (r8(row + (sx >> 3)) >> (7 - (sx & 7))) & 1;
                }
                if (on) *s->px(tx, ty) = fg;
                else if (paint_bg) *s->px(tx, ty) = bg;
            }
        }
        pen1x += adv;
    }
}

}  // namespace font

HLE(FntGetFont) { return cur_font; }
HLE(FntSetFont) { int o = cur_font; cur_font = (int)(a[0] & 0xFF); return o; }
HLE(FntDefineFont) { defined[(int)(a[0] & 0xFF)] = a[1]; cache.erase(a[1]); return 0; }
HLE(FntGetFontPtr) { return font_ptr(cur_font); }
HLE(FntBaseLine) { return scale(cur().ascent); }
HLE(FntCharHeight) { return scale(cur().rectHeight); }
HLE(FntLineHeight) { return scale(cur().rectHeight + cur().leading); }
HLE(FntDescenderHeight) { return scale(cur().descent); }
HLE(FntAverageCharWidth) { return scale(cur().maxWidth); }
HLE(FntCharWidth) { return scale(advance(cur(), (uint8_t)a[0])); }
HLE(FntWCharWidth) { return scale(advance(cur(), (uint8_t)a[0])); }
HLE(FntCharsWidth) { return scale(font::width1x(rstr(a[0]).substr(0, (int16_t)a[1] < 0 ? 0 : (size_t)(int16_t)a[1]))); }
HLE(FntLineWidth) { return scale(font::width1x(rstr(a[0]).substr(0, (int16_t)a[1] < 0 ? 0 : (size_t)(int16_t)a[1]))); }
// FntCharsInWidth(string, Int16* stringWidthP, Int16* stringLengthP, Boolean* fitWithinWidth)
HLE(FntCharsInWidth) {
    std::string s = rstr(a[0]).substr(0, (size_t)(int16_t)r16(a[2]));
    int maxw = (int16_t)r16(a[1]), w = 0;
    size_t n = 0;
    for (; n < s.size(); n++) {
        int cw = scale(font::width1x(s.substr(0, n + 1)));
        if (cw > maxw) break;
        w = cw;
    }
    while (n > 0 && s[n - 1] == ' ') { n--; w = scale(font::width1x(s.substr(0, n))); }
    w16(a[1], (uint16_t)w);
    w16(a[2], (uint16_t)n);
    if (a[3]) w8(a[3], n == s.size());
    return 0;
}
HLE(FntWidthToOffset) {
    // FntWidthToOffset(chars, length, pixelWidth, Boolean* leadingEdge, Int16* truncWidth)
    std::string s = rstr(a[0]).substr(0, (size_t)(uint16_t)a[1]);
    int target = (int16_t)a[2];
    size_t n = 0;
    int w = 0;
    for (; n < s.size(); n++) {
        int next = scale(font::width1x(s.substr(0, n + 1)));
        if (next > target) break;
        w = next;
    }
    if (a[3]) w8(a[3], 1);
    if (a[4]) w16(a[4], (uint16_t)w);
    return (uint32_t)n;
}
HLE(FntWordWrap) {
    // UInt16 FntWordWrap(const Char* chars, UInt16 maxWidth): characters that fit on one line
    std::string s = rstr(a[0]);
    int maxw = (uint16_t)a[1];
    size_t n = 0, lastBreak = 0;
    for (; n < s.size(); n++) {
        if (s[n] == '\n') return (uint32_t)n + 1;
        if (scale(font::width1x(s.substr(0, n + 1))) > maxw) break;
        if (s[n] == ' ') lastBreak = n + 1;
    }
    if (n == s.size()) return (uint32_t)n;
    return (uint32_t)(lastBreak ? lastBreak : (n ? n : 1));
}
