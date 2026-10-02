// System Manager, Feature Manager, preferences, time, strings and misc Tapwave helpers.
#include "gfx.h"
#include "host.h"
#include "palmos.h"
#include "audio.h"
#include <ctime>
#include <map>
#include <zlib.h>

// ---- Feature Manager ---------------------------------------------------------
static std::map<std::pair<uint32_t, uint16_t>, uint32_t> g_features;
static bool ftr_get(uint32_t creator, uint16_t num, uint32_t* v) {
    auto it = g_features.find({creator, num});
    if (it != g_features.end()) { *v = it->second; return true; }
    if (creator == fourcc("psys")) {
        switch (num) {
            case 1: *v = 0x05203000; return true;   // sysFtrNumROMVersion: Palm OS 5.2 release
            case 2: *v = fourcc("Tpwv"); return true;   // sysFtrNumProductID (company)
            case 13: *v = 0; return true;           // sysFtrNumProcessorID
            case 24: *v = 0x00010000; return true;  // sysFtrNumWinVersion (High density)
        }
    }
    if (creator == fourcc("vfsm") && num == 0) { *v = 200; return true; }   // VFS Manager present
    if (creator == fourcc("expn") && num == 0) { *v = 200; return true; }   // Expansion Manager present
    if (creator == fourcc("Tpwv")) {
        switch (num) {
            case 0: *v = 0x0205; return true;          // twFtrAPIVersion (1.1 ROM)
            case 1: *v = g.glue_table; return true;    // twFtrAPIGlue
            case 2: *v = 0; return true;               // twFtrMusicControlState
            case 3: *v = 0; return true;               // twFtrChangeNumber
        }
    }
    return false;
}
HLE(FtrGet) {
    uint32_t v = 0;
    if (!ftr_get(a[0], (uint16_t)a[1], &v)) {
        LOG("FtrGet('%s', %u): not found", fourcc_str(a[0]).c_str(), a[1] & 0xFFFF);
        return ftrErrNoSuchFeature;
    }
    if (a[2]) w32(a[2], v);
    return 0;
}
HLE(FtrSet) { g_features[{a[0], (uint16_t)a[1]}] = a[2]; return 0; }
HLE(FtrUnregister) { return g_features.erase({a[0], (uint16_t)a[1]}) ? 0 : ftrErrNoSuchFeature; }
HLE(FtrPtrNew) {
    uint32_t p = heap::alloc(a[2], true);
    if (!p) return memErrNotEnoughSpace;
    g_features[{a[0], (uint16_t)a[1]}] = p;
    w32(a[3], p);
    return 0;
}
HLE(FtrPtrFree) {
    auto it = g_features.find({a[0], (uint16_t)a[1]});
    if (it == g_features.end()) return ftrErrNoSuchFeature;
    heap::free(it->second);
    g_features.erase(it);
    return 0;
}
HLE(FtrPtrResize) {
    auto it = g_features.find({a[0], (uint16_t)a[1]});
    if (it == g_features.end()) return ftrErrNoSuchFeature;
    if (!heap::resize(it->second, a[2])) {
        uint32_t q = heap::alloc(a[2], true);
        if (!q) return memErrNotEnoughSpace;
        uint32_t old = heap::size(it->second);
        mem_write(q, host_ptr(it->second, old), old < a[2] ? old : a[2]);
        heap::free(it->second);
        it->second = q;
    }
    w32(a[3], it->second);
    return 0;
}

