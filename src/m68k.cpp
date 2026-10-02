// PACE-lite: runs 68k Palm OS code on a second Unicorn engine (m68k, big endian) that shares
// the emulated address space with the ARM engine. System traps ("trap #15; dc.w sysTrapXxx")
// are implemented on the host. This is enough for the 68k launch stubs of Tapwave Native
// Applications (SysAppStartup, features, TwLoadModule) and for Call68KFunc callbacks.
#include "m68k.h"
#include "host.h"
#include "tna.h"
#include <map>
#include <set>

using namespace layout;

bool ftr_get(uint32_t creator, uint16_t num, uint32_t* v);   // sys.cpp
void ftr_set(uint32_t creator, uint16_t num, uint32_t v);
uint32_t ui_display_window();                              // evt.cpp

namespace {

constexpr uint32_t RETURN_68K = TRAP_BASE + 0x8000;   // host->68k calls stop here
constexpr uint32_t STACK_68K_SIZE = 0x10000;
constexpr uint32_t INTR_TRAP15 = 32 + 15;             // exception vector number of trap #15
constexpr uint16_t kLaunchFlags = 0x0E;               // sysAppLaunchFlagNewStack | NewGlobals | UIApp

uc_engine* m = nullptr;
bool fault = false;                  // the 68k code hit an exception or bad memory
int depth = 0;                       // nested host->68k calls in progress
uint32_t stack_top = 0;
Db* app_db = nullptr;
uint32_t app_ref = 0, code1_h = 0, code0_h = 0;
uint32_t app_info = 0, screen_bmp = 0;

uint32_t mreg(int r) { uint32_t v = 0; uc_reg_read(m, r, &v); return v; }
void set_mreg(int r, uint32_t v) { uc_reg_write(m, r, &v); }

// ---- trap table -------------------------------------------------------------
// CodeWarrior/GCC push arguments right to left, so the first one is at SP. Bytes are pushed as
// a word with the value in the even (high) byte.
struct P68 {
    uint32_t sp, off = 0;
    uint16_t sel = 0;   // d2 selector of dispatch traps
    uint16_t w() { uint16_t v = rb16(sp + off); off += 2; return v; }
    uint32_t l() { uint32_t v = rb32(sp + off); off += 4; return v; }
    uint8_t b() { uint8_t v = r8(sp + off); off += 2; return v; }
};
using Trap68Fn = uint32_t (*)(P68&);
struct Trap68 { const char* name; Trap68Fn fn; };
std::map<uint16_t, Trap68>& traps() { static std::map<uint16_t, Trap68> t; return t; }
struct Trap68Reg { Trap68Reg(uint16_t n, const char* name, Trap68Fn fn) { traps()[n] = {name, fn}; } };

const char* trap_name(uint16_t n) {
    static std::map<uint16_t, const char*> names = {
#define TRAPNAME(n, s) {n, s},
#define TWSELNAME(n, s)
#include "traps68_table.inc"
#undef TRAPNAME
#undef TWSELNAME
    };
    auto it = names.find(n);
    return it == names.end() ? "?" : it->second;
}
const char* tw_selector_name(uint16_t n) {
    static std::map<uint16_t, const char*> names = {
#define TRAPNAME(n, s)
#define TWSELNAME(n, s) {n, s},
#include "traps68_table.inc"
#undef TRAPNAME
#undef TWSELNAME
    };
    auto it = names.find(n);
    return it == names.end() ? "?" : it->second;
}

// Calls the ARM-side HLE implementation of a TwGlue function with plain values.
uint32_t arm(const char* name, std::initializer_list<uint32_t> vals) {
    HleFn fn = glue_hle(name);
    if (!fn) { LOG("!! PACE-lite: no ARM HLE for %s", name); return 0; }
    uint32_t v[12] = {};
    size_t i = 0;
    for (uint32_t x : vals) if (i < 12) v[i++] = x;
    Args a{};
    a.ext = v;
    return fn(a);
}

std::set<uint32_t> warned;
void unimplemented(const char* what, uint32_t id) {
    if (warned.insert(id).second || g.opt.trace) LOG("UNIMPL 68k %s", what);
}

uint32_t dispatch(uint16_t num, uint32_t sp, uint16_t sel) {
    auto it = traps().find(num);
    if (it == traps().end()) {
        char buf[96];
        snprintf(buf, sizeof buf, "trap %04X %s (d2=%u)", num, trap_name(num), sel);
        unimplemented(buf, num);
        return 0;
    }
    if (g.opt.trace) LOG("[%7u] 68k %s (sel %u)", now_ms(), it->second.name, sel);
    P68 p{sp, 0, sel};
    return it->second.fn(p);
}

void hook_intr(uc_engine* uc, uint32_t intno, void*) {
    uint32_t pc = mreg(UC_M68K_REG_PC);
    if (intno != INTR_TRAP15) {
        LOG("!! 68k exception %u at %08x", intno, pc);
        fault = true;
        g.quit = true;
        uc_emu_stop(uc);
        return;
    }
    // PC is at the trap instruction; the trap number follows it.
    uint16_t num = rb16(pc + 2);
    uint32_t r = dispatch(num, mreg(UC_M68K_REG_A7), (uint16_t)mreg(UC_M68K_REG_D2));
    // Pointer results go in A0, others in D0; both are scratch registers across traps, and
    // dispatch traps (TwDispatch) return either kind, so set both.
    set_mreg(UC_M68K_REG_D0, r);
    set_mreg(UC_M68K_REG_A0, r);
    set_mreg(UC_M68K_REG_PC, pc + 4);
    if (g.quit) uc_emu_stop(uc);
}

bool hook_bad_mem(uc_engine* uc, uc_mem_type type, uint64_t addr, int size, int64_t, void*) {
    LOG("!! 68k invalid %s at %08llx (size %d), pc=%08x", type == UC_MEM_WRITE_UNMAPPED ? "write" : "read",
        (unsigned long long)addr, size, mreg(UC_M68K_REG_PC));
    fault = true;
    g.quit = true;
    return false;
}

void map_shared(uint32_t base, uint32_t size, uint8_t* host) {
    uc_err e = uc_mem_map_ptr(m, base, size, UC_PROT_ALL, host);
    if (e) LOG("!! m68k map %08x: %s", base, uc_strerror(e));
}

// Runs a 68k function with raw stack arguments; registers are preserved for the caller.
uint32_t call_function(uint32_t fn, const uint8_t* args, uint32_t nargs, bool wantA0) {
    uint32_t saved[18];
    for (int i = 0; i < 8; i++) { saved[i] = mreg(UC_M68K_REG_D0 + i); saved[8 + i] = mreg(UC_M68K_REG_A0 + i); }
    saved[16] = mreg(UC_M68K_REG_PC);
    saved[17] = mreg(UC_M68K_REG_SR);

    uint32_t sp = depth ? saved[15] - 64 : stack_top;
    sp = (sp - nargs) & ~3u;
    if (nargs) mem_write(sp, args, nargs);
    sp -= 4;
    wb32(sp, RETURN_68K);
    set_mreg(UC_M68K_REG_A7, sp);

    depth++;
    uc_err e = uc_emu_start(m, fn, RETURN_68K, 0, 0);
    depth--;
    if (e) { LOG("!! 68k emulation error: %s (calling %08x)", uc_strerror(e), fn); g.quit = true; }
    uint32_t r = mreg(wantA0 ? UC_M68K_REG_A0 : UC_M68K_REG_D0);

    set_mreg(UC_M68K_REG_SR, saved[17]);
    for (int i = 0; i < 8; i++) { set_mreg(UC_M68K_REG_D0 + i, saved[i]); set_mreg(UC_M68K_REG_A0 + i, saved[8 + i]); }
    set_mreg(UC_M68K_REG_PC, saved[16]);
    return r;
}

// Out-parameters: ARM handlers write little endian into a scratch buffer, copied back BE.
struct Scratch {
    uint32_t p;
    Scratch() : p(heap::alloc(64, true)) {}
    ~Scratch() { heap::free(p); }
    uint32_t at(int i) const { return p + i * 8; }
    void out32(int i, uint32_t dst) const { if (dst) wb32(dst, r32(at(i))); }
    void out16(int i, uint32_t dst) const { if (dst) wb16(dst, r16(at(i))); }
    void out8(int i, uint32_t dst) const { if (dst) w8(dst, r8(at(i))); }
};

uint32_t find_res(Db* db, const char* type, uint16_t id) {
    for (auto& r : db->recs) if (r.type == fourcc(type) && r.id == id) return r.h;
    return 0;
}

}  // namespace

