// TwJpg HLE: JPEG decoding (the device uses IJG libjpeg; we use SDL2_image).
#include "gfx.h"
#include "palmos.h"
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <map>
#include <vector>

constexpr uint32_t twJpgErrorBase = 0x7000 + 0x300;
constexpr uint32_t twJpgErrorNullPointer = twJpgErrorBase + 1, twJpgErrorBadImage = twJpgErrorBase + 4,
                   twJpgErrorInvalidHandle = twJpgErrorBase + 10, twJpgErrorInvalidPixelFormat = twJpgErrorBase + 11;

struct Jpg {
    SDL_Surface* rgb = nullptr;
    uint32_t abort_fn = 0, abort_handle = 0;
};
static std::map<uint32_t, Jpg> g_jpgs;

static Jpg* get(uint32_t h) {
    auto it = g_jpgs.find(h);
    return it == g_jpgs.end() ? nullptr : &it->second;
}

// Sample the RGB24 image at (x, y) of a w*h output grid (nearest or bilinear).
static void sample(SDL_Surface* s, int x, int y, int w, int h, bool aa, uint8_t* rgb) {
    const uint8_t* px = (const uint8_t*)s->pixels;
    if (!aa || (w == s->w && h == s->h)) {
        int sx = (int)((int64_t)x * s->w / w), sy = (int)((int64_t)y * s->h / h);
        memcpy(rgb, px + sy * s->pitch + sx * 3, 3);
        return;
    }
    float fx = (x + 0.5f) * s->w / w - 0.5f, fy = (y + 0.5f) * s->h / h - 0.5f;
    int x0 = fx < 0 ? 0 : (int)fx, y0 = fy < 0 ? 0 : (int)fy;
    int x1 = x0 + 1 < s->w ? x0 + 1 : x0, y1 = y0 + 1 < s->h ? y0 + 1 : y0;
    float ax = fx - x0, ay = fy - y0;
    if (ax < 0) ax = 0;
    if (ay < 0) ay = 0;
    for (int c = 0; c < 3; c++) {
        float a0 = px[y0 * s->pitch + x0 * 3 + c] * (1 - ax) + px[y0 * s->pitch + x1 * 3 + c] * ax;
        float a1 = px[y1 * s->pitch + x0 * 3 + c] * (1 - ax) + px[y1 * s->pitch + x1 * 3 + c] * ax;
        rgb[c] = (uint8_t)(a0 * (1 - ay) + a1 * ay + 0.5f);
    }
}
static uint16_t to565(const uint8_t* c) { return (uint16_t)((c[0] & 0xF8) << 8 | (c[1] & 0xFC) << 3 | c[2] >> 3); }

// Writes w*h pixels in the requested format (1 = RGB565 LE, 2 = RGB565 BE, 3 = RGB888) to a new MemPtr.
static uint32_t render(SDL_Surface* s, int w, int h, bool aa, int fmt) {
    int bpp = fmt == 3 ? 3 : 2;
    uint32_t out = heap::alloc((uint32_t)(w * h * bpp));
    if (!out) return 0;
    uint8_t* dst = host_ptr(out, w * h * bpp);
    uint8_t c[3];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            sample(s, x, y, w, h, aa, c);
            uint8_t* d = dst + (y * w + x) * bpp;
            if (fmt == 3) {
                memcpy(d, c, 3);
                continue;
            }
            uint16_t v = to565(c);
            if (fmt == 2) v = (uint16_t)(v >> 8 | v << 8);
            memcpy(d, &v, 2);
        }
    return out;
}

