// SDL2 host layer.
#include "host.h"
#include "palmos.h"
#include "audio.h"
#include <SDL2/SDL.h>
#include <filesystem>
#include <vector>

namespace fs = std::filesystem;

HostInput g_input;
void midi_pump();
static SDL_Window* win = nullptr;
static SDL_Renderer* ren = nullptr;
static SDL_Texture* tex = nullptr;
static uint32_t screen_addr = layout::VRAM_BASE;
static uint32_t last_present = 0;
static bool portrait = false;
static int scr_w() { return portrait ? SCREEN_H : SCREEN_W; }
static int scr_h() { return portrait ? SCREEN_W : SCREEN_H; }

void host_set_portrait(bool p) {
    if (p == portrait) return;
    portrait = p;
    if (!ren) return;
    if (tex) SDL_DestroyTexture(tex);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, scr_w(), scr_h());
    SDL_RenderSetLogicalSize(ren, scr_w(), scr_h());
    SDL_SetWindowSize(win, scr_w() * g.opt.scale, scr_h() * g.opt.scale);
}

uint32_t host_screen_addr() { return screen_addr; }
void host_set_screen_addr(uint32_t a) { screen_addr = a; }

void host_load_ram_dir(const std::string& dir) {
    if (!fs::exists(dir)) return;
    for (auto& e : fs::directory_iterator(dir)) {
        auto ext = e.path().extension().string();
        if (ext != ".pdb" && ext != ".prc" && ext != ".PDB" && ext != ".PRC") continue;
        Db* d = dm::load_file(e.path().string(), true);
        if (d) LOG("loaded saved database '%s' from %s", d->name.c_str(), e.path().string().c_str());
    }
}

void host_init() {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    if (SDL_Init(SDL_INIT_TIMER | SDL_INIT_EVENTS | (g.opt.headless ? 0 : SDL_INIT_VIDEO)) != 0) {
        LOG("SDL_Init: %s", SDL_GetError());
        return;
    }
    if (g.opt.headless) return;
    win = SDL_CreateWindow("Ecliptic", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, SCREEN_W * g.opt.scale,
                           SCREEN_H * g.opt.scale, SDL_WINDOW_RESIZABLE);
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    SDL_RenderSetLogicalSize(ren, SCREEN_W, SCREEN_H);
    tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB565, SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);
}

void host_shutdown() {
    if (!g.opt.screenshot.empty()) host_save_screenshot(g.opt.screenshot);
    if (tex) SDL_DestroyTexture(tex);
    if (ren) SDL_DestroyRenderer(ren);
    if (win) SDL_DestroyWindow(win);
    SDL_Quit();
}

void host_save_screenshot(const std::string& path) {
    uint8_t* p = host_ptr(screen_addr, SCREEN_W * SCREEN_H * 2);
    if (!p) return;
    SDL_Surface* s = SDL_CreateRGBSurfaceWithFormatFrom(p, scr_w(), scr_h(), 16, scr_w() * 2, SDL_PIXELFORMAT_RGB565);
    if (SDL_SaveBMP(s, path.c_str()) == 0) LOG("screenshot saved to %s", path.c_str());
    SDL_FreeSurface(s);
}

static void check_exit() {
    if (g.opt.exit_after_ms >= 0 && now_ms() >= (uint32_t)g.opt.exit_after_ms) request_stop("exit-after reached");
}