// The handler's result goes to both D0 and A0 (see hook_intr).
#define TRAP68(num, name)                                             \
    static uint32_t t68_##name(P68& p);                               \
    static Trap68Reg t68reg_##name(num, #name, t68_##name);           \
    static uint32_t t68_##name(P68& p)

// ---- application startup ------------------------------------------------------------------
// SysAppInfoType (68k layout): cmd 0, cmdPBP 2, launchFlags 6, taskID 8, codeH 0xC, dbP 0x10,
// stackP 0x14, globalsChunkP 0x18, memOwnerID 0x1C, ... a5Ptr 0x28.
TRAP68(0xA08F, SysAppStartup) {
    uint32_t infoPP = p.l(), prevGlobalsPP = p.l(), globalsPtrPP = p.l();
    uint32_t ai = heap::alloc(0x40, true);
    wb16(ai + 0, 0);                  // sysAppLaunchCmdNormalLaunch
    wb32(ai + 2, 0);
    wb16(ai + 6, kLaunchFlags);
    wb32(ai + 0xC, code1_h);
    wb32(ai + 0x10, app_ref);
    wb16(ai + 0x1C, 2);
    // Globals: code #0 = aboveA5 size, belowA5 size, jump table size and offset, jump table.
    uint32_t above = 0, below = 0, jt_size = 0, jt_off = 0, c0 = heap::deref(code0_h);
    if (c0 && heap::size(c0) >= 16) { above = rb32(c0); below = rb32(c0 + 4); jt_size = rb32(c0 + 8); jt_off = rb32(c0 + 12); }
    uint32_t glob = heap::alloc(below + above + 16, true), a5 = glob + below;
    if (c0 && jt_size && 16 + jt_size <= heap::size(c0) && jt_off + jt_size <= above)
        mem_write(a5 + jt_off, host_ptr(c0 + 16, jt_size), jt_size);
    wb32(ai + 0x18, glob);
    wb32(ai + 0x28, a5);
    uint32_t prev = mreg(UC_M68K_REG_A5);
    set_mreg(UC_M68K_REG_A5, a5);
    app_info = ai;
    if (infoPP) wb32(infoPP, ai);
    if (prevGlobalsPP) wb32(prevGlobalsPP, prev);
    if (globalsPtrPP) wb32(globalsPtrPP, a5);
    LOG("68k: SysAppStartup, A5=%08x (%u above, %u below)", a5, above, below);
    return 0;
}
TRAP68(0xA090, SysAppExit) {
    p.l();
    uint32_t prevGlobals = p.l();
    set_mreg(UC_M68K_REG_A5, prevGlobals);
    return 0;
}
TRAP68(0xA33C, SysGetAppInfo) {
    uint32_t uiAppPP = p.l(), actionPP = p.l();
    if (uiAppPP) wb32(uiAppPP, app_info);
    if (actionPP) wb32(actionPP, app_info);
    return app_info;
}
TRAP68(0xA0AC, SysCurAppDatabase) {
    uint32_t cardNoP = p.l(), dbIDP = p.l();
    if (cardNoP) wb16(cardNoP, 0);
    if (dbIDP) wb32(dbIDP, app_db ? app_db->dbH : 0);
    return 0;
}
TRAP68(0xA0A7, SysUIAppSwitch) {
    LOG("68k: SysUIAppSwitch requested (application refused to start?)");
    return sysErrNotAllowed;
}
TRAP68(0xA084, ErrDisplayFileLineMsg) {
    uint32_t file = p.l(); uint16_t line = p.w(); uint32_t msg = p.l();
    LOG("!! 68k ErrDisplayFileLineMsg: %s:%u: %s", rstr(file).c_str(), line, rstr(msg).c_str());
    g.quit = true;
    return 0;
}
TRAP68(0xA0AD, SysFatalAlert) { LOG("!! 68k SysFatalAlert: %s", rstr(p.l()).c_str()); return 0; }
TRAP68(0xA192, FrmAlert) {
    uint16_t id = p.w();
    uint32_t h = dm::find_resource_any(fourcc("Talt"), id);
    uint32_t t = h ? heap::deref(h) : 0;
    if (t) {
        std::string title = rstr(t + 8), msg = rstr(t + 8 + (uint32_t)title.size() + 1);
        LOG("68k: FrmAlert(%u) \"%s\": %s", id, title.c_str(), msg.c_str());
    } else {
        LOG("68k: FrmAlert(%u)", id);
    }
    return 0;
}

