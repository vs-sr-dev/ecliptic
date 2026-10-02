// Host platform layer (SDL2): window, input, timing, audio.
#pragma once
#include <cstdint>
#include <string>

void host_init();
void host_shutdown();
void host_load_ram_dir(const std::string& dir);

// Presents the emulated screen (RGB565, little endian) and pumps host events.
void host_present();
// Pumps host events without presenting; returns false if the user closed the window.
bool host_pump();
// Sleep for up to ms while pumping events.
void host_sleep(uint32_t ms);

// Zodiac hardware buttons (bit layout of KeyCurrentState / TwInput)
struct HostInput {
    uint32_t keys = 0;          // keyBitXXX mask
    int16_t joy_x = 0, joy_y = 0;  // analog stick, -128..127 style
    bool pen_down = false;
    int16_t pen_x = 0, pen_y = 0;  // screen coordinates
};
extern HostInput g_input;

// Screen geometry (native, landscape: 480x320)
constexpr int SCREEN_W = 480, SCREEN_H = 320;
uint32_t host_screen_addr();          // emulated address of the visible framebuffer
void host_set_portrait(bool portrait);    // 320x480 framebuffer instead of 480x320
void host_set_screen_addr(uint32_t a);
void host_save_screenshot(const std::string& path);
