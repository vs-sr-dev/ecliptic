// Palm OS Window Manager: draw state (colours, draw window) and basic drawing onto the
// display surface. Coordinates follow the active coordinate system (WinSetCoordinateSystem).
#include "font.h"
#include "gfx.h"
#include "palmos.h"
#include <vector>

uint16_t win_coord_system();     // sys.cpp
uint32_t ui_display_window();    // evt.cpp
namespace gfx { uint32_t palm_index_rgb(int index); }   // bitmap.cpp

namespace {

struct DrawState {
    uint8_t fore = 0xFF, back = 0x00, text = 0xFF;   // indexes in the system palette
    uint16_t drawMode = 0, underline = 0, font = 0;
};
DrawState st;
std::vector<DrawState> stack;
uint32_t draw_window = 0;

// Standard/native coordinates -> display pixels.
int px(int v) { int c = win_coord_system(); return c == 108 ? v : v * 108 / c; }

uint16_t color565(uint8_t index) { return gfx::packed_to_565(gfx::palm_index_rgb(index)); }

uint8_t rgb_to_index(uint8_t r, uint8_t g, uint8_t b) {
    int best = 0, bestd = 1 << 30;
    for (int i = 0; i < 256; i++) {
        uint32_t c = gfx::palm_index_rgb(i);
        int dr = (int)(c >> 16 & 0xFF) - r, dg = (int)(c >> 8 & 0xFF) - g, db = (int)(c & 0xFF) - b;
        int d = dr * dr + dg * dg + db * db;
        if (d < bestd) { bestd = d; best = i; }
    }
    return (uint8_t)best;
}

// RGBColorType: index, r, g, b
uint8_t set_rgb(uint32_t newP, uint32_t oldP, uint8_t& slot) {
    if (oldP) { uint32_t c = gfx::palm_index_rgb(slot); w8(oldP, slot); w8(oldP + 1, c >> 16); w8(oldP + 2, c >> 8); w8(oldP + 3, c); }
    if (newP) slot = rgb_to_index(r8(newP + 1), r8(newP + 2), r8(newP + 3));
    return 0;
}

Surface* target() { return gfx::palm_display(); }

void fill(int x, int y, int w, int h, uint16_t c) {
    Surface* s = target();
    if (s) gfx::fill_rect(s, Rect{px(x), px(y), px(x + w) - px(x), px(y + h) - px(y)}, c);
    if (s && s->display) gfx::frame_tick();
}

}  // namespace

HLE(WinSetForeColor) { uint8_t o = st.fore; st.fore = (uint8_t)a[0]; return o; }
HLE(WinSetBackColor) { uint8_t o = st.back; st.back = (uint8_t)a[0]; return o; }
HLE(WinSetTextColor) { uint8_t o = st.text; st.text = (uint8_t)a[0]; return o; }
HLE(WinSetForeColorRGB) { return set_rgb(a[0], a[1], st.fore); }
HLE(WinSetBackColorRGB) { return set_rgb(a[0], a[1], st.back); }
HLE(WinSetTextColorRGB) { return set_rgb(a[0], a[1], st.text); }
HLE(WinSetDrawMode) { uint16_t o = st.drawMode; st.drawMode = (uint16_t)a[0]; return o; }
HLE(WinSetUnderlineMode) { uint16_t o = st.underline; st.underline = (uint16_t)a[0]; return o; }
HLE(WinPushDrawState) { stack.push_back(st); return 0; }
HLE(WinPopDrawState) { if (!stack.empty()) { st = stack.back(); stack.pop_back(); } return 0; }
HLE(WinIndexToRGB) {
    uint32_t c = gfx::palm_index_rgb((uint8_t)a[0]);
    if (a[1]) { w8(a[1], (uint8_t)a[0]); w8(a[1] + 1, c >> 16); w8(a[1] + 2, c >> 8); w8(a[1] + 3, c); }
    return 0;
}
HLE(WinRGBToIndex) { return a[0] ? rgb_to_index(r8(a[0] + 1), r8(a[0] + 2), r8(a[0] + 3)) : 0; }

HLE(WinGetDrawWindow) { return draw_window ? draw_window : ui_display_window(); }
HLE(WinSetDrawWindow) {
    uint32_t o = draw_window ? draw_window : ui_display_window();
    draw_window = a[0];
    return o;
}

// RectangleType: Int16 x, y, w, h
HLE(WinPaintRectangle) {
    int16_t x = (int16_t)r16(a[0]), y = (int16_t)r16(a[0] + 2), w = (int16_t)r16(a[0] + 4), h = (int16_t)r16(a[0] + 6);
    fill(x, y, w, h, color565(st.fore));
    return 0;
}
HLE(x_WinEraseWindow) {
    Surface* s = target();
    if (s) { gfx::fill_rect(s, Rect{0, 0, s->w, s->h}, color565(st.back)); if (s->display) gfx::frame_tick(); }
    return 0;
}

// Text: winPaint fills the character cells with the back colour, winOverlay does not;
// winPaintInverse swaps the colours.
static void paint_text(const std::string& txt, int x, int y) {
    Surface* s = target();
    if (!s) return;
    uint16_t fg = color565(st.text), bg = color565(st.back);
    bool cells = st.drawMode == 0 || st.drawMode == 5;
    if (st.drawMode == 5) std::swap(fg, bg);
    font::draw(s, px(x), px(y), txt, fg, bg, cells);
    if (s->display) gfx::frame_tick();
}
HLE(WinPaintChars) {
    int16_t len = (int16_t)a[1];
    paint_text(len > 0 ? rstr(a[0], (size_t)len) : std::string(), (int16_t)a[2], (int16_t)a[3]);
    return 0;
}
HLE(WinPaintChar) { paint_text(std::string(1, (char)(a[0] & 0xFF)), (int16_t)a[1], (int16_t)a[2]); return 0; }

uint8_t win_text_color() { return st.text; }
uint8_t win_back_color() { return st.back; }