// ---- display / pen input area ------------------------------------------------
static int g_orientation = 2;   // landscape
void post_display_changed(int w, int h);
static uint16_t g_coord = 72;
static bool portrait() { return g_orientation == 1 || g_orientation == 3; }
// Display size in the current coordinate system (native = 1.5x the standard 72 dpi grid).
static int disp_w() { int n = portrait() ? 320 : 480; return g_coord == 72 ? n * 2 / 3 : n; }
static int disp_h() { int n = portrait() ? 480 : 320; return g_coord == 72 ? n * 2 / 3 : n; }
static void display_changed() { post_display_changed(disp_w(), disp_h()); }
HLE(SysSetOrientation) {
    g_orientation = (int)(a[0] & 0xFFFF);
    if (g_orientation == 0) g_orientation = 2;
    gfx::orientation_changed(g_orientation);
    display_changed();
    return 0;
}
HLE(SysGetOrientation) { return g_orientation; }
HLE(SysGetOrientationTriggerState) { return 0; }
HLE(SysSetOrientationTriggerState) { return 0; }
static uint16_t g_pin_state = 1;
HLE(PINSetInputAreaState) { g_pin_state = (uint16_t)a[0]; display_changed(); return 0; }
HLE(PINGetInputAreaState) { return g_pin_state; }
HLE(PINSetInputTriggerState) { return 0; }
HLE(PINGetInputTriggerState) { return 0; }
HLE(StatHide) { display_changed(); return 0; }
HLE(StatShow) { display_changed(); return 0; }
HLE(StatGetAttribute) { if (a[1]) w32(a[1], 0); return 0; }

HLE(WinGetBounds) {
    // RectangleType: Int16 topLeft.x, topLeft.y, extent.x, extent.y
    w16(a[1], 0); w16(a[1] + 2, 0); w16(a[1] + 4, (uint16_t)disp_w()); w16(a[1] + 6, (uint16_t)disp_h());
    return 0;
}
HLE(WinSetBounds) { return 0; }
HLE(WinSetCoordinateSystem) { uint16_t old = g_coord; g_coord = (uint16_t)a[0]; return old; }
HLE(WinGetCoordinateSystem) { return g_coord; }
HLE(WinScreenMode) {
    // winScreenModeGet* (0, 1) / Set (2) / GetSupported...
    uint32_t op = a[0] & 0xFF;
    if (op <= 1 || op == 3) {
        bool portrait = g_orientation == 1 || g_orientation == 3;
        if (a[1]) w32(a[1], portrait ? 320 : 480);
        if (a[2]) w32(a[2], portrait ? 480 : 320);
        if (a[3]) w32(a[3], 16);
        if (a[4]) w8(a[4], 1);
    }
    return 0;
}
HLE(WinScreenGetAttribute) {
    // winScreenWidth 0, Height 1, RowBytes 2, Depth 3, AllDepths 4, Density 5, PixelFormat 6, Resolution 7
    uint32_t v = 0;
    bool portrait = g_orientation == 1 || g_orientation == 3;
    switch (a[0] & 0xFF) {
        case 0: v = portrait ? 320 : 480; break;
        case 1: v = portrait ? 480 : 320; break;
        case 2: v = (portrait ? 320 : 480) * 2; break;
        case 3: v = 16; break;
        case 4: v = 0x8000 | 0x80 | 0x8 | 0x2 | 0x1; break;
        case 5: v = 108; break;   // kDensityOneAndAHalf
        case 6: v = 3; break;     // pixelFormat565LE
        case 7: v = 108; break;
    }
    if (a[1]) w32(a[1], v);
    return 0;
}

// ---- time ------------------------------------------------------------------
static uint32_t palm_seconds() { return (uint32_t)(time(nullptr) + 2082844800u + 0 /* UTC */); }
// Timing queries are polled constantly by game loops: good, quiet points to refill audio
// (never inside file or memory calls, where a callback could disturb the caller's state).
HLE(TimGetTicks) { audio::pump(); return now_ms(); }   // TNA: SysTicksPerSecond() == 1000
HLE(TimGetSeconds) { return palm_seconds(); }
HLE(TwGetMicroSeconds) {
    // UInt64 TwGetMicroSeconds(void) -> r0:r1
    audio::pump();
    uint64_t us = (uint64_t)now_ms() * 1000;
    set_reg(1, (uint32_t)(us >> 32));
    return (uint32_t)us;
}
HLE(SysTaskDelay) { host_sleep(a[0]); gfx::frame_tick(); return 0; }
HLE(TimSecondsToDateTime) {
    time_t t = (time_t)a[0] - 2082844800;
    struct tm tmv;
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    uint32_t p = a[1];   // second, minute, hour, day, month, year, weekDay (Int16 each)
    w16(p, tmv.tm_sec); w16(p + 2, tmv.tm_min); w16(p + 4, tmv.tm_hour); w16(p + 6, tmv.tm_mday);
    w16(p + 8, tmv.tm_mon + 1); w16(p + 10, tmv.tm_year + 1900); w16(p + 12, tmv.tm_wday);
    return 0;
}
HLE(TimDateTimeToSeconds) {
    uint32_t p = a[0];
    struct tm tmv{};
    tmv.tm_sec = (int16_t)r16(p); tmv.tm_min = (int16_t)r16(p + 2); tmv.tm_hour = (int16_t)r16(p + 4);
    tmv.tm_mday = (int16_t)r16(p + 6); tmv.tm_mon = (int16_t)r16(p + 8) - 1; tmv.tm_year = (int16_t)r16(p + 10) - 1900;
#ifdef _WIN32
    time_t t = _mkgmtime(&tmv);
#else
    time_t t = timegm(&tmv);
#endif
    return (uint32_t)(t + 2082844800);
}

