// Miscellaneous: ARM semihosting (Angel SWI), TwDevice stubs.
#include "palmos.h"

// Angel semihosting SWI 0x123456 (ARM) / 0xAB (Thumb): used by some C runtimes for debug I/O.
bool semihosting(uint32_t imm) {
    if (imm != 0x123456 && imm != 0xAB) return false;
    uint32_t op = reg(0), arg = reg(1);
    switch (op) {
        case 0x03: LOG("[semihost] %c", (char)r8(arg)); set_reg(0, 0); return true;            // SYS_WRITEC
        case 0x04: LOG("[semihost] %s", rstr(arg).c_str()); set_reg(0, 0); return true;        // SYS_WRITE0
        case 0x05: {                                                                             // SYS_WRITE
            uint32_t buf = r32(arg + 4), len = r32(arg + 8);
            std::string s(len, 0);
            mem_read(buf, s.data(), len);
            LOG("[semihost] %s", s.c_str());
            set_reg(0, 0);
            return true;
        }
        case 0x18: LOG("[semihost] exit(%x)", arg); g.quit = true; return true;                  // angel_SWIreason_ReportException
        default: LOG("[semihost] op %x ignored", op); set_reg(0, (uint32_t)-1); return true;
    }
}

// ---- TwDevice: named devices ("vibrator0", "led", "backlight"...) accepted and ignored ----
#include <map>
static std::map<uint32_t, std::string> g_devices;
static uint32_t g_next_dev = 1;
HLE(TwDeviceOpen) {
    uint32_t h = g_next_dev++;
    g_devices[h] = rstr(a[1]);
    LOG("TwDeviceOpen('%s', '%s') -> %u", rstr(a[1]).c_str(), rstr(a[2]).c_str(), h);
    w32(a[0], h);
    return 0;
}
HLE(TwDeviceClose) { g_devices.erase(a[0]); return 0; }
HLE(TwDeviceRead) { if (a[2]) w32(a[2], 0); return 0; }
HLE(TwDeviceWrite) { return 0; }
HLE(TwDeviceGetProperty) { if (a[3]) { uint32_t n = r32(a[3]); if (a[2] && n) mem_fill(a[2], 0, n); } return 0; }
HLE(TwDeviceSetProperty) { return 0; }
HLE(TwDeviceControl) { return 0; }

// ---- Tapwave DRM (TwSecurity.h): every signature check passes --------------------------
static uint32_t g_sec_table = 0;
static uint32_t sec_true(Args& a) { return 1; }
HLE(TwSecGetFunctions) {
    if (!g_sec_table) {
        // Hardened titles (e.g. MTX Mototrax) check that the table lives in the ROM range
        // 0x10000000-0x1001FFFF, that the functions live in 0x20000000-0x207FFFFF and that they
        // belong to the "DAL" database.
        g_sec_table = sys_area_alloc(9 * 4);
        w32(g_sec_table, 1);   // version
        w32(g_sec_table + 4, thunk_to(glue_extra_trap("x_TwSecGetPublicKey", [](Args& a) -> uint32_t {
            if (a[0]) mem_fill(a[0], 0, 162);
            return 0;
        })));
        w32(g_sec_table + 8, thunk_to(glue_extra_trap("x_TwSecGetHardwareId", [](Args& a) -> uint32_t {
            if (a[0]) for (uint32_t i = 0; i < 128; i++) w8(a[0] + i, (uint8_t)(0x5A ^ i));
            return 0;
        })));
        w32(g_sec_table + 12, thunk_to(glue_extra_trap("x_TwSecVerifyDatabase", sec_true)));
        w32(g_sec_table + 16, thunk_to(glue_extra_trap("x_TwSecVerifyCurrentApp", sec_true)));
        w32(g_sec_table + 20, thunk_to(glue_extra_trap("x_TwSecVerifyMemory", sec_true)));
        w32(g_sec_table + 24, thunk_to(glue_extra_trap("x_TwSecFailureReset", [](Args& a) -> uint32_t {
            LOG("!! TwSecFailureReset(%s, %08x, %s)", rstr(a[0]).c_str(), a[1], rstr(a[2]).c_str());
            g.quit = true;
            return 0;
        })));
        w32(g_sec_table + 28, thunk_to(glue_extra_trap("x_TwSecVerifyDatabaseWithKey", sec_true)));
        w32(g_sec_table + 32, thunk_to(glue_extra_trap("x_TwSecVerifyPointerOwnership", sec_true)));
    }
    if (a[2]) w32(a[2], g.code_base);
    if (a[3]) w32(a[3], g.code_size);
    return g_sec_table;
}