// Keyboard -> Zodiac controls
//   arrows = analog stick + 5-way, Enter = stick press (select)
//   I/L/K/J = action A (up) / B (right) / C (down) / D (left); also Z = C, X = B
//   Q / E = left / right trigger, Tab = Function, Esc = Home
static uint32_t key_bit(SDL_Scancode sc) {
    switch (sc) {
        case SDL_SCANCODE_UP: return 0x00010000;
        case SDL_SCANCODE_DOWN: return 0x00020000;
        case SDL_SCANCODE_LEFT: return 0x00040000;
        case SDL_SCANCODE_RIGHT: return 0x00080000;
        case SDL_SCANCODE_RETURN: return 0x00100000;
        case SDL_SCANCODE_TAB: return 0x00008000;
        case SDL_SCANCODE_ESCAPE: return 0x00800000;
        case SDL_SCANCODE_Q: return 0x04000000;
        case SDL_SCANCODE_E: return 0x08000000;
        case SDL_SCANCODE_I: return 0x10000000;
        case SDL_SCANCODE_L: case SDL_SCANCODE_X: return 0x20000000;
        case SDL_SCANCODE_K: case SDL_SCANCODE_Z: return 0x40000000;
        case SDL_SCANCODE_J: return 0x80000000;
        default: return 0;
    }
}
static uint32_t name_bit(const std::string& n) {
    static const struct { const char* n; uint32_t b; } t[] = {
        {"up", 0x00010000}, {"down", 0x00020000}, {"left", 0x00040000}, {"right", 0x00080000},
        {"select", 0x00100000}, {"F", 0x00008000}, {"home", 0x00800000}, {"L", 0x04000000}, {"R", 0x08000000},
        {"A", 0x10000000}, {"B", 0x20000000}, {"C", 0x40000000}, {"D", 0x80000000}};
    for (auto& e : t) if (n == e.n) return e.b;
    return 0;
}
static uint32_t g_kbd_keys = 0;
static int g_mouse_down = 0;

static uint32_t scripted_keys() {
    if (g.opt.keys.empty()) return 0;
    static std::vector<uint32_t> seq;
    static bool parsed = false;
    if (!parsed) {
        parsed = true;
        size_t p = 0;
        std::string k = g.opt.keys;
        while (p <= k.size()) {
            size_t q = k.find(',', p);
            if (q == std::string::npos) q = k.size();
            std::string tok = k.substr(p, q - p);
            uint32_t bits = 0;
            size_t r = 0;   // "A+B" chords
            while (r <= tok.size()) {
                size_t s2 = tok.find('+', r);
                if (s2 == std::string::npos) s2 = tok.size();
                bits |= name_bit(tok.substr(r, s2 - r));
                r = s2 + 1;
            }
            seq.push_back(bits);
            p = q + 1;
        }
    }
    int t = (int)now_ms() - g.opt.key_start;
    if (t < 0) return 0;
    size_t i = (size_t)(t / g.opt.key_interval);
    if (i >= seq.size()) return 0;
    return (t % g.opt.key_interval) < 150 ? seq[i] : 0;
}

bool host_pump() {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        switch (e.type) {
            case SDL_QUIT:
                // Close at once for the user; the application gets up to 3 s to quit by itself.
                if (win) SDL_HideWindow(win);
                audio::silence();
                request_stop("window closed");
                break;
            case SDL_KEYDOWN: if (!e.key.repeat) g_kbd_keys |= key_bit(e.key.keysym.scancode); break;
            case SDL_KEYUP: g_kbd_keys &= ~key_bit(e.key.keysym.scancode); break;
            case SDL_MOUSEBUTTONDOWN: if (e.button.button == SDL_BUTTON_LEFT) g_mouse_down = 1; break;
            case SDL_MOUSEBUTTONUP: if (e.button.button == SDL_BUTTON_LEFT) g_mouse_down = 0; break;
        }
    }
    if (ren) {
        int mx, my;
        SDL_GetMouseState(&mx, &my);
        float lx, ly;
        SDL_RenderWindowToLogical(ren, mx, my, &lx, &ly);
        g_input.pen_x = (int16_t)lx;
        g_input.pen_y = (int16_t)ly;
    }
    g_input.pen_down = g_mouse_down;
    g_input.keys = g_kbd_keys | scripted_keys();
    uint32_t k = g_input.keys;
    g_input.joy_x = (k & 0x00040000) ? -0x7fff : (k & 0x00080000) ? 0x7fff : 0;
    g_input.joy_y = (k & 0x00010000) ? -0x7fff : (k & 0x00020000) ? 0x7fff : 0;
    check_exit();
    audio::pump();
    midi_pump();
    return !g.quit;
}

void host_present() {
    host_pump();
    if (!tex) return;
    uint8_t* p = host_ptr(screen_addr, SCREEN_W * SCREEN_H * 2);
    if (p) SDL_UpdateTexture(tex, nullptr, p, scr_w() * 2);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, nullptr, nullptr);
    SDL_RenderPresent(ren);
    last_present = now_ms();
}

void host_sleep(uint32_t ms) {
    uint32_t end = now_ms() + ms;
    while (now_ms() < end && host_pump()) SDL_Delay(1);
}