// ---- features ---------------------------------------------------------------------------
TRAP68(0xA27B, FtrGet) {
    uint32_t creator = p.l(); uint16_t num = p.w(); uint32_t valueP = p.l();
    uint32_t v = 0;
    if (!ftr_get(creator, num, &v)) return ftrErrNoSuchFeature;
    if (valueP) wb32(valueP, v);
    return 0;
}
TRAP68(0xA27C, FtrSet) {
    uint32_t creator = p.l(); uint16_t num = p.w(); uint32_t v = p.l();
    ftr_set(creator, num, v);
    return 0;
}
TRAP68(0xA27A, FtrUnregister) { uint32_t c = p.l(); uint16_t n = p.w(); return arm("FtrUnregister", {c, n}); }
TRAP68(0xA35A, FtrPtrNew) {
    uint32_t creator = p.l(); uint16_t num = p.w(); uint32_t size = p.l(), newPtrP = p.l();
    uint32_t q = heap::alloc(size, true);
    if (!q) return memErrNotEnoughSpace;
    ftr_set(creator, num, q);
    if (newPtrP) wb32(newPtrP, q);
    return 0;
}
TRAP68(0xA35B, FtrPtrFree) { uint32_t c = p.l(); uint16_t n = p.w(); return arm("FtrPtrFree", {c, n}); }

