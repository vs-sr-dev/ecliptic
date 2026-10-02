// Ecliptic - Tapwave Zodiac HLE emulator. Core shared declarations.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unicorn/unicorn.h>

// ---------------------------------------------------------------------------
// Emulated address space (all little endian, like the real device)
// ---------------------------------------------------------------------------
namespace layout {
constexpr uint32_t LOWMEM_BASE = 0x00000000, LOWMEM_SIZE = 0x00010000; // readable like on Palm OS 5 devices
constexpr uint32_t STACK_BASE = 0x0FE00000, STACK_SIZE = 0x00200000;  // 2 MB, grows down
constexpr uint32_t RAM_BASE   = 0x10000000, RAM_SIZE   = 0x08000000;  // 128 MB heap
constexpr uint32_t SYS_AREA = RAM_BASE, SYS_AREA_SIZE = 0x20000;       // "ROM" tables some games range-check
constexpr uint32_t HANDLE_TABLE = RAM_BASE + SYS_AREA_SIZE, HANDLE_SLOTS = 0x10000;    // master pointers
constexpr uint32_t HEAP_START = RAM_BASE + HANDLE_SLOTS * 4;
constexpr uint32_t ROM_BASE   = 0x20000000, ROM_SIZE   = 0x00800000;  // device ROM, as on the Zodiac
constexpr uint32_t THUNK_AREA = ROM_BASE + ROM_SIZE - 0x1000;            // our code thunks (unused ROM tail)
constexpr uint32_t VRAM_BASE  = 0x30000000, VRAM_SIZE  = 0x00800000;  // 8 MB (Imageon W4200)
constexpr uint32_t TRAP_BASE  = 0x7F000000, TRAP_SIZE  = 0x00010000;  // svc stubs
constexpr uint32_t OPAQUE_DB    = 0x7D000000;  // MemHandle dbH values (not dereferenced)
constexpr uint32_t OPAQUE_OPEN  = 0x7E000000;  // DmOpenRef values
}

// Trap ids: 0..611 are TwGlue slots; extra traps (callbacks, sentinels) start here.
constexpr uint32_t TRAP_EXTRA_BASE = 0x400;
constexpr uint32_t TRAP_RETURN = 0xFFF;  // return sentinel for host->ARM calls

// ---------------------------------------------------------------------------
// Global emulator state
// ---------------------------------------------------------------------------
struct Options {
    std::string prc_path;
    std::string card_dir = "card";      // host directory mounted as the SD card volume
    std::string rom_path;               // Zodiac ROM image (databases, DRM ranges)
    bool trace = false;                 // log every TwGlue call
    bool strict = false;                // stop on unimplemented calls
    bool headless = false;
    int scale = 2;
    long exit_after_ms = -1;            // emulated ms
    std::string screenshot;
    std::string keys;                   // scripted key presses
    std::string blocks_file;            // dump of the last executed basic blocks
    int key_start = 1000, key_interval = 500;
};

struct Emu {
    uc_engine* uc = nullptr;
    uint8_t* ram = nullptr;
    uint8_t* vram = nullptr;
    uint8_t* stack = nullptr;
    uint8_t* trap = nullptr;
    uint8_t* rom = nullptr;
    uint8_t* lowmem = nullptr;
    Options opt;
    volatile bool quit = false;
    volatile bool done = false;
    volatile bool watchdog_fired = false;
    int nest = 0;
    uint32_t glue_table = 0;
    uint32_t code_base = 0, code_size = 0;
};
extern Emu g;

// ---------------------------------------------------------------------------
// Memory access helpers (emulated address -> host)
// ---------------------------------------------------------------------------
uint8_t* host_ptr(uint32_t addr, uint32_t len = 1);  // nullptr if not backed
uint8_t  r8(uint32_t a);
uint16_t r16(uint32_t a);
uint32_t r32(uint32_t a);
void w8(uint32_t a, uint8_t v);
void w16(uint32_t a, uint16_t v);
void w32(uint32_t a, uint32_t v);
std::string rstr(uint32_t a, size_t max = 4096);
void wstr(uint32_t a, const std::string& s);  // writes with NUL
void mem_read(uint32_t a, void* dst, uint32_t n);
void mem_write(uint32_t a, const void* src, uint32_t n);
void mem_fill(uint32_t a, uint8_t v, uint32_t n);

// ---------------------------------------------------------------------------
// Heap (MemPtr / MemHandle)
// ---------------------------------------------------------------------------
namespace heap {
void init();
uint32_t alloc(uint32_t size, bool zero = false);  // 0 on failure
void free(uint32_t p);
bool valid(uint32_t p);
uint32_t size(uint32_t p);
bool resize(uint32_t p, uint32_t newSize);  // in place only
uint32_t new_handle(uint32_t size, bool zero = false);
uint32_t new_foreign_handle(uint32_t ptr, uint32_t size);   // handle to memory outside the heap (ROM)
void free_handle(uint32_t h);
bool is_handle(uint32_t h);
uint32_t deref(uint32_t h);                 // master pointer
uint32_t lock(uint32_t h);
void unlock(uint32_t h);
uint32_t lock_count(uint32_t h);
uint32_t recover_handle(uint32_t p);        // 0 if p is not a handle chunk
bool resize_handle(uint32_t h, uint32_t newSize);  // may move
uint32_t free_bytes();
uint32_t largest_free();
}

// ---------------------------------------------------------------------------
// CPU
// ---------------------------------------------------------------------------
void cpu_init();
uint32_t cpu_call(uint32_t addr, const std::vector<uint32_t>& args);
uint32_t reg(int r);           // r0..r15
void set_reg(int r, uint32_t v);
void cpu_dump(const char* why);
void cpu_trace_blocks(uint32_t base, uint32_t size);
void cpu_fix_bx_pc_veneers(uint32_t base, uint32_t size);
void cpu_dump_blocks(const std::string& path);

// ---------------------------------------------------------------------------
// HLE dispatch
// ---------------------------------------------------------------------------
struct Args {
    uint32_t r[4];
    uint32_t sp;
    uint32_t operator[](int i) const { return i < 4 ? r[i] : r32(sp + (i - 4) * 4); }  // AAPCS
};
using HleFn = uint32_t (*)(Args&);
struct HleReg { HleReg(const char* name, HleFn fn); };
#define HLE(name)                                              \
    static uint32_t hle_##name(Args& a);                       \
    static HleReg hlereg_##name(#name, hle_##name);            \
    static uint32_t hle_##name(Args& a)

void glue_init();                          // builds TwGlue table in emulated RAM
uint32_t glue_extra_trap(const char* name, HleFn fn);  // returns ARM address of a new trap
void glue_dispatch(uint32_t trap_id);
int glue_index(const char* name);
uint32_t glue_slot_addr(const char* name);   // trap address for a glue slot by name
uint32_t sys_area_alloc(uint32_t size);      // memory in the low "ROM" area
uint32_t thunk_to(uint32_t target);          // "ldr pc,=target" stub inside THUNK_AREA

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
#define LOG(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)
uint32_t now_ms();   // emulated milliseconds since start
