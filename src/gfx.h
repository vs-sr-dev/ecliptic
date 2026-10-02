// TwGfx surfaces shared with the Palm Window/Bitmap layer.
#pragma once
#include "emu.h"

struct Rect { int x, y, w, h; };

struct Surface {
    uint32_t handle = 0;       // emulated TwGfxSurfaceType* value
    uint32_t gfx = 0;          // owning TwGfxType*
    uint32_t addr = 0;         // pixel memory (VRAM or RAM)
    int w = 0, h = 0, rowBytes = 0;
    int format = 1;            // twGfxPixelFormatRGB565_LE
    int location = 1;
    Rect clip{};
    int locks = 0;
    bool display = false;      // aliases the visible framebuffer
    bool owns_memory = true;
    uint16_t* px(int x, int y) { return (uint16_t*)host_ptr(addr + y * rowBytes + x * 2, 2); }
};

namespace gfx {
void init();
Surface* surface(uint32_t handle);
Surface* palm_display();               // surface the Palm Window Manager draws into
uint32_t vram_alloc(uint32_t bytes);
uint32_t alloc_surface(uint32_t gfx, int w, int h);   // surface handle, 0 on failure
void vram_free(uint32_t addr);
uint16_t packed_to_565(uint32_t rgb);
uint32_t rgb565_to_packed(uint16_t c);
void fill_rect(Surface* s, Rect r, uint16_t c);
// Draw a Palm OS BitmapType (68K/big-endian or native) at (x, y) honouring the clip.
bool draw_palm_bitmap(Surface* dst, int x, int y, uint32_t bmp, bool use_transparency = true);
bool palm_bitmap_size(uint32_t bmp, int* w, int* h);
void frame_tick();
void debug_dump_surfaces();                     // present if a frame interval elapsed
void orientation_changed(int orientation);
}
