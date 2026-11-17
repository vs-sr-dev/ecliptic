// Palm OS 5 system calls made through r9 instead of TwGlue.
//
// Native Palm OS 5 code reaches the system modules with "ldr ip, [r9, #-4*n]; ldr pc, [ip, #4*i]":
// r9 points just past a list of module dispatch tables ([r9-4] DAL, [r9-8] Boot, [r9-12] UI,
// [r9-16] the Tapwave Application Layer's exports). Every entry of our tables is a trap.
// In the real ROM most TwGlue slots are stubs that jump straight into one of these tables, so
// those entries reuse the TwGlue slot's trap (src/r9_map.inc, from tools/gen_r9map.py). The few
// entries no TwGlue slot reaches are implemented here, with names inferred from how the games
// call them (only Another World uses them today).
#include "palmos.h"
#include "audio.h"
#include <map>

bool sems_all_free();   // vfs.cpp

namespace {

constexpr uint32_t kTrapBase = 0x1000;   // trap ids 0x1000 + module * 0x400 + index
constexpr int kEntries = 0x400;
const char* const kModules[] = {"DAL", "Boot", "UI", "TAL"};
constexpr int kModuleCount = 4;

struct GlueAlias { int module, index; const char* slot; };
const GlueAlias kGlueAliases[] = {
#include "r9_map.inc"
};

uint32_t call(const char* name, std::initializer_list<uint32_t> vals) {
    HleFn fn = glue_hle(name);
    if (!fn) return 0;
    uint32_t v[12] = {};
    size_t i = 0;
    for (uint32_t x : vals) if (i < 12) v[i++] = x;
    Args a{};
    a.ext = v;
    return fn(a);
}

// ---- DAL mutexes: share the TwSemaphore state, so audio and timers wait for them ----------
uint32_t mutex_create(Args& a) { return call("TwSemaphoreCreate", {a[0], 1}); }   // (&id, tag)
uint32_t mutex_delete(Args& a) { return call("TwSemaphoreDelete", {a[0]}); }
uint32_t mutex_reserve(Args& a) { return call("TwSemaphoreAcquire", {a[0], a[1]}); }   // (id, timeout)
uint32_t mutex_release(Args& a) { return call("TwSemaphoreRelease", {a[0]}); }

// ---- DAL timers: (&id, tag, callback, userData), schedule(id, ticks), delete(id) --------
struct Timer { uint32_t cb = 0, user = 0, due = 0; bool armed = false; };
std::map<uint32_t, Timer> timers;
uint32_t next_timer = 1;
bool in_timer = false;

uint32_t timer_create(Args& a) {
    uint32_t id = next_timer++;
    timers[id] = {a[2], a[3], 0, false};
    if (a[0]) w32(a[0], id);
    return 0;
}
uint32_t timer_delete(Args& a) { timers.erase(a[0]); return 0; }
uint32_t timer_schedule(Args& a) {
    auto it = timers.find(a[0]);
    if (it == timers.end()) return sysErrParamErr;
    it->second.due = now_ms() + a[1];   // ticks: 1000 per second on the Zodiac
    it->second.armed = true;
    return 0;
}

// ---- Boot / UI ----------------------------------------------------------------------------
// (Boot 518, which Another World uses to post its quit event, is EvtAddUniqueEventToQueue:
// TwGlue slot 553 jumps there, so it comes from r9_map.inc.)
uint32_t win_erase_window(Args&) { return call("x_WinEraseWindow", {}); }

struct SysFn { int module, index; const char* name; HleFn fn; };
const SysFn kSysFns[] = {
    {0, 146, "MutexCreate", mutex_create},
    {0, 147, "MutexDelete", mutex_delete},
    {0, 148, "MutexReserve", mutex_reserve},
    {0, 149, "MutexRelease", mutex_release},
    {0, 169, "TimerCreate", timer_create},
    {0, 170, "TimerDelete", timer_delete},
    {0, 171, "TimerSchedule", timer_schedule},
    {1, 726, "WinEraseWindow", win_erase_window},
};

}  // namespace

// Builds the dispatch tables and returns the value for r9.
uint32_t dal_init() {
    uint32_t block = heap::alloc(16 + kModuleCount * 4, true);
    uint32_t r9 = block + kModuleCount * 4;
    w32(r9, r9);   // some homebrew code dereferences r9 twice before jumping into the ROM
    int aliased = 0;
    for (int m = 0; m < kModuleCount; m++) {
        uint32_t table = heap::alloc(kEntries * 4, true);
        w32(r9 - 4 * (m + 1), table);
        for (int i = 0; i < kEntries; i++) {
            char name[32];
            snprintf(name, sizeof name, "%s#%d", kModules[m], i);
            HleFn fn = nullptr;
            for (auto& f : kSysFns)
                if (f.module == m && f.index == i) { snprintf(name, sizeof name, "%s_%s", kModules[m], f.name); fn = f.fn; }
            uint32_t addr = 0;
            if (!fn)
                for (auto& al : kGlueAliases)
                    if (al.module == m && al.index == i) { addr = glue_slot_addr(al.slot); break; }
            if (addr) aliased++;
            else addr = glue_trap_at(kTrapBase + m * kEntries + i, name, fn);
            w32(table + i * 4, addr);
        }
    }
    LOG("r9 tables: %d entries share a TwGlue slot", aliased);
    return r9;
}

// Fires due timers at a safe point (no critical section held).
void dal_pump() {
    if (in_timer || timers.empty() || g.quit || !sems_all_free()) return;
    uint32_t t = now_ms();
    std::vector<uint32_t> due;
    for (auto& [id, tm] : timers)
        if (tm.armed && (int32_t)(t - tm.due) >= 0) due.push_back(id);
    in_timer = true;
    for (uint32_t id : due) {
        auto it = timers.find(id);   // a callback may delete timers
        if (it == timers.end()) continue;
        it->second.armed = false;    // the callback re-arms it if periodic
        cpu_call(it->second.cb, {it->second.user});
        if (g.quit) break;
    }
    in_timer = false;
}
