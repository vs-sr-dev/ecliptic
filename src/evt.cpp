// Event Manager, Tapwave application framework (TwAppStart/TwAppRun), keys and TwInput.
#include "gfx.h"
#include "host.h"
#include "palmos.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <vector>

constexpr uint32_t EVENT_SIZE = 28;   // sizeof(EventType) with ARM alignment
enum : uint16_t {
    nilEvent = 0, penDownEvent = 1, penUpEvent = 2, penMoveEvent = 3, keyDownEvent = 4, winEnterEvent = 5, winExitEvent = 6,
    appStopEvent = 22, frmLoadEvent = 23, frmOpenEvent = 24, frmCloseEvent = 28, keyUpEvent = 0x4000,
};
constexpr uint16_t commandKeyMask = 0x0008;

using Event = std::array<uint8_t, EVENT_SIZE>;
static std::deque<Event> g_queue;
static uint32_t g_form_handler = 0;
static uint32_t g_key_mask = 0xFFFFFFFF;
static uint32_t g_prev_keys = 0;
static bool g_prev_pen = false;
static bool g_stop_sent = false;

static Event make_event(uint16_t type) {
    Event e{};
    memcpy(&e[0], &type, 2);
    return e;
}
static void push_key(uint16_t chr) {
    Event e = make_event(keyDownEvent);
    uint16_t mod = commandKeyMask;
    memcpy(&e[8], &chr, 2);
    memcpy(&e[12], &mod, 2);
    g_queue.push_back(e);
}

// Map a key bit to the virtual character the device would post.
static uint16_t vchr_for_bit(uint32_t bit) {
    switch (bit) {
        case 0x00010000: return 0x0132;   // vchrRockerUp
        case 0x00020000: return 0x0133;
        case 0x00040000: return 0x0134;
        case 0x00080000: return 0x0135;
        case 0x00100000: return 0x0136;   // center
        case 0x00008000: return 0x0131;   // vchrFunction (thumb wheel back)
        case 0x00800000: return 0x0108;   // vchrLaunch (Home)
        case 0x04000000: return 0x1B04;   // vchrTriggerLeft
        case 0x08000000: return 0x1B05;   // vchrTriggerRight
        case 0x10000000: return 0x1B06;   // vchrActionUp (A)
        case 0x20000000: return 0x1B07;   // vchrActionRight (B)
        case 0x40000000: return 0x1B08;   // vchrActionDown (C)
        case 0x80000000: return 0x1B09;   // vchrActionLeft (D)
    }
    return 0;
}

// Turn host input changes into Palm events.
static void poll_input() {
    host_pump();
    uint32_t keys = g_input.keys;
    uint32_t pressed = keys & ~g_prev_keys & g_key_mask;
    for (uint32_t bit = 1; bit; bit <<= 1)
        if (pressed & bit) { uint16_t c = vchr_for_bit(bit); if (c) push_key(c); }
    g_prev_keys = keys;
    if (g_input.pen_down != g_prev_pen || (g_input.pen_down && 0)) {
        Event e = make_event(g_input.pen_down ? penDownEvent : penUpEvent);
        e[2] = g_input.pen_down;
        memcpy(&e[4], &g_input.pen_x, 2);
        memcpy(&e[6], &g_input.pen_y, 2);
        g_queue.push_back(e);
        g_prev_pen = g_input.pen_down;
    }
    // Quitting works like the Home button: the application sees keyDown vchrLaunch first (many
    // games quit on it, e.g. Tomb Raider), SysHandleEvent turns it into appStopEvent. If nobody
    // has passed it to SysHandleEvent after 1 s, appStopEvent is posted directly.
    static bool home_sent = false;
    if ((g.quit || g.stop_requested) && !home_sent) {
        home_sent = true;
        push_key(0x0108);   // vchrLaunch
    }
    if ((g.quit || (g.stop_requested && now_ms() - g.stop_time > 1000)) && !g_stop_sent) {
        g_stop_sent = true;
        g_queue.push_back(make_event(appStopEvent));
    }
}

static void get_event(uint32_t evP, int32_t timeout) {
    gfx::frame_tick();
    poll_input();
    uint32_t start = now_ms();
    while (g_queue.empty()) {
        if (timeout == 0 || g.quit || g.stop_requested) break;
        if (timeout > 0 && now_ms() - start >= (uint32_t)timeout) break;
        host_sleep(1);
        gfx::frame_tick();
        poll_input();
    }
    Event e = g_queue.empty() ? make_event(nilEvent) : g_queue.front();
    if (!g_queue.empty()) g_queue.pop_front();
    if (g_input.pen_down) { e[2] = 1; }
    mem_write(evP, e.data(), EVENT_SIZE);
}