HLE(TwJpgOpenImage) {
    if (!a[0] || !a[1]) return twJpgErrorNullPointer;
    // Pull the whole stream through the reader callback of the game.
    std::vector<uint8_t> data;
    uint32_t buf = heap::alloc(4096);
    for (int guard = 0; guard < 65536; guard++) {
        int32_t n = (int32_t)cpu_call(a[1], {a[2], buf, 4096});
        if (n <= 0) break;
        if (n > 4096) n = 4096;
        uint8_t* p = host_ptr(buf, (uint32_t)n);
        data.insert(data.end(), p, p + n);
        if (g.quit) break;
    }
    heap::free(buf);
    SDL_RWops* rw = SDL_RWFromConstMem(data.data(), (int)data.size());
    SDL_Surface* img = rw ? IMG_LoadJPG_RW(rw) : nullptr;
    if (rw) SDL_RWclose(rw);
    if (!img) {
        LOG("TwJpgOpenImage: decode failed (%zu bytes): %s", data.size(), IMG_GetError());
        return twJpgErrorBadImage;
    }
    SDL_Surface* rgb = SDL_ConvertSurfaceFormat(img, SDL_PIXELFORMAT_RGB24, 0);
    SDL_FreeSurface(img);
    uint32_t h = heap::alloc(16, true);
    g_jpgs[h].rgb = rgb;
    w32(a[0], h);
    return 0;
}
HLE(TwJpgCloseImage) {
    Jpg* j = get(a[0]);
    if (!j) return twJpgErrorInvalidHandle;
    SDL_FreeSurface(j->rgb);
    g_jpgs.erase(a[0]);
    heap::free(a[0]);
    return 0;
}
HLE(TwJpgSetAbortCheck) {
    Jpg* j = get(a[0]);
    if (!j) return twJpgErrorInvalidHandle;
    j->abort_fn = a[1];
    j->abort_handle = a[2];
    return 0;
}
HLE(TwJpgGetAbortCheck) {
    Jpg* j = get(a[0]);
    if (!j) return twJpgErrorInvalidHandle;
    if (a[1]) w32(a[1], j->abort_fn);
    if (a[2]) w32(a[2], j->abort_handle);
    return 0;
}
HLE(TwJpgGetImageInfo) {
    Jpg* j = get(a[0]);
    if (!j) return twJpgErrorInvalidHandle;
    w32(a[1] + 4, (uint32_t)j->rgb->w);
    w32(a[1] + 8, (uint32_t)j->rgb->h);
    w32(a[1] + 12, 3);   // YCbCr
    w32(a[1] + 16, 3);
    return 0;
}
HLE(TwJpgDecodeImage) {
    Jpg* j = get(a[0]);
    if (!j) return twJpgErrorInvalidHandle;
    int fmt = (int)a[1];
    if (fmt < 1 || fmt > 3) return twJpgErrorInvalidPixelFormat;
    uint32_t out = render(j->rgb, j->rgb->w, j->rgb->h, false, fmt);
    if (!out) return memErrNotEnoughSpace;
    w32(a[2], out);
    return 0;
}
HLE(TwJpgDecodeAndScaleImage) {
    Jpg* j = get(a[0]);
    if (!j) return twJpgErrorInvalidHandle;
    uint32_t out = render(j->rgb, (int)a[1], (int)a[2], a[3] & 0xFF, 1);
    if (!out) return memErrNotEnoughSpace;
    w32(a[4], out);
    return 0;
}
static void blit_to_surface(Jpg* j, Surface* s, bool aa) {
    uint8_t c[3];
    for (int y = 0; y < s->h; y++)
        for (int x = 0; x < s->w; x++) {
            sample(j->rgb, x, y, s->w, s->h, aa, c);
            *s->px(x, y) = to565(c);
        }
}
HLE(TwJpgDecodeImageToSurface) {
    Jpg* j = get(a[0]);
    Surface* s = gfx::surface(a[2]);
    if (!j || !s) return twJpgErrorInvalidHandle;
    blit_to_surface(j, s, a[1] & 0xFF);
    return 0;
}
HLE(TwJpgDecodeImageToNewSurface) {
    Jpg* j = get(a[0]);
    if (!j) return twJpgErrorInvalidHandle;
    int w = (int)a[1] ? (int)a[1] : j->rgb->w, h = (int)a[2] ? (int)a[2] : j->rgb->h;
    uint32_t sh = gfx::alloc_surface(a[4], w, h);
    if (!sh) return 0x7000 + 0x100 + 3;   // twGfxErrorSurfaceAllocFailed
    blit_to_surface(j, gfx::surface(sh), a[3] & 0xFF);
    w32(a[5], sh);
    return 0;
}
HLE(TwJpgScaleImageBuffer) {
    // RGB565 buffer -> scaled RGB565 buffer
    int iw = (int)a[1], ih = (int)a[2], ow = (int)a[3], oh = (int)a[4];
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormat(0, iw, ih, 24, SDL_PIXELFORMAT_RGB24);
    for (int y = 0; y < ih; y++)
        for (int x = 0; x < iw; x++) {
            uint16_t v = r16(a[0] + (y * iw + x) * 2);
            uint8_t* d = (uint8_t*)s->pixels + y * s->pitch + x * 3;
            d[0] = (v >> 8) & 0xF8;
            d[1] = (v >> 3) & 0xFC;
            d[2] = (uint8_t)(v << 3);
        }
    uint32_t out = render(s, ow, oh, a[5] & 0xFF, 1);
    SDL_FreeSurface(s);
    if (!out) return memErrNotEnoughSpace;
    w32(a[6], out);
    return 0;
}
