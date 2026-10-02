// Emulated memory access and the Palm-style heap (pointers + handles).
#include "emu.h"
#include <map>

using namespace layout;

uint8_t* host_ptr(uint32_t a, uint32_t len) {
    uint64_t end = (uint64_t)a + len;
    if (a >= RAM_BASE && end <= (uint64_t)RAM_BASE + RAM_SIZE) return g.ram + (a - RAM_BASE);
    if (a >= VRAM_BASE && end <= (uint64_t)VRAM_BASE + VRAM_SIZE) return g.vram + (a - VRAM_BASE);
    if (a >= STACK_BASE && end <= (uint64_t)STACK_BASE + STACK_SIZE) return g.stack + (a - STACK_BASE);
    if (a >= TRAP_BASE && end <= (uint64_t)TRAP_BASE + TRAP_SIZE) return g.trap + (a - TRAP_BASE);
    if (a >= ROM_BASE && end <= (uint64_t)ROM_BASE + ROM_SIZE) return g.rom + (a - ROM_BASE);
    if (end <= (uint64_t)LOWMEM_BASE + LOWMEM_SIZE) return g.lowmem + a;
    return nullptr;
}

static uint8_t* need(uint32_t a, uint32_t len) {
    uint8_t* p = host_ptr(a, len);
    if (!p) {
        LOG("!! host access to unmapped %08x (+%u)", a, len);
        cpu_dump("bad host access");
        static uint8_t junk[65536];
        memset(junk, 0, sizeof junk);
        return junk;
    }
    return p;
}

uint8_t r8(uint32_t a) { return *need(a, 1); }
uint16_t r16(uint32_t a) { uint16_t v; memcpy(&v, need(a, 2), 2); return v; }
uint32_t r32(uint32_t a) { uint32_t v; memcpy(&v, need(a, 4), 4); return v; }
void w8(uint32_t a, uint8_t v) { *need(a, 1) = v; }
void w16(uint32_t a, uint16_t v) { memcpy(need(a, 2), &v, 2); }
void w32(uint32_t a, uint32_t v) { memcpy(need(a, 4), &v, 4); }
void mem_read(uint32_t a, void* dst, uint32_t n) { if (n) memcpy(dst, need(a, n), n); }
void mem_write(uint32_t a, const void* src, uint32_t n) { if (n) memcpy(need(a, n), src, n); }
void mem_fill(uint32_t a, uint8_t v, uint32_t n) { if (n) memset(need(a, n), v, n); }

std::string rstr(uint32_t a, size_t max) {
    std::string s;
    if (!a) return "(null)";
    for (size_t i = 0; i < max; i++) {
        uint8_t* p = host_ptr(a + i);
        if (!p || !*p) break;
        s += (char)*p;
    }
    return s;
}
void wstr(uint32_t a, const std::string& s) { mem_write(a, s.c_str(), (uint32_t)s.size() + 1); }