// ---- memory -----------------------------------------------------------------------------
TRAP68(0xA011, MemChunkNew) { p.w(); uint32_t size = p.l(); return heap::alloc(size); }
TRAP68(0xA012, MemChunkFree) { uint32_t q = p.l(); if (q) heap::free(q); return 0; }
TRAP68(0xA013, MemPtrNew) { return heap::alloc(p.l()); }
TRAP68(0xA01E, MemHandleNew) { return heap::new_handle(p.l()); }
TRAP68(0xA02B, MemHandleFree) { uint32_t h = p.l(); if (h) heap::free_handle(h); return 0; }
TRAP68(0xA021, MemHandleLock) { return arm("MemHandleLock", {p.l()}); }
TRAP68(0xA022, MemHandleUnlock) { return arm("MemHandleUnlock", {p.l()}); }
TRAP68(0xA035, MemPtrUnlock) { uint32_t q = p.l(); uint32_t h = heap::recover_handle(q); if (h) heap::unlock(h); return 0; }
TRAP68(0xA014, MemPtrRecoverHandle) { return heap::recover_handle(p.l()); }
TRAP68(0xA027, MemSet) { uint32_t d = p.l(), n = p.l(); uint8_t v = p.b(); mem_fill(d, v, n); return 0; }
TRAP68(0xA026, MemMove) { uint32_t d = p.l(), s = p.l(), n = p.l(); return arm("MemMove", {d, s, n}); }
TRAP68(0xA00E, MemHeapCompact) { return 0; }

// ---- data manager -----------------------------------------------------------------------
TRAP68(0xA060, DmGet1Resource) { uint32_t t = p.l(); uint16_t id = p.w(); return arm("DmGet1Resource", {t, id}); }
TRAP68(0xA05F, DmGetResource) { uint32_t t = p.l(); uint16_t id = p.w(); return arm("DmGetResource", {t, id}); }
TRAP68(0xA061, DmReleaseResource) { p.l(); return 0; }
TRAP68(0xA076, DmWrite) {
    uint32_t rec = p.l(), off = p.l(), src = p.l(), n = p.l();
    return arm("DmWrite", {rec, off, src, n});
}
TRAP68(0xA078, DmGetNextDatabaseByTypeCreator) {
    uint8_t fresh = p.b(); uint32_t stateP = p.l(), type = p.l(), creator = p.l(); uint8_t latest = p.b();
    uint32_t cardP = p.l(), dbIDP = p.l();
    uint32_t out = heap::alloc(4, true);
    uint32_t r = arm("DmGetNextDatabaseByTypeCreator", {fresh, stateP, type, creator, latest, out});
    if (cardP) wb16(cardP, 0);
    if (dbIDP) wb32(dbIDP, r32(out));
    heap::free(out);
    return r;
}

