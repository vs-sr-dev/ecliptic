// TwGlue dispatch table: every slot points at an svc stub in the trap page.
#include "emu.h"
#include <map>
#include <set>

using namespace layout;

struct GlueEntry { const char* name; int nargs; bool variadic; bool returns; const char* proto; };
static const GlueEntry kGlue[] = {
#define GLUE(n, a, v, r, p) {#n, a, v != 0, strcmp(r, "void") != 0, p},
#include "glue_table.inc"
#undef GLUE
};
static constexpr int kGlueCount = sizeof(kGlue) / sizeof(kGlue[0]);
static constexpr int kGlueReserved = 64;

struct Slot { std::string name; HleFn fn = nullptr; int nargs = 4; bool returns = true; uint64_t calls = 0; };
static std::map<uint32_t, Slot> slots;              // trap id -> slot
static std::map<std::string, HleFn>& registry() { static std::map<std::string, HleFn> r; return r; }
static uint32_t next_extra = TRAP_EXTRA_BASE;

HleReg::HleReg(const char* name, HleFn fn) { registry()[name] = fn; }

int glue_index(const char* name) {
    for (int i = 0; i < kGlueCount; i++) if (!strcmp(kGlue[i].name, name)) return i;
    return -1;
}
HleFn glue_hle(const char* name) {
    auto it = registry().find(name);
    return it == registry().end() ? nullptr : it->second;
}
uint32_t glue_slot_addr(const char* name) {
    int i = glue_index(name);
    return i < 0 ? 0 : TRAP_BASE + i * 4;
}

static void set_trap(uint32_t id) { w32(TRAP_BASE + id * 4, 0xE12FFF1E); }  // bx lr (see hook_trap)

void glue_init() {
    for (int i = 0; i < kGlueCount; i++) {
        Slot s;
        s.name = kGlue[i].name;
        s.nargs = kGlue[i].nargs;
        s.returns = kGlue[i].returns;
        auto it = registry().find(s.name);
        if (it != registry().end()) s.fn = it->second;
        slots[i] = s;
        set_trap(i);
    }
    // the return sentinel is never executed (uc_emu_start stops there) but keep it a valid insn
    w32(TRAP_BASE + TRAP_RETURN * 4, 0xE12FFF1E);  // bx lr

    uint32_t n = kGlueCount + kGlueReserved;
    g.glue_table = heap::alloc(n * 4, true);
    for (int i = 0; i < kGlueCount; i++) w32(g.glue_table + i * 4, TRAP_BASE + i * 4);

    int impl = 0;
    for (auto& [id, s] : slots) impl += s.fn != nullptr;
    for (auto& [name, fn] : registry())
        if (glue_index(name.c_str()) < 0 && name.rfind("x_", 0) != 0) LOG("warning: HLE '%s' is not a TwGlue slot", name.c_str());
    LOG("TwGlue: %d slots, %d implemented, table at %08x", kGlueCount, impl, g.glue_table);
}

uint32_t glue_extra_trap(const char* name, HleFn fn) {
    uint32_t id = next_extra++;
    Slot s;
    s.name = name;
    s.fn = fn;
    slots[id] = s;
    set_trap(id);
    return TRAP_BASE + id * 4;
}

uint32_t glue_trap_at(uint32_t id, const char* name, HleFn fn) {
    Slot s;
    s.name = name;
    s.fn = fn;
    slots[id] = s;
    set_trap(id);
    return TRAP_BASE + id * 4;
}

static std::set<uint32_t> warned;

void glue_dispatch(uint32_t id) {
    auto it = slots.find(id);
    Args a;
    for (int i = 0; i < 4; i++) a.r[i] = reg(i);
    a.sp = reg(13);
    uint32_t lr = reg(14);
    if (it == slots.end()) {
        LOG("!! unknown trap %x", id);
        g.quit = true;
        return;
    }
    Slot& s = it->second;
    s.calls++;
    uint32_t ret = 0;
    if (s.fn) {
        if (g.opt.trace) {
            char buf[256];
            int n = snprintf(buf, sizeof buf, "[%7u] %s(", now_ms(), s.name.c_str());
            for (int i = 0; i < s.nargs && i < 8 && n < 200; i++)
                n += snprintf(buf + n, sizeof buf - n, "%s%x", i ? ", " : "", a[i]);
            fprintf(stderr, "%s) lr=code+%x\n", buf, lr - g.code_base);
        }
        ret = s.fn(a);
        if (g.opt.trace && s.returns) fprintf(stderr, "          -> %x\n", ret);
    } else {
        if (!warned.count(id) || g.opt.trace) {
            warned.insert(id);
            const char* proto = id < (uint32_t)kGlueCount ? kGlue[id].proto : s.name.c_str();
            LOG("UNIMPL [%u] %s  args=%x,%x,%x,%x lr=code+%x", id, proto, a[0], a[1], a[2], a[3], lr - g.code_base);
        }
        if (g.opt.strict) { g.quit = true; return; }
    }
    if (g.quit) return;
    set_reg(0, ret);
    // Return explicitly: after a nested emulation Unicorn would otherwise resume at the trap
    // address and run the hook again. Bit 0 of LR selects Thumb.
    set_reg(15, lr);
}

void glue_report() {
    LOG("---- TwGlue usage ----");
    for (auto& [id, s] : slots)
        if (s.calls) LOG("  %-36s %8llu %s", s.name.c_str(), (unsigned long long)s.calls, s.fn ? "" : "UNIMPL");
}

static uint32_t g_sys_area_used = 0, g_thunks_used = 0;
uint32_t sys_area_alloc(uint32_t size) {
    uint32_t a = SYS_AREA + g_sys_area_used;
    g_sys_area_used += (size + 7) & ~7u;
    if (g_sys_area_used > SYS_AREA_SIZE) { LOG("!! system area exhausted"); return 0; }
    return a;
}
uint32_t thunk_to(uint32_t target) {
    uint32_t a = THUNK_AREA + g_thunks_used;
    g_thunks_used += 8;
    w32(a, 0xE51FF004);   // ldr pc, [pc, #-4]
    w32(a + 4, target);
    return a;
}