// ---- preferences (stored as a 'Saved Preferences' style database) -------------
static Db* pref_db(bool create) {
    Db* d = dm::by_name("Ecliptic Preferences");
    if (d || !create) return d;
    d = new Db;
    // reuse DmCreateDatabase path rules: res DB saved in ram/
    d->name = "Ecliptic Preferences"; d->type = fourcc("sprf"); d->creator = fourcc("psys"); d->attr = dmHdrAttrResDB;
    d->save_path = "ram/Ecliptic Preferences.prc";
    d->dbH = 0;
    extern Db* dm_register(Db*);
    return dm_register(d);
}
static DbRec* pref_rec(uint32_t creator, uint16_t id, bool saved, bool create) {
    Db* d = pref_db(create);
    if (!d) return nullptr;
    uint32_t type = saved ? creator : (creator ^ 0x80808080u);   // unsaved prefs live under a mangled type
    for (auto& r : d->recs) if (r.type == type && r.id == id) return &r;
    if (!create) return nullptr;
    DbRec r; r.type = type; r.id = id; r.h = 0;
    d->recs.push_back(r);
    return &d->recs.back();
}
HLE(PrefGetAppPreferences) {
    DbRec* r = pref_rec(a[0], (uint16_t)a[1], a[4] & 0xFF, false);
    if (!r || !r->h) { if (a[3]) w16(a[3], 0); return (uint32_t)(int32_t)-1; }   // noPreferenceFound
    uint32_t p = heap::deref(r->h), n = heap::size(p);
    if (n < 2) return (uint32_t)(int32_t)-1;
    int16_t ver = (int16_t)r16(p);
    uint32_t len = n - 2;
    if (a[3]) {
        uint16_t cap = r16(a[3]);
        if (a[2]) mem_write(a[2], host_ptr(p + 2, len), len < cap ? len : cap);
        w16(a[3], (uint16_t)len);
    }
    return (uint32_t)(int32_t)ver;
}
HLE(PrefSetAppPreferences) {
    DbRec* r = pref_rec(a[0], (uint16_t)a[1], a[5] & 0xFF, true);
    uint32_t len = a[4] & 0xFFFF;
    if (r->h) heap::free_handle(r->h);
    r->h = heap::new_handle(len + 2);
    uint32_t p = heap::deref(r->h);
    w16(p, (uint16_t)a[2]);
    if (len && a[3]) mem_write(p + 2, host_ptr(a[3], len), len);
    pref_db(true)->dirty = true;
    return 0;
}
HLE(PrefGetPreference) {
    switch (a[0]) {
        case 1: return 0;      // prefVersion
        case 7: return 0;      // country
        default: return 0;
    }
}
HLE(PrefSetPreference) { return 0; }