// ---- window / bitmap / pen input ----------------------------------------------------------
TRAP68(0xA200, WinGetDisplayWindow) { return ui_display_window(); }
TRAP68(0xA3A2, WinGetBitmap) {
    p.l();
    if (!screen_bmp) screen_bmp = heap::alloc(32, true);
    // BitmapType v3 header (big endian, as a 68k app sees it): width, height, rowBytes,
    // flags, pixelSize, version, size, pixelFormat, ...
    uint32_t o = arm("SysGetOrientation", {});
    int w = SCREEN_W, h = SCREEN_H;
    if (o == 1 || o == 3) std::swap(w, h);
    wb16(screen_bmp + 0, (uint16_t)w); wb16(screen_bmp + 2, (uint16_t)h); wb16(screen_bmp + 4, (uint16_t)(w * 2));
    wb16(screen_bmp + 6, 0); w8(screen_bmp + 8, 16); w8(screen_bmp + 9, 3);
    return screen_bmp;
}
TRAP68(0xA376, BmpGetBits) {
    uint32_t bmp = p.l();
    if (bmp && bmp == screen_bmp) return host_screen_addr();
    LOG("68k: BmpGetBits(%08x) on a non-screen bitmap is not supported", bmp);
    return 0;
}
TRAP68(0xA470, PinsDispatch) {
    switch (p.sel) {
        case 0: return arm("PINSetInputAreaState", {p.w()});
        case 1: return arm("PINGetInputAreaState", {});
        case 2: return arm("PINSetInputTriggerState", {p.w()});
        case 3: return arm("PINGetInputTriggerState", {});
        case 16: return arm("StatHide", {});
        case 17: return arm("StatShow", {});
        case 19: return arm("SysGetOrientation", {});
        case 20: return arm("SysSetOrientation", {p.w()});
        case 21: return arm("SysGetOrientationTriggerState", {});
        case 22: return arm("SysSetOrientationTriggerState", {p.w()});
    }
    unimplemented(("PinsDispatch selector " + std::to_string(p.sel)).c_str(), 0x10000 | p.sel);
    return 0;
}

TRAP68(0xA33E, WinScreenMode) {
    uint8_t op = p.b(); uint32_t wP = p.l(), hP = p.l(), dP = p.l(), cP = p.l();
    Scratch t;
    uint32_t r = arm("WinScreenMode", {op, wP ? t.at(0) : 0, hP ? t.at(1) : 0, dP ? t.at(2) : 0, cP ? t.at(3) : 0});
    if (op == 0 || op == 1 || op == 4 || op == 5) { t.out32(0, wP); t.out32(1, hP); t.out32(2, dP); t.out8(3, cP); }
    return r;
}
TRAP68(0xA20B, WinGetDisplayExtent) {
    uint32_t xP = p.l(), yP = p.l();
    Scratch t;
    arm("WinGetBounds", {0, t.p});
    if (xP) wb16(xP, r16(t.p + 4));
    if (yP) wb16(yP, r16(t.p + 6));
    return 0;
}
TRAP68(0xA39D, WinPalette) { return 0; }
TRAP68(0xA3EC, HighDensityDispatch) {
    switch (p.sel) {
        case 8: return arm("WinSetCoordinateSystem", {p.w()});
        case 9: return arm("WinGetCoordinateSystem", {});
        case 14: {   // WinScreenGetAttribute(selector, UInt32* attrP)
            uint8_t sel = p.b(); uint32_t attrP = p.l();
            Scratch t;
            uint32_t r = arm("WinScreenGetAttribute", {sel, t.at(0)});
            t.out32(0, attrP);
            return r;
        }
    }
    unimplemented(("HighDensityDispatch selector " + std::to_string(p.sel)).c_str(), 0x30000 | p.sel);
    return 0;
}
// A minimal form so launchers can show (and remove) a splash form.
TRAP68(0xA32B, FrmNewForm) { return heap::alloc(64, true); }
TRAP68(0xA171, FrmDrawForm) { return 0; }
TRAP68(0xA170, FrmDeleteForm) { uint32_t f = p.l(); if (heap::valid(f)) heap::free(f); return 0; }

// VFS Manager (FileSystemDispatch, selector in d2)
TRAP68(0xA348, FileSystemDispatch) {
    switch (p.sel) {
        case 19: { uint16_t vol = p.w(); uint32_t name = p.l(); return arm("VFSDirCreate", {vol, name}); }
        case 22: return 0;   // VFSRegisterDefaultDirectory: no default directories are tracked
        case 27: {   // VFSVolumeEnumerate(UInt16* volRefNumP, UInt32* volIteratorP)
            uint32_t volP = p.l(), itP = p.l();
            Scratch t;
            if (itP) w32(t.at(1), rb32(itP));
            uint32_t r = arm("VFSVolumeEnumerate", {t.at(0), t.at(1)});
            t.out16(0, volP);
            t.out32(1, itP);
            return r;
        }
    }
    char buf[64];
    snprintf(buf, sizeof buf, "FileSystemDispatch selector %u", p.sel);
    unimplemented(buf, 0x40000 | p.sel);
    return sysErrParamErr;
}