// ---------------------------------------------------------------------------
// Heap: first-fit allocator over [HEAP_START, RAM_BASE+RAM_SIZE).
// Handles are master-pointer slots in HANDLE_TABLE; *handle == chunk address.
// ---------------------------------------------------------------------------
namespace heap {
struct Blk { uint32_t size; uint32_t handle; uint32_t locks; };
static std::map<uint32_t, Blk> used;
static std::map<uint32_t, uint32_t> freel;   // addr -> size
static std::vector<uint32_t> free_slots;
static std::map<uint32_t, uint32_t> foreign;  // ROM chunks: addr -> size

static uint32_t align8(uint32_t v) { return (v + 7) & ~7u; }

void init() {
    used.clear(); freel.clear(); free_slots.clear();
    freel[HEAP_START] = RAM_BASE + RAM_SIZE - HEAP_START;
    for (uint32_t i = HANDLE_SLOTS - 1; i >= 1; i--) free_slots.push_back(i);
}

uint32_t alloc(uint32_t size, bool zero) {
    uint32_t sz = align8(size ? size : 1);
    for (auto it = freel.begin(); it != freel.end(); ++it) {
        if (it->second < sz) continue;
        uint32_t a = it->first, rest = it->second - sz;
        freel.erase(it);
        if (rest) freel[a + sz] = rest;
        used[a] = {size, 0, 0};
        if (zero) mem_fill(a, 0, sz);
        return a;
    }
    LOG("!! heap exhausted allocating %u bytes", size);
    return 0;
}

static void release(uint32_t a, uint32_t sz) {
    auto nx = freel.find(a + sz);
    if (nx != freel.end()) { sz += nx->second; freel.erase(nx); }
    auto it = freel.lower_bound(a);
    if (it != freel.begin()) {
        auto pv = std::prev(it);
        if (pv->first + pv->second == a) { pv->second += sz; return; }
    }
    freel[a] = sz;
}

void free(uint32_t p) {
    auto it = used.find(p);
    if (it == used.end()) { LOG("!! free of unknown pointer %08x (lr=code+%x)", p, reg(14) - g.code_base); return; }
    uint32_t sz = align8(it->second.size ? it->second.size : 1);
    used.erase(it);
    release(p, sz);
}

bool valid(uint32_t p) { return used.count(p) != 0; }
uint32_t size(uint32_t p) {
    auto it = used.find(p);
    if (it != used.end()) return it->second.size;
    auto f = foreign.find(p);
    return f == foreign.end() ? 0 : f->second;
}
uint32_t new_foreign_handle(uint32_t ptr, uint32_t sz) {
    if (free_slots.empty()) return 0;
    uint32_t h = HANDLE_TABLE + free_slots.back() * 4;
    free_slots.pop_back();
    foreign[ptr] = sz;
    w32(h, ptr);
    return h;
}

bool resize(uint32_t p, uint32_t newSize) {
    auto it = used.find(p);
    if (it == used.end()) return false;
    uint32_t cur = align8(it->second.size ? it->second.size : 1), want = align8(newSize ? newSize : 1);
    if (want <= cur) {
        if (want < cur) release(p + want, cur - want);
        it->second.size = newSize;
        return true;
    }
    auto nx = freel.find(p + cur);
    if (nx == freel.end() || cur + nx->second < want) return false;
    uint32_t avail = nx->second;
    freel.erase(nx);
    uint32_t rest = cur + avail - want;
    if (rest) freel[p + want] = rest;
    mem_fill(p + cur, 0, want - cur);
    it->second.size = newSize;
    return true;
}

uint32_t new_handle(uint32_t size, bool zero) {
    if (free_slots.empty()) { LOG("!! out of handles"); return 0; }
    uint32_t p = alloc(size, zero);
    if (!p) return 0;
    uint32_t h = HANDLE_TABLE + free_slots.back() * 4;
    free_slots.pop_back();
    used[p].handle = h;
    w32(h, p);
    return h;
}

bool is_handle(uint32_t h) {
    return h > HANDLE_TABLE && h < HANDLE_TABLE + HANDLE_SLOTS * 4 && !(h & 3) && r32(h) != 0;
}
uint32_t deref(uint32_t h) { return is_handle(h) ? r32(h) : 0; }

void free_handle(uint32_t h) {
    if (!is_handle(h)) { LOG("!! free of bad handle %08x", h); return; }
    if (foreign.count(r32(h))) { w32(h, 0); free_slots.push_back((h - HANDLE_TABLE) / 4); return; }
    free(r32(h));
    w32(h, 0);
    free_slots.push_back((h - HANDLE_TABLE) / 4);
}

uint32_t lock(uint32_t h) {
    uint32_t p = deref(h);
    auto it = used.find(p);
    if (it != used.end()) it->second.locks++;
    return p;
}
void unlock(uint32_t h) {
    auto it = used.find(deref(h));
    if (it != used.end() && it->second.locks) it->second.locks--;
}
uint32_t lock_count(uint32_t h) { auto it = used.find(deref(h)); return it == used.end() ? 0 : it->second.locks; }

uint32_t recover_handle(uint32_t p) { auto it = used.find(p); return it == used.end() ? 0 : it->second.handle; }

bool resize_handle(uint32_t h, uint32_t newSize) {
    uint32_t p = deref(h);
    if (!p) return false;
    if (resize(p, newSize)) return true;
    Blk b = used[p];
    uint32_t q = alloc(newSize, true);
    if (!q) return false;
    mem_write(q, need(p, b.size), b.size < newSize ? b.size : newSize);
    free(p);
    used[q].handle = h;
    used[q].locks = b.locks;
    w32(h, q);
    return true;
}

uint32_t free_bytes() { uint32_t t = 0; for (auto& f : freel) t += f.second; return t; }
uint32_t largest_free() { uint32_t m = 0; for (auto& f : freel) m = f.second > m ? f.second : m; return m; }
}  // namespace heap