HLE(EvtGetEvent) { get_event(a[0], (int32_t)a[1]); return 0; }
HLE(EvtEventAvail) { poll_input(); return !g_queue.empty(); }
HLE(EvtAddEventToQueue) {
    Event e;
    mem_read(a[0], e.data(), EVENT_SIZE);
    g_queue.push_back(e);
    return 0;
}
HLE(EvtAddUniqueEventToQueue) {
    Event e;
    mem_read(a[0], e.data(), EVENT_SIZE);
    for (auto& q : g_queue)
        if (q[0] == e[0] && q[1] == e[1]) { if (a[2] & 0xFF) q = e; return 0; }
    g_queue.push_back(e);
    return 0;
}
HLE(EvtGetPen) {
    host_pump();
    if (a[0]) w16(a[0], g_input.pen_x);
    if (a[1]) w16(a[1], g_input.pen_y);
    if (a[2]) w8(a[2], g_input.pen_down);
    return 0;
}
HLE(EvtGetPenNative) {
    host_pump();
    if (a[1]) w16(a[1], g_input.pen_x);
    if (a[2]) w16(a[2], g_input.pen_y);
    if (a[3]) w8(a[3], g_input.pen_down);
    return 0;
}
HLE(EvtDequeuePenPoint) { return 1; }

HLE(KeyCurrentState) { host_pump(); return g_input.keys; }
HLE(KeySetMask) { uint32_t old = g_key_mask; g_key_mask = a[0]; return old; }

// ---- event dispatch --------------------------------------------------------------
static uint16_t ev_type(uint32_t evP) { return r16(evP); }
static bool sys_handle(uint32_t evP) {
    if (ev_type(evP) == keyDownEvent && r16(evP + 8) == 0x0108) {   // Home -> quit the app
        if (!g_stop_sent) { g_stop_sent = true; g_queue.push_back(make_event(appStopEvent)); }
        return true;
    }
    return false;
}
static bool frm_dispatch(uint32_t evP) {
    if (!g_form_handler) return false;
    if (g.opt.trace && r16(evP) != nilEvent) LOG("  -> form handler(event %u)", r16(evP));
    return (cpu_call(g_form_handler, {evP}) & 0xFF) != 0;
}
HLE(SysHandleEvent) { return sys_handle(a[0]); }
HLE(MenuHandleEvent) { if (a[2]) w16(a[2], 0); return 0; }
HLE(FrmDispatchEvent) { return frm_dispatch(a[0]); }
// winDisplayChangedEvent (0x4101, PenInputMgr.h): posted when the input area, status bar or
// orientation changes the display bounds. Some games (Xploids) wait for it before starting.
void post_display_changed(int w, int h) {
    for (auto& q : g_queue) if (q[0] == 0x01 && q[1] == 0x41) return;   // already pending
    Event e = make_event(0x4101);
    int16_t r[4] = {0, 0, (int16_t)w, (int16_t)h};
    memcpy(&e[8], r, 8);
    g_queue.push_back(e);
}

// A full-screen form/window pair standing in for the one TwAppStart creates.
static uint32_t g_form = 0, g_window = 0;
uint32_t ui_display_window() {
    if (!g_window) { g_window = heap::alloc(64, true); g_form = heap::alloc(128, true); w32(g_form, g_window); }
    return g_window;
}
HLE(FrmGetActiveForm) { ui_display_window(); return g_form_handler ? g_form : 0; }
HLE(FrmGetWindowHandle) { return a[0] == g_form ? ui_display_window() : 0; }
HLE(WinGetDisplayWindow) { return ui_display_window(); }
HLE(WinGetActiveWindow) { return ui_display_window(); }

HLE(TwAppStart) {
    g_form_handler = a[0];
    // The framework opens a full-screen form; deliver the usual open events.
    Event e = make_event(frmLoadEvent);
    g_queue.push_back(e);
    e = make_event(frmOpenEvent);
    g_queue.push_back(e);
    e = make_event(winEnterEvent);   // the form window gets the focus
    uint32_t w = ui_display_window();
    memcpy(&e[8], &w, 4);
    g_queue.push_back(e);
    return 0;
}
HLE(TwAppStop) { g_form_handler = 0; return 0; }
HLE(TwAppRun) {
    uint32_t timeoutP = a[0];
    uint32_t evP = heap::alloc(EVENT_SIZE, true);
    uint32_t errP = heap::alloc(4, true);
    for (;;) {
        int32_t timeout = timeoutP ? (int32_t)r32(timeoutP) : -1;
        static int32_t last_timeout = 12345;
        if (g.opt.trace && timeout != last_timeout) { LOG("  TwAppRun timeout = %d", timeout); last_timeout = timeout; }
        get_event(evP, timeout);
        if (!sys_handle(evP)) frm_dispatch(evP);
        // The loop ends with appStopEvent, after the form handler has seen it.
        if (ev_type(evP) == appStopEvent || g.quit) break;
    }
    heap::free(evP);
    heap::free(errP);
    return 0;
}
HLE(TwNextEvent) {
    get_event(a[0], (int32_t)a[1]);
    if (sys_handle(a[0]) || frm_dispatch(a[0])) w16(a[0], nilEvent);
    return 0;
}
HLE(TwConfirmQuit) { return 1; }