// ---- misc ---------------------------------------------------------------------------------
// 68k code sees the classic 100 Hz tick (TNA code gets 1000 Hz through TwGlue).
TRAP68(0xA0F7, TimGetTicks) { return now_ms() / 10; }
TRAP68(0xA2E9, SysTicksPerSecond) { return 100; }
TRAP68(0xA0C2, SysRandom) {   // Palm OS linear congruential generator, 0..sysRandomMax
    static uint32_t seed = 1;
    int32_t ns = (int32_t)p.l();
    if (ns) seed = (uint32_t)ns;
    seed = seed * 0x015A4E35u + 1;
    return (seed >> 16) & 0x7FFF;
}
TRAP68(0xA355, SysNotifyBroadcast) { p.l(); return 0; }
TRAP68(0xA0C0, SysSetAutoOffTime) { return 0; }
TRAP68(0xA130, EvtResetAutoOffTimer) { return 0; }
TRAP68(0xA2A9, DlkGetSyncInfo) {
    uint32_t succP = p.l(), lastP = p.l(), stateP = p.l(), nameP = p.l(), logP = p.l(), logLenP = p.l();
    if (succP) wb32(succP, 0);
    if (lastP) wb32(lastP, 0);
    if (stateP) w8(stateP, 0);
    if (nameP) wstr(nameP, "Ecliptic");   // HotSync user name
    if (logP) w8(logP, 0);
    if (logLenP) wb16(logLenP, 0);
    return 0;
}
TRAP68(0xA0C5, StrCopy) { uint32_t d = p.l(), s = p.l(); return arm("StrCopy", {d, s}); }
TRAP68(0xA0C6, StrCat) { uint32_t d = p.l(), s = p.l(); return arm("StrCat", {d, s}); }
TRAP68(0xA0C7, StrLen) { return arm("StrLen", {p.l()}); }

// ---- native code --------------------------------------------------------------------------
TRAP68(0xA45A, PceNativeCall) {
    uint32_t fn = p.l(), userData = p.l();
    if (!fn) { LOG("68k: PceNativeCall(NULL)"); return 0; }
    return tna::run(fn, userData);
}
// TwSecGetFunctions from 68k code returns a table of 68k-callable functions: big-endian
// pointers to stubs "trap #15; dc.w 0xAF0n; rts" handled by the pseudo-traps below. Every
// verification succeeds, as with the ARM table (misc.cpp).
static uint32_t sec_table_68k() {
    static uint32_t table = 0;
    if (table) return table;
    table = sys_area_alloc(9 * 4);
    uint32_t stubs = heap::alloc(8 * 6, true);
    wb32(table, 1);   // version
    for (int i = 0; i < 8; i++) {
        uint32_t st = stubs + i * 6;
        wb16(st, 0x4E4F); wb16(st + 2, (uint16_t)(0xAF00 + i)); wb16(st + 4, 0x4E75);
        wb32(table + 4 + i * 4, st);
    }
    return table;
}
TRAP68(0xAF00, TwSecGetPublicKey) { uint32_t k = p.l(); if (k) mem_fill(k, 0, 162); return 0; }
TRAP68(0xAF01, TwSecGetHardwareId) {
    uint32_t id = p.l();
    if (id) for (uint32_t i = 0; i < 128; i++) w8(id + i, (uint8_t)(0x5A ^ i));
    return 0;
}
TRAP68(0xAF02, TwSecVerifyDatabase) { return 1; }
TRAP68(0xAF03, TwSecVerifyCurrentApp) { return 1; }
TRAP68(0xAF04, TwSecVerifyMemory) { return 1; }
TRAP68(0xAF05, TwSecFailureReset) { LOG("!! 68k TwSecFailureReset"); g.quit = true; return 0; }
TRAP68(0xAF06, TwSecVerifyDatabaseWithKey) { return 1; }
TRAP68(0xAF07, TwSecVerifyPointerOwnership) { return 1; }

