// TwGfx HLE: 2D RGB565 accelerator (ATI Imageon W4200) reimplemented in software.
#include "gfx.h"
#include "host.h"
#include "palmos.h"
#include <algorithm>
#include <map>
#include <vector>

using namespace layout;

constexpr uint32_t twGfxErrorBase = 0x7000 + 0x100;
constexpr uint32_t twGfxErrorNullPointer = twGfxErrorBase + 1, twGfxErrorSurfaceAllocFailed = twGfxErrorBase + 3,
                   twGfxErrorInvalidHandle = twGfxErrorBase + 10, twGfxErrorInvalidPixelFormat = twGfxErrorBase + 11,
                   twGfxErrorInvalidSize = twGfxErrorBase + 13, twGfxErrorInvalidCoord = twGfxErrorBase + 18;
constexpr uint32_t FB_BYTES = SCREEN_W * SCREEN_H * 2;

static std::map<uint32_t, Surface> surfaces;      // by handle
static std::map<uint32_t, uint32_t> vram_used;    // addr -> size
static int g_width = SCREEN_W, g_height = SCREEN_H;
static Surface* g_palm_display = nullptr;
static uint32_t g_last_frame = 0;

namespace gfx {

uint16_t packed_to_565(uint32_t c) {
    return (uint16_t)(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
}
uint32_t rgb565_to_packed(uint16_t c) {
    uint32_t r = (c >> 8) & 0xF8, g = (c >> 3) & 0xFC, b = (c << 3) & 0xF8;
    return r << 16 | g << 8 | b;
}

uint32_t vram_alloc(uint32_t bytes) {
    bytes = (bytes + 31) & ~31u;
    uint32_t a = VRAM_BASE + FB_BYTES * 2;   // framebuffer + Palm back buffer reserved
    for (auto& [addr, sz] : vram_used) {
        if (addr >= a + bytes) break;
        a = std::max(a, (addr + sz + 31) & ~31u);
    }
    if (a + bytes > VRAM_BASE + VRAM_SIZE) return 0;
    vram_used[a] = bytes;
    return a;
}
void vram_free(uint32_t addr) { vram_used.erase(addr); }

static Surface& make_surface(uint32_t gfxh, uint32_t addr, int w, int h, bool display) {
    uint32_t handle = heap::alloc(16, true);
    Surface& s = surfaces[handle];
    s.handle = handle; s.gfx = gfxh; s.addr = addr; s.w = w; s.h = h; s.rowBytes = w * 2;
    s.clip = {0, 0, w, h};
    s.display = display;
    s.owns_memory = !display;
    return s;
}

uint32_t alloc_surface(uint32_t gfxh, int w, int h) {
    uint32_t addr = vram_alloc((uint32_t)(w * h * 2));
    if (!addr) return 0;
    mem_fill(addr, 0, w * h * 2);
    return make_surface(gfxh, addr, w, h, false).handle;
}

void init() {
    g_palm_display = &make_surface(0, VRAM_BASE, g_width, g_height, true);
    host_set_screen_addr(VRAM_BASE);
}
Surface* surface(uint32_t h) { auto it = surfaces.find(h); return it == surfaces.end() ? nullptr : &it->second; }
Surface* palm_display() { return g_palm_display; }

void orientation_changed(int o) {
    bool portrait = (o == 1 || o == 3);
    g_width = portrait ? SCREEN_H : SCREEN_W;
    g_height = portrait ? SCREEN_W : SCREEN_H;
    for (auto& [h, s] : surfaces)
        if (s.display) { s.w = g_width; s.h = g_height; s.rowBytes = g_width * 2; s.clip = {0, 0, s.w, s.h}; }
    host_set_portrait(portrait);
}

void debug_dump_surfaces() {
    for (auto& [h, s] : surfaces) {
        size_t nz = 0;
        for (int y = 0; y < s.h; y++) for (int x = 0; x < s.w; x++) nz += *s.px(x, y) != 0;
        LOG("surface %08x %dx%d at %08x%s: %zu non-black pixels", h, s.w, s.h, s.addr, s.display ? " (display)" : "", nz);
    }
}

void frame_tick() {
    uint32_t t = now_ms();
    if (t - g_last_frame >= 16) { g_last_frame = t; host_present(); }
}

static bool intersect(Rect& r, const Rect& c) {
    int x0 = std::max(r.x, c.x), y0 = std::max(r.y, c.y);
    int x1 = std::min(r.x + r.w, c.x + c.w), y1 = std::min(r.y + r.h, c.y + c.h);
    if (x1 <= x0 || y1 <= y0) { r.w = r.h = 0; return false; }
    r = {x0, y0, x1 - x0, y1 - y0};
    return true;
}

void fill_rect(Surface* s, Rect r, uint16_t c) {
    if (!intersect(r, s->clip) || !intersect(r, {0, 0, s->w, s->h})) return;
    for (int y = r.y; y < r.y + r.h; y++) {
        uint16_t* p = s->px(r.x, y);
        for (int x = 0; x < r.w; x++) p[x] = c;
    }
}

static inline void plot(Surface* s, int x, int y, uint16_t c) {
    const Rect& k = s->clip;
    if (x < k.x || y < k.y || x >= k.x + k.w || y >= k.y + k.h || x < 0 || y < 0 || x >= s->w || y >= s->h) return;
    *s->px(x, y) = c;
}

static void line(Surface* s, int x0, int y0, int x1, int y1, uint16_t c) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1, dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1, err = dx + dy;
    for (;;) {
        plot(s, x0, y0, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static inline uint16_t blend565(uint16_t s, uint16_t d, int ar, int ag, int ab) {   // alpha 0..256
    int sr = s >> 11, sg = (s >> 5) & 63, sb = s & 31, dr = d >> 11, dg = (d >> 5) & 63, db = d & 31;
    int r = dr + (((sr - dr) * ar) >> 8), g = dg + (((sg - dg) * ag) >> 8), b = db + (((sb - db) * ab) >> 8);
    return (uint16_t)(r << 11 | g << 5 | b);
}

// Generic clipped blit. op(dstPixel*, srcPixel, sx, sy) with sx/sy relative to the source rect.
template <class Op>
static uint32_t blit(Surface* d, int dx, int dy, Surface* s, Rect sr, Op op) {
    Rect r{dx, dy, sr.w, sr.h};
    Rect srcBounds{0, 0, s->w, s->h};
    // clip source rect to the source surface, shifting the destination accordingly
    Rect sc = sr;
    intersect(sc, srcBounds);
    if (!sc.w) return 0;
    r.x += sc.x - sr.x; r.y += sc.y - sr.y; r.w = sc.w; r.h = sc.h;
    Rect rc = r;
    if (!intersect(rc, d->clip) || !intersect(rc, {0, 0, d->w, d->h})) return 0;
    int ox = sc.x + (rc.x - r.x), oy = sc.y + (rc.y - r.y);   // source origin of clipped area
    std::vector<uint16_t> tmp;
    bool same = d->addr == s->addr;
    if (same) {
        tmp.resize((size_t)rc.w * rc.h);
        for (int y = 0; y < rc.h; y++) memcpy(&tmp[(size_t)y * rc.w], s->px(ox, oy + y), rc.w * 2);
    }
    for (int y = 0; y < rc.h; y++) {
        uint16_t* dp = d->px(rc.x, rc.y + y);
        const uint16_t* sp = same ? &tmp[(size_t)y * rc.w] : s->px(ox, oy + y);
        int relx = ox - sr.x, rely = oy - sr.y + y;
        for (int x = 0; x < rc.w; x++) op(dp + x, sp[x], relx + x, rely);
    }
    return 0;
}

static Rect rd_rect(uint32_t p) { return {(int)r32(p), (int)r32(p + 4), (int)r32(p + 8), (int)r32(p + 12)}; }

}  // namespace gfx

using namespace gfx;

// ---- library / info --------------------------------------------------------
static void fill_info(uint32_t p) {
    if (!p) return;
    w32(p + 4, g_width); w32(p + 8, g_height); w32(p + 12, g_width * 2); w32(p + 16, 1);
    uint32_t used = 0;
    for (auto& [a, s] : vram_used) used += s;
    w32(p + 20, VRAM_SIZE - FB_BYTES * 2 - used);
    w32(p + 24, VRAM_SIZE);
}
HLE(TwGfxOpen) {
    uint32_t h = heap::alloc(32, true);
    w32(a[0], h);
    fill_info(a[1]);
    return 0;
}
HLE(TwGfxClose) {
    for (auto it = surfaces.begin(); it != surfaces.end();) {
        if (it->second.gfx == a[0] && it->second.gfx) {
            if (it->second.owns_memory) vram_free(it->second.addr);
            heap::free(it->first);
            it = surfaces.erase(it);
        } else ++it;
    }
    return 0;
}
HLE(TwGfxGetInfo) { fill_info(a[1]); return 0; }
HLE(TwGfxGetMemoryUsage) {
    uint32_t used = 0;
    for (auto& [ad, s] : vram_used) used += s;
    if (a[2]) w32(a[2], used + FB_BYTES * 2);
    return 0;
}
static uint32_t get_display(uint32_t gfxh, uint32_t out) {
    Surface& s = make_surface(gfxh, VRAM_BASE, g_width, g_height, true);
    w32(out, s.handle);
    return 0;
}
HLE(TwGfxGetDisplaySurface) { return get_display(a[0], a[1]); }
HLE(TwGfxGetPalmDisplaySurface) { return get_display(a[0], a[1]); }
HLE(TwGfxInVBlank) {
    if (a[1]) w8(a[1], (now_ms() % 16) < 2);
    return 0;
}
HLE(TwGfxWaitForVBlank) {
    uint32_t t = now_ms(), next = (t / 16 + 1) * 16;
    host_sleep(next - t);
    host_present();
    return 0;
}

// ---- surfaces ----------------------------------------------------------------
HLE(TwGfxAllocSurface) {
    uint32_t desc = a[2];
    int w = (int)r32(desc + 4), h = (int)r32(desc + 8), loc = (int)r32(desc + 16), fmt = (int)r32(desc + 20);
    if (fmt != 1) { LOG("TwGfxAllocSurface: unsupported pixel format %d", fmt); return twGfxErrorInvalidPixelFormat; }
    if (w <= 0 || h <= 0) return twGfxErrorInvalidSize;
    uint32_t addr = vram_alloc((uint32_t)(w * h * 2));
    if (!addr) {
        LOG("TwGfxAllocSurface(%dx%d): out of accelerator memory", w, h);
        return twGfxErrorSurfaceAllocFailed;
    }
    mem_fill(addr, 0, w * h * 2);
    Surface& s = make_surface(a[0], addr, w, h, false);
    s.location = loc;
    w32(desc + 12, s.rowBytes);
    w32(a[1], s.handle);
    return 0;
}
HLE(TwGfxFreeSurface) {
    Surface* s = surface(a[0]);
    if (!s) return twGfxErrorInvalidHandle;
    if (s->owns_memory) vram_free(s->addr);
    heap::free(a[0]);
    surfaces.erase(a[0]);
    return 0;
}
HLE(TwGfxSetClip) {
    Surface* s = surface(a[0]);
    if (!s) return twGfxErrorInvalidHandle;
    if (!a[1]) s->clip = {0, 0, s->w, s->h};
    else { s->clip = rd_rect(a[1]); intersect(s->clip, {0, 0, s->w, s->h}); }
    return 0;
}
HLE(TwGfxGetClip) {
    Surface* s = surface(a[0]);
    if (!s) return twGfxErrorInvalidHandle;
    w32(a[1], s->clip.x); w32(a[1] + 4, s->clip.y); w32(a[1] + 8, s->clip.w); w32(a[1] + 12, s->clip.h);
    return 0;
}
HLE(TwGfxGetSurfaceInfo) {
    Surface* s = surface(a[0]);
    if (!s) return twGfxErrorInvalidHandle;
    w32(a[1] + 4, s->w); w32(a[1] + 8, s->h); w32(a[1] + 12, s->rowBytes);
    w32(a[1] + 16, s->location); w32(a[1] + 20, s->format);
    return 0;
}
HLE(TwGfxLockSurface) {
    Surface* s = surface(a[0]);
    if (!s) return twGfxErrorInvalidHandle;
    s->locks++;
    w32(a[1], s->addr);
    return 0;
}
HLE(TwGfxUnlockSurface) {
    Surface* s = surface(a[0]);
    if (!s) return twGfxErrorInvalidHandle;
    if (s->locks) s->locks--;
    if (s->display) frame_tick();
    return 0;
}
HLE(TwGfxIsSurfaceReady) { return 0; }
HLE(TwGfxReadSurface) {
    Surface* s = surface(a[0]);
    if (!s) return twGfxErrorInvalidHandle;
    for (int y = 0; y < s->h; y++) mem_write(a[1] + y * s->w * 2, s->px(0, y), s->w * 2);
    return 0;
}
HLE(TwGfxWriteSurface) {
    Surface* s = surface(a[0]);
    if (!s) return twGfxErrorInvalidHandle;
    for (int y = 0; y < s->h; y++) mem_read(a[1] + y * s->w * 2, s->px(0, y), s->w * 2);
    if (s->display) frame_tick();
    return 0;
}
HLE(TwGfxReadSurfaceRegion) {
    Surface* s = surface(a[0]);
    if (!s) return twGfxErrorInvalidHandle;
    Rect r = rd_rect(a[1]);
    if (!intersect(r, {0, 0, s->w, s->h})) return twGfxErrorInvalidCoord;
    for (int y = 0; y < r.h; y++) mem_write(a[2] + y * a[3], s->px(r.x, r.y + y), r.w * 2);
    return 0;
}
HLE(TwGfxWriteSurfaceRegion) {
    Surface* s = surface(a[0]);
    if (!s) return twGfxErrorInvalidHandle;
    Rect r = rd_rect(a[1]), o = r;
    if (!intersect(r, {0, 0, s->w, s->h})) return twGfxErrorInvalidCoord;
    for (int y = 0; y < r.h; y++)
        mem_read(a[2] + (r.y - o.y + y) * a[3] + (r.x - o.x) * 2, s->px(r.x, r.y + y), r.w * 2);
    if (s->display) frame_tick();
    return 0;
}

// ---- blits -------------------------------------------------------------------
#define BLIT_PROLOGUE                                                    \
    Surface* d = surface(a[0]);                                          \
    Surface* s = surface(a[2]);                                          \
    if (!d || !s) return twGfxErrorInvalidHandle;                        \
    if (!a[1] || !a[3]) return twGfxErrorNullPointer;                    \
    int dx = (int)r32(a[1]), dy = (int)r32(a[1] + 4);                    \
    Rect sr = rd_rect(a[3]);

HLE(TwGfxBitBlt) {
    BLIT_PROLOGUE
    blit(d, dx, dy, s, sr, [](uint16_t* dp, uint16_t c, int, int) { *dp = c; });
    return 0;
}
HLE(TwGfxAsyncBlt) {
    BLIT_PROLOGUE
    blit(d, dx, dy, s, sr, [](uint16_t* dp, uint16_t c, int, int) { *dp = c; });
    return 0;
}
HLE(TwGfxTransparentBlt) {
    BLIT_PROLOGUE
    uint16_t key = packed_to_565(a[4]);
    blit(d, dx, dy, s, sr, [key](uint16_t* dp, uint16_t c, int, int) { if (c != key) *dp = c; });
    return 0;
}
HLE(TwGfxBlendBlt) {
    BLIT_PROLOGUE
    uint32_t al = a[4];
    int ar = ((al >> 16) & 0xFF), ag = ((al >> 8) & 0xFF), ab = (al & 0xFF);
    ar += ar >> 7; ag += ag >> 7; ab += ab >> 7;   // 255 -> 256
    blit(d, dx, dy, s, sr, [=](uint16_t* dp, uint16_t c, int, int) { *dp = blend565(c, *dp, ar, ag, ab); });
    return 0;
}

struct MaskBmp { int w, h, rowBytes, fmt; uint32_t data; };
static MaskBmp rd_bmp(uint32_t p) { return {(int)r32(p + 4), (int)r32(p + 8), (int)r32(p + 12), (int)r32(p + 16), r32(p + 20)}; }
static int bmp_index(const MaskBmp& m, int x, int y) {
    int bpp = m.fmt == 0 ? 1 : m.fmt == 3 ? 2 : m.fmt == 4 ? 4 : 8;
    uint32_t bit = (uint32_t)x * bpp;
    uint8_t byte = r8(m.data + y * m.rowBytes + (bit >> 3));
    int shift = 8 - bpp - (bit & 7);
    return (byte >> shift) & ((1 << bpp) - 1);
}
HLE(TwGfxMaskBlt) {
    BLIT_PROLOGUE
    MaskBmp m = rd_bmp(a[4]);
    blit(d, dx, dy, s, sr, [&](uint16_t* dp, uint16_t c, int x, int y) {
        if (x < m.w && y < m.h && bmp_index(m, x, y)) *dp = c;
    });
    return 0;
}
HLE(TwGfxMaskBlendBlt) {
    BLIT_PROLOGUE
    MaskBmp m = rd_bmp(a[4]);
    int maxv = m.fmt == 0 ? 1 : m.fmt == 3 ? 3 : m.fmt == 4 ? 15 : 255;
    blit(d, dx, dy, s, sr, [&](uint16_t* dp, uint16_t c, int x, int y) {
        if (x >= m.w || y >= m.h) return;
        int al = bmp_index(m, x, y) * 256 / maxv;
        if (al >= 256) *dp = c;
        else if (al > 0) *dp = blend565(c, *dp, al, al, al);
    });
    return 0;
}
static void stretch(Surface* d, Rect dr, Surface* s, Rect sr, bool smooth) {
    if (dr.w <= 0 || dr.h <= 0 || sr.w <= 0 || sr.h <= 0) return;
    Rect rc = dr;
    if (!intersect(rc, d->clip) || !intersect(rc, {0, 0, d->w, d->h})) return;
    for (int y = rc.y; y < rc.y + rc.h; y++) {
        int sy = sr.y + (int)((int64_t)(y - dr.y) * sr.h / dr.h);
        if (sy < 0 || sy >= s->h) continue;
        uint16_t* dp = d->px(rc.x, y);
        for (int x = rc.x; x < rc.x + rc.w; x++) {
            int sx = sr.x + (int)((int64_t)(x - dr.x) * sr.w / dr.w);
            if (sx >= 0 && sx < s->w) dp[x - rc.x] = *s->px(sx, sy);
        }
    }
}
HLE(TwGfxStretchBlt) {
    Surface* d = surface(a[0]); Surface* s = surface(a[2]);
    if (!d || !s) return twGfxErrorInvalidHandle;
    stretch(d, rd_rect(a[1]), s, rd_rect(a[3]), false);
    return 0;
}
HLE(TwGfxStretchBlt2) {
    Surface* d = surface(a[0]); Surface* s = surface(a[2]);
    if (!d || !s) return twGfxErrorInvalidHandle;
    stretch(d, rd_rect(a[1]), s, rd_rect(a[3]), a[4] & 4);
    return 0;
}
HLE(TwGfxTileBlt) {
    Surface* d = surface(a[0]); Surface* s = surface(a[2]);
    if (!d || !s) return twGfxErrorInvalidHandle;
    Rect dr = rd_rect(a[1]), rc = dr;
    int ax = a[3] ? (int)r32(a[3]) : 0, ay = a[3] ? (int)r32(a[3] + 4) : 0;
    if (!intersect(rc, d->clip) || !intersect(rc, {0, 0, d->w, d->h})) return 0;
    for (int y = rc.y; y < rc.y + rc.h; y++) {
        int sy = ((y - dr.y + ay) % s->h + s->h) % s->h;
        uint16_t* dp = d->px(rc.x, y);
        for (int x = rc.x; x < rc.x + rc.w; x++) {
            int sx = ((x - dr.x + ax) % s->w + s->w) % s->w;
            dp[x - rc.x] = *s->px(sx, sy);
        }
    }
    return 0;
}
HLE(TwGfxTransformBlt) {
    BLIT_PROLOGUE
    int rot = (int)a[4] & 3, mir = (int)a[5] & 3;
    Rect sc = sr;
    intersect(sc, {0, 0, s->w, s->h});
    bool swap = rot == 1 || rot == 3;
    int ow = swap ? sc.h : sc.w, oh = swap ? sc.w : sc.h;
    for (int v = 0; v < oh; v++)
        for (int u = 0; u < ow; u++) {
            int x, y;   // coordinates inside the source rect
            switch (rot) {
                case 1: x = v; y = sc.h - 1 - u; break;            // CW90
                case 2: x = sc.w - 1 - u; y = sc.h - 1 - v; break;
                case 3: x = sc.w - 1 - v; y = u; break;            // CW270
                default: x = u; y = v;
            }
            if (mir & 1) x = sc.w - 1 - x;
            if (mir & 2) y = sc.h - 1 - y;
            plot(d, dx + u, dy + v, *s->px(sc.x + x, sc.y + y));
        }
    return 0;
}

// ---- primitives ----------------------------------------------------------------
HLE(TwGfxDrawPoints) {
    Surface* d = surface(a[0]);
    if (!d) return twGfxErrorInvalidHandle;
    uint16_t c = packed_to_565(a[3]);
    for (uint32_t i = 0; i < a[2]; i++) plot(d, (int)r32(a[1] + i * 8), (int)r32(a[1] + i * 8 + 4), c);
    return 0;
}
HLE(TwGfxDrawColorPoints) {
    Surface* d = surface(a[0]);
    if (!d) return twGfxErrorInvalidHandle;
    for (uint32_t i = 0; i < a[2]; i++)
        plot(d, (int)r32(a[1] + i * 8), (int)r32(a[1] + i * 8 + 4), packed_to_565(r32(a[3] + i * 4)));
    return 0;
}
HLE(TwGfxDrawLines) {
    Surface* d = surface(a[0]);
    if (!d) return twGfxErrorInvalidHandle;
    uint16_t c = packed_to_565(a[3]);
    for (uint32_t i = 0; i + 1 < a[2]; i++)
        line(d, (int)r32(a[1] + i * 8), (int)r32(a[1] + i * 8 + 4), (int)r32(a[1] + i * 8 + 8), (int)r32(a[1] + i * 8 + 12), c);
    return 0;
}
HLE(TwGfxDrawLineSegments) {
    Surface* d = surface(a[0]);
    if (!d) return twGfxErrorInvalidHandle;
    uint16_t c = packed_to_565(a[3]);
    for (uint32_t i = 0; i + 1 < a[2]; i += 2)
        line(d, (int)r32(a[1] + i * 8), (int)r32(a[1] + i * 8 + 4), (int)r32(a[1] + i * 8 + 8), (int)r32(a[1] + i * 8 + 12), c);
    return 0;
}
HLE(TwGfxDrawRect) {
    Surface* d = surface(a[0]);
    if (!d) return twGfxErrorInvalidHandle;
    Rect r = rd_rect(a[1]);
    uint16_t c = packed_to_565(a[2]);
    if (r.w <= 0 || r.h <= 0) return 0;
    fill_rect(d, {r.x, r.y, r.w, 1}, c);
    fill_rect(d, {r.x, r.y + r.h - 1, r.w, 1}, c);
    fill_rect(d, {r.x, r.y, 1, r.h}, c);
    fill_rect(d, {r.x + r.w - 1, r.y, 1, r.h}, c);
    return 0;
}
HLE(TwGfxFillRect) {
    Surface* d = surface(a[0]);
    if (!d) return twGfxErrorInvalidHandle;
    fill_rect(d, rd_rect(a[1]), packed_to_565(a[2]));
    return 0;
}
HLE(TwGfxDrawSpans) {
    Surface* d = surface(a[0]);
    if (!d) return twGfxErrorInvalidHandle;
    uint16_t c = packed_to_565(a[3]);
    for (uint32_t i = 0; i < a[2]; i++)
        fill_rect(d, {(int)r32(a[1] + i * 12), (int)r32(a[1] + i * 12 + 4), (int)r32(a[1] + i * 12 + 8), 1}, c);
    return 0;
}
HLE(TwGfxDrawBitmap) {
    Surface* d = surface(a[0]);
    if (!d) return twGfxErrorInvalidHandle;
    if (!a[1] || !a[2]) return twGfxErrorNullPointer;
    int dx = (int)r32(a[1]), dy = (int)r32(a[1] + 4);
    MaskBmp m = rd_bmp(a[2]);
    uint32_t pal = r32(a[2] + 24);
    for (int y = 0; y < m.h; y++)
        for (int x = 0; x < m.w; x++) {
            uint16_t c;
            if (m.fmt == 1) c = r16(m.data + y * m.rowBytes + x * 2);
            else if (m.fmt == 2) { uint16_t v = r16(m.data + y * m.rowBytes + x * 2); c = (uint16_t)(v >> 8 | v << 8); }
            else c = pal ? r16(pal + bmp_index(m, x, y) * 2) : 0;
            plot(d, dx + x, dy + y, c);
        }
    return 0;
}
HLE(TwGfxDrawPalmBitmap) {
    Surface* d = surface(a[0]);
    if (!d) return twGfxErrorInvalidHandle;
    if (!a[1] || !a[2]) return twGfxErrorNullPointer;
    draw_palm_bitmap(d, (int)r32(a[1]), (int)r32(a[1] + 4), a[2]);
    return 0;
}