// ---- TwInput ------------------------------------------------------------------------
struct InputQueue { std::vector<int32_t> fmt; bool active = false; uint32_t seq = 0; };
static std::map<uint32_t, InputQueue> g_inputs;
static uint32_t g_next_input = 1;

static int32_t input_value(int32_t f, uint32_t seq) {
    uint32_t k = g_input.keys;
    switch (f) {
        case 256: return g_input.pen_x;
        case 257: return g_input.pen_y;
        case 258: return g_input.pen_down ? 255 : 0;
        case 259: return g_input.joy_x;
        case 260: return g_input.joy_y;
        case 261: return (int32_t)k;
        case 262: case 263: {
            bool u = k & 0x10000, d = k & 0x20000, l = k & 0x40000, r = k & 0x80000;
            if (f == 262) return u ? 1 : r ? 3 : d ? 5 : l ? 7 : 0;
            if (u && r) return 2; if (d && r) return 4; if (d && l) return 6; if (u && l) return 8;
            return u ? 1 : r ? 3 : d ? 5 : l ? 7 : 0;
        }
        case 270: return !!(k & 0x00800000);
        case 271: return 0;
        case 272: return !!(k & 0x01000000);
        case 273: return !!(k & 0x00008000);
        case 274: return !!(k & 0x10000000);
        case 275: return !!(k & 0x20000000);
        case 276: return !!(k & 0x40000000);
        case 277: return !!(k & 0x80000000);
        case 282: return !!(k & 0x04000000);
        case 283: return !!(k & 0x08000000);
        case 284: return !!(k & 0x10000);
        case 285: return !!(k & 0x20000);
        case 286: return !!(k & 0x40000);
        case 287: return !!(k & 0x80000);
        case 288: return !!(k & 0x100000);
        case 290: case 291: case 292: case 293: return 0;
        case 294: return (int32_t)uptime_ms();
        case 295: return (int32_t)seq;
        // Polar stick: R in 0..32767, Theta in degrees 0..359. The Programmer's Reference says
        // "counter-clockwise from the right", but Xploids (verified by hand) expects screen
        // coordinates like NavY: right = 0, down = 90, up = 270.
        // CircX/CircY = R*cos/R*sin: documented but missing from TwInput.h, assumed 296/297.
        case 296: case 297: case 298: case 299: {
            double x = g_input.joy_x, y = g_input.joy_y;
            double r = std::min(32767.0, std::sqrt(x * x + y * y));
            if (r < 1) return 0;
            double th = std::atan2(y, x);
            if (f == 298) return (int32_t)r;
            if (f == 299) { int d = (int)std::lround(th * 180.0 / M_PI); return (d % 360 + 360) % 360; }
            return (int32_t)std::lround(r * (f == 296 ? std::cos(th) : std::sin(th)));
        }
    }
    return 0;
}

HLE(TwInputOpen) { uint32_t h = g_next_input++; g_inputs[h] = {}; w32(a[0], h); return 0; }
HLE(TwInputClose) { g_inputs.erase(a[0]); return 0; }
HLE(TwInputActivate) { g_inputs[a[0]].active = true; return 0; }
HLE(TwInputDeactivate) { g_inputs[a[0]].active = false; return 0; }
HLE(TwInputGetPeriod) { if (a[1]) w32(a[1], 10); return 0; }
HLE(TwInputSetPeriod) { return 0; }
HLE(TwInputGetCapacity) { if (a[1]) w32(a[1], 0); return 0; }
HLE(TwInputSetCapacity) { return 0; }
HLE(TwInputSetFormat) {
    InputQueue& q = g_inputs[a[0]];
    q.fmt.clear();
    for (uint32_t i = 0; i < a[2] / 4; i++) q.fmt.push_back((int32_t)r32(a[1] + i * 4));
    if (g.opt.trace) {
        std::string f;
        for (int32_t v : q.fmt) f += std::to_string(v) + " ";
        LOG("  TwInput format: %s", f.c_str());
    }
    return 0;
}
HLE(TwInputGetFormat) {
    InputQueue& q = g_inputs[a[0]];
    uint32_t cap = a[2] ? r32(a[2]) / 4 : 0;
    for (uint32_t i = 0; i < q.fmt.size() && i < cap; i++) w32(a[1] + i * 4, (uint32_t)q.fmt[i]);
    if (a[2]) w32(a[2], (uint32_t)q.fmt.size() * 4);
    return 0;
}
static uint32_t fill_input(uint32_t h, uint32_t ev, uint32_t size) {
    host_pump();
    InputQueue& q = g_inputs[h];
    q.seq++;
    for (uint32_t i = 0; i < q.fmt.size() && i * 4 < size; i++) w32(ev + i * 4, (uint32_t)input_value(q.fmt[i], q.seq));
    return 0;
}
HLE(TwInputPoll) { return fill_input(a[0], a[1], a[2]); }
HLE(TwInputPeek) { return 11; }   // EAGAIN: we only support zero-capacity (polling) queues
HLE(TwInputRead) { return 11; }
HLE(TwInputControl) { return 0; }