// OEMDispatch2 is the Tapwave TAL dispatcher (TwTraps.h): selector in d2.
TRAP68(0xA443, TwDispatch) {
    if (p.sel == 388) {   // TwLoadModule(dbType, dbCreator, rsrcType, rsrcID, flags, entryP)
        uint32_t type = p.l(), creator = p.l(), rtype = p.l(), rid = p.l(), flags = p.l(), entryP = p.l();
        Db* db = app_db;
        if (type || creator) {
            db = nullptr;
            for (Db* d : dm::all()) if (!d->deleted && (!type || d->type == type) && (!creator || d->creator == creator)) db = d;
        }
        LOG("68k: TwLoadModule('%s', %u, flags %x)", db ? db->name.c_str() : "?", rid, flags);
        if (!db || rid > 0xFFFF) return sysErrParamErr;
        if (!(flags & 1)) LOG("68k: TwLoadModule without twLoadFlagTNA is treated as a TNA load");
        uint32_t entry = tna::load(db, rtype, (uint16_t)rid);
        if (!entry) return sysErrLibNotFound;
        if (entryP) wb32(entryP, entry);
        if (flags & 2) tna::run(entry, 0);   // twLoadFlagQuickRun
        return 0;
    }
    if (p.sel == 389) return 0;   // TwUnloadModule
    if (p.sel == 358) return sec_table_68k();   // TwSecGetFunctions
    char buf[96];
    snprintf(buf, sizeof buf, "TwDispatch selector %u %s", p.sel, tw_selector_name(p.sel));
    unimplemented(buf, 0x20000 | p.sel);
    return 0;
}

namespace m68k {

void init() {
    uc_err e = uc_open(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, &m);
    if (e) { LOG("uc_open(m68k): %s", uc_strerror(e)); m = nullptr; return; }
    uc_ctl_set_cpu_model(m, UC_CPU_M68K_M68040);
    map_shared(LOWMEM_BASE, LOWMEM_SIZE, g.lowmem);
    map_shared(RAM_BASE, RAM_SIZE, g.ram);
    map_shared(ROM_BASE, ROM_SIZE, g.rom);
    map_shared(VRAM_BASE, VRAM_SIZE, g.vram);
    map_shared(TRAP_BASE, TRAP_SIZE, g.trap);
    uc_hook h1, h2;
    uc_hook_add(m, &h1, UC_HOOK_INTR, (void*)hook_intr, nullptr, 1, 0);
    uc_hook_add(m, &h2, UC_HOOK_MEM_INVALID, (void*)hook_bad_mem, nullptr, 1, 0);
    // Supervisor mode, as Palm OS runs applications. SR first: writing it switches stacks.
    set_mreg(UC_M68K_REG_SR, 0x2000);
    uint32_t stack = heap::alloc(STACK_68K_SIZE, true);
    stack_top = stack + STACK_68K_SIZE - 16;
    set_mreg(UC_M68K_REG_A7, stack_top);
    wb16(RETURN_68K, 0x4E71);   // nop: never executed, emulation stops here
}

bool faulted() { return fault; }

bool run_app(Db* app) {
    if (!m) return false;
    app_db = app;
    code1_h = find_res(app, "code", 1);
    code0_h = find_res(app, "code", 0);
    if (!code1_h) return false;
    app_ref = dm::open(app, 1);
    uint32_t entry = heap::deref(code1_h);
    LOG("68k: running code #1 (%u bytes at %08x)", heap::size(entry), entry);
    uint32_t r = call_function(entry, nullptr, 0, false);
    LOG("68k: PilotMain returned %u", r);
    return true;
}

uint32_t call68k(uint32_t trapOrFunction, uint32_t argsP, uint32_t sizeAndWantA0) {
    if (!m) return 0;
    bool wantA0 = sizeAndWantA0 & 0x10000000;   // kPceNativeWantA0
    uint32_t n = sizeAndWantA0 & 0x0FFFFFFF;
    std::vector<uint8_t> args(n);
    if (n) mem_read(argsP, args.data(), n);
    if (trapOrFunction <= 0xFFF) {
        // A system trap: run the host implementation on a copy of the arguments.
        uint16_t num = (uint16_t)(0xA000 | trapOrFunction);
        uint32_t sp = (depth ? mreg(UC_M68K_REG_A7) - 64 : stack_top) - n;
        sp &= ~3u;
        if (n) mem_write(sp, args.data(), n);
        return dispatch(num, sp, 0);
    }
    return call_function(trapOrFunction, args.data(), n, wantA0);
}

}  // namespace m68k