// ---- misc system ---------------------------------------------------------------
HLE(SysCurAppDatabase) { if (a[0]) w32(a[0], dm::app_db()->dbH); return 0; }
HLE(SysGetStackInfo) {
    if (a[0]) w32(a[0], layout::STACK_BASE);
    if (a[1]) w32(a[1], layout::STACK_BASE + layout::STACK_SIZE);
    return 1;
}
HLE(SysNotifyRegister) { return 0; }
HLE(SysNotifyUnregister) { return 0; }
HLE(SysNotifyBroadcast) { return 0; }
HLE(SysNotifyBroadcastDeferred) { return 0; }
HLE(EvtResetAutoOffTimer) { return 0; }
HLE(EvtSetAutoOffTimer) { return 0; }
HLE(SysBatteryInfo) {
    if (a[0] & 0xFF) return 0;
    if (a[7]) w8(a[7], 100);
    if (a[6]) w8(a[6], 1);
    return 3900;   // millivolts * ... (returns current voltage in hundredths)
}
HLE(TwBatteryState) { return 0; }
HLE(SysGetROMToken) { return 1; }   // not found
HLE(DbgMessage) { LOG("[DbgMessage] %s", rstr(a[0]).c_str()); return 0; }
HLE(DbgBreak) { LOG("[DbgBreak]"); return 0; }
HLE(ErrDisplayFileLineMsg) {
    LOG("!! ErrDisplayFileLineMsg: %s:%u: %s", rstr(a[0]).c_str(), a[1] & 0xFFFF, rstr(a[2]).c_str());
    cpu_dump("fatal error");
    g.quit = true;
    return 0;
}
HLE(SysErrString) {
    char buf[32];
    snprintf(buf, sizeof buf, "Error 0x%04x", a[0] & 0xFFFF);
    std::string s(buf);
    uint16_t maxLen = (uint16_t)a[2];
    if (maxLen && a[1]) { if (s.size() >= maxLen) s.resize(maxLen - 1); wstr(a[1], s); }
    return a[1];
}

// ---- compression / allocation helpers from TwGlue --------------------------------
HLE(gunzip) {
    uint8_t* src = host_ptr(a[0], a[1]);
    if (!src || a[1] < 18 || src[0] != 0x1F || src[1] != 0x8B) return 0;   // not gzip -> NULL
    uint32_t isize = src[a[1] - 4] | src[a[1] - 3] << 8 | src[a[1] - 2] << 16 | (uint32_t)src[a[1] - 1] << 24;
    uint32_t out = heap::alloc(isize ? isize : 1);
    if (!out) return 0;
    z_stream z{};
    inflateInit2(&z, 16 + MAX_WBITS);
    z.next_in = src; z.avail_in = a[1];
    z.next_out = host_ptr(out, isize); z.avail_out = isize;
    int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    if (r != Z_STREAM_END) { LOG("gunzip: inflate error %d", r); heap::free(out); return 0; }
    return out;
}
HLE(gzip) {
    uLong bound = compressBound(a[1]) + 32;
    std::vector<uint8_t> tmp(bound);
    z_stream z{};
    deflateInit2(&z, 6, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
    z.next_in = host_ptr(a[0], a[1]); z.avail_in = a[1];
    z.next_out = tmp.data(); z.avail_out = (uInt)bound;
    deflate(&z, Z_FINISH);
    uint32_t n = (uint32_t)z.total_out;
    deflateEnd(&z);
    uint32_t out = heap::alloc(n);
    if (out) mem_write(out, tmp.data(), n);
    return out;
}
HLE(crc32) { return (uint32_t)::crc32(a[0], a[2] ? host_ptr(a[1], a[2]) : nullptr, a[2]); }
HLE(Malloc) { return heap::alloc(a[0]); }
HLE(Calloc) { return heap::alloc(a[0] * a[1], true); }
HLE(Free) { if (a[0]) heap::free(a[0]); return 0; }
HLE(Realloc) {
    if (!a[0]) return heap::alloc(a[1]);
    if (!a[1]) { heap::free(a[0]); return 0; }
    if (heap::resize(a[0], a[1])) return a[0];
    uint32_t q = heap::alloc(a[1]);
    if (!q) return 0;
    uint32_t old = heap::size(a[0]);
    mem_write(q, host_ptr(a[0], old), old < a[1] ? old : a[1]);
    heap::free(a[0]);
    return q;
}
HLE(Strdup) {
    std::string s = rstr(a[0], 1 << 20);
    uint32_t p = heap::alloc((uint32_t)s.size() + 1);
    if (p) wstr(p, s);
    return p;
}
