// TwMidi / TwSmf / SndPlaySmf HLE. Standard MIDI files are sequenced on a host thread and sent
// to the Windows GS synthesizer (winmm midiOut). Completion callbacks run on the emulation
// thread at safe points (midi_pump).
#include "palmos.h"
#include "host.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#endif

constexpr uint32_t twMidiErrorBase = 0x7000 + 0x500;   // oemErrorClass + 0x500 (TwError.h)
constexpr uint32_t twMidiErrorInvalidHandle = twMidiErrorBase + 2, twMidiErrorInvalidFormat = twMidiErrorBase + 5;

struct MidiEvent { uint32_t ms; uint32_t msg; };   // short message (status | d1<<8 | d2<<16)
struct Song {
    std::vector<MidiEvent> events;
    uint32_t duration = 0;
    // playback state (guarded by g_mutex)
    bool playing = false;
    size_t next = 0;
    std::chrono::steady_clock::time_point start;
    uint32_t start_ms = 0, end_ms = 0;
    int amplitude = 64;   // 0..64
    uint32_t cb_func = 0, cb_user = 0;
};

static std::mutex g_mutex;
static std::map<uint32_t, Song*> g_songs;          // TwSmfHandle -> song
static std::vector<std::pair<uint32_t, uint32_t>> g_pending_callbacks;
static std::atomic<int> g_master{127};
static std::thread g_thread;
static std::atomic<bool> g_run{false};
#ifdef _WIN32
static HMIDIOUT g_out = nullptr;
#endif

static void send(uint32_t msg) {
#ifdef _WIN32
    if (g_out) midiOutShortMsg(g_out, msg);
#endif
}
static void all_notes_off() {
    for (int ch = 0; ch < 16; ch++) { send(0xB0 | ch | (123 << 8)); send(0xB0 | ch | (120 << 8)); }
}

// ---- SMF parsing -------------------------------------------------------------------------
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }

static bool parse_smf(const uint8_t* d, size_t n, Song& song) {
    if (n < 14 || memcmp(d, "MThd", 4) != 0) return false;
    uint32_t hlen = be32(d + 4);
    uint16_t ntracks = be16(d + 10), division = be16(d + 12);
    if (division & 0x8000) return false;   // SMPTE time not supported
    struct Raw { uint64_t tick; int order; uint32_t msg; uint32_t tempo; };
    std::vector<Raw> raw;
    size_t p = 8 + hlen;
    int order = 0;
    for (int t = 0; t < ntracks && p + 8 <= n; t++) {
        if (memcmp(d + p, "MTrk", 4) != 0) break;
        size_t len = be32(d + p + 4), q = p + 8, end = std::min(n, p + 8 + len);
        uint64_t tick = 0;
        uint8_t status = 0;
        while (q < end) {
            uint32_t delta = 0;
            for (int k = 0; k < 4 && q < end; k++) { uint8_t b = d[q++]; delta = delta << 7 | (b & 0x7F); if (!(b & 0x80)) break; }
            tick += delta;
            if (q >= end) break;
            uint8_t b = d[q];
            if (b & 0x80) { status = b; q++; }
            if (status == 0xFF) {
                uint8_t type = d[q++];
                uint32_t ml = 0;
                for (int k = 0; k < 4 && q < end; k++) { uint8_t c = d[q++]; ml = ml << 7 | (c & 0x7F); if (!(c & 0x80)) break; }
                if (type == 0x51 && ml == 3) raw.push_back({tick, order++, 0, (uint32_t)(d[q] << 16 | d[q + 1] << 8 | d[q + 2])});
                if (type == 0x2F) { q = end; break; }
                q += ml;
            } else if (status == 0xF0 || status == 0xF7) {
                uint32_t ml = 0;
                for (int k = 0; k < 4 && q < end; k++) { uint8_t c = d[q++]; ml = ml << 7 | (c & 0x7F); if (!(c & 0x80)) break; }
                q += ml;
            } else {
                int nd = ((status & 0xE0) == 0xC0) ? 1 : 2;
                uint32_t msg = status;
                for (int k = 0; k < nd && q < end; k++) msg |= (uint32_t)d[q++] << (8 * (k + 1));
                raw.push_back({tick, order++, msg, 0});
            }
        }
        p += 8 + len;
    }
    std::stable_sort(raw.begin(), raw.end(), [](const Raw& a, const Raw& b) { return a.tick < b.tick; });
    double us_per_tick = 500000.0 / division, ms = 0;
    uint64_t last = 0;
    for (auto& r : raw) {
        ms += (r.tick - last) * us_per_tick / 1000.0;
        last = r.tick;
        if (r.tempo) us_per_tick = (double)r.tempo / division;
        else song.events.push_back({(uint32_t)ms, r.msg});
    }
    song.duration = (uint32_t)ms;
    return true;
}

static Song* parse_at(uint32_t addr) {
    // SMF size is not passed: parse the header and walk the track chunks to find the end.
    if (!addr || r32(addr) != 0x6468544D) return nullptr;   // "MThd"
    uint32_t hlen = __builtin_bswap32(r32(addr + 4));
    uint16_t ntr = (uint16_t)(r8(addr + 10) << 8 | r8(addr + 11));
    uint32_t total = 8 + hlen;
    for (int t = 0; t < ntr; t++) total += 8 + __builtin_bswap32(r32(addr + total + 4));
    std::vector<uint8_t> buf(total);
    mem_read(addr, buf.data(), total);
    Song* s = new Song;
    if (!parse_smf(buf.data(), buf.size(), *s)) { delete s; return nullptr; }
    return s;
}

// ---- sequencer thread ---------------------------------------------------------------------
static void sequencer() {
    while (g_run) {
        {
            std::lock_guard<std::mutex> lk(g_mutex);
            auto now = std::chrono::steady_clock::now();
            for (auto& [h, s] : g_songs) {
                if (!s->playing) continue;
                uint32_t pos = s->start_ms + (uint32_t)std::chrono::duration_cast<std::chrono::milliseconds>(now - s->start).count();
                while (s->next < s->events.size() && s->events[s->next].ms <= pos) {
                    uint32_t m = s->events[s->next++].msg;
                    if ((m & 0xF0) == 0x90 && (m >> 16 & 0x7F)) {   // scale note-on velocity
                        uint32_t v = (m >> 16 & 0x7F) * s->amplitude * g_master / (64 * 127);
                        m = (m & 0xFFFF) | (std::max<uint32_t>(v, 1) << 16);
                    }
                    send(m);
                }
                if (s->next >= s->events.size() || pos >= s->end_ms) {
                    s->playing = false;
                    all_notes_off();
                    if (s->cb_func) g_pending_callbacks.push_back({s->cb_func, s->cb_user});
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

static void ensure_started() {
    if (g_run) return;
#ifdef _WIN32
    if (!g.opt.headless && midiOutOpen(&g_out, MIDI_MAPPER, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        LOG("midiOutOpen failed: MIDI music disabled");
        g_out = nullptr;
    }
#endif
    g_run = true;
    g_thread = std::thread(sequencer);
}

void midi_shutdown() {
    if (!g_run) return;
    g_run = false;
    g_thread.join();
    all_notes_off();
#ifdef _WIN32
    if (g_out) midiOutClose(g_out);
    g_out = nullptr;
#endif
}

bool sems_all_free();
// Run completion callbacks on the emulation thread.
void midi_pump() {
    std::vector<std::pair<uint32_t, uint32_t>> cbs;
    {
        std::lock_guard<std::mutex> lk(g_mutex);
        if (g_pending_callbacks.empty() || !sems_all_free()) return;
        cbs.swap(g_pending_callbacks);
    }
    for (auto& [f, u] : cbs) cpu_call(f, {u});
}

static void start_song(Song* s, uint32_t optsP, uint32_t cbP) {
    std::lock_guard<std::mutex> lk(g_mutex);
    s->start_ms = optsP ? r32(optsP) : 0;
    uint32_t endms = optsP ? r32(optsP + 4) : 0xFFFFFFFF;
    s->end_ms = endms ? endms : 0xFFFFFFFF;
    s->amplitude = optsP ? std::min<int>(r16(optsP + 8), 64) : 64;
    s->cb_func = cbP ? r32(cbP) : 0;
    s->cb_user = cbP ? r32(cbP + 4) : 0;
    s->next = 0;
    while (s->next < s->events.size() && s->events[s->next].ms < s->start_ms) s->next++;
    s->start = std::chrono::steady_clock::now();
    s->playing = true;
}

// ---- TwSmf ------------------------------------------------------------------------------------
HLE(TwSmfOpen) {
    Song* s = parse_at(a[1]);
    if (!s) { LOG("TwSmfOpen: invalid SMF at %08x", a[1]); return twMidiErrorInvalidFormat; }
    ensure_started();
    uint32_t h = heap::alloc(8, true);
    { std::lock_guard<std::mutex> lk(g_mutex); g_songs[h] = s; }
    w32(a[0], h);
    if (a[2]) w32(a[2], s->duration);
    return 0;
}
HLE(TwSmfClose) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_songs.find(a[0]);
    if (it == g_songs.end()) return twMidiErrorInvalidHandle;
    if (it->second->playing) all_notes_off();
    delete it->second;
    g_songs.erase(it);
    heap::free(a[0]);
    return 0;
}
HLE(TwSmfPlay) {
    Song* s;
    { std::lock_guard<std::mutex> lk(g_mutex); auto it = g_songs.find(a[0]); if (it == g_songs.end()) return twMidiErrorInvalidHandle; s = it->second; }
    start_song(s, a[1], a[3]);
    return 0;
}
HLE(TwSmfIsPlaying) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_songs.find(a[0]);
    if (it == g_songs.end()) return twMidiErrorInvalidHandle;
    if (a[1]) w8(a[1], it->second->playing);
    return 0;
}
HLE(TwSmfStop) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_songs.find(a[0]);
    if (it == g_songs.end()) return twMidiErrorInvalidHandle;
    if (it->second->playing) { it->second->playing = false; all_notes_off(); }
    return 0;
}
HLE(TwMidiPlaySmf) {
    Song* s = parse_at(a[0]);
    if (!s) return twMidiErrorInvalidFormat;
    ensure_started();
    if (a[1]) w32(a[1], s->duration);
    uint32_t h = heap::alloc(8, true);
    { std::lock_guard<std::mutex> lk(g_mutex); g_songs[h] = s; }
    start_song(s, 0, 0);
    if (!(a[2] & 0xFF)) {
        for (;;) {
            { std::lock_guard<std::mutex> lk(g_mutex); if (!s->playing) break; }
            if (!host_pump()) break;
            host_sleep(5);
        }
    }
    return 0;
}

// ---- TwMidi real-time -----------------------------------------------------------------------------
HLE(TwMidiOpen) { ensure_started(); w32(a[0], heap::alloc(8, true)); return 0; }
HLE(TwMidiClose) { if (a[0]) heap::free(a[0]); return 0; }
HLE(TwMidiNoteOn) { send(0x90 | (a[1] & 15) | (a[2] & 0x7F) << 8 | (a[3] & 0x7F) << 16); return 0; }
HLE(TwMidiNoteOff) { send(0x80 | (a[1] & 15) | (a[2] & 0x7F) << 8 | (a[3] & 0x7F) << 16); return 0; }
HLE(TwMidiProgramChange) { send(0xC0 | (a[1] & 15) | (a[2] & 0x7F) << 8); return 0; }
HLE(TwMidiPitchBend) {
    int32_t v = (int32_t)a[2] + 8192;
    v = v < 0 ? 0 : v > 16383 ? 16383 : v;
    send(0xE0 | (a[1] & 15) | (v & 0x7F) << 8 | (v >> 7) << 16);
    return 0;
}
HLE(TwMidiControlChange) {
    static const uint8_t cc[] = {1, 7, 10, 123};   // modulation, volume, pan, all notes off
    int32_t c = (int32_t)a[2];
    send(0xB0 | (a[1] & 15) | (c >= 0 && c < 4 ? cc[c] : (c & 0x7F)) << 8 | (a[3] & 0x7F) << 16);
    return 0;
}
HLE(TwMidiSysEx) { return 0; }
HLE(TwMidiSetMasterVolume) { g_master = std::min<int32_t>(std::max<int32_t>((int32_t)a[0], 0), 127); return 0; }
HLE(TwMidiGetMasterVolume) { if (a[0]) w32(a[0], (uint32_t)g_master.load()); return 0; }
HLE(TwMidiGetLimits) { if (a[1]) w32(a[1], a[0] == 0 ? 8 : 4); return 0; }

// ---- Palm OS SMF ---------------------------------------------------------------------------------
HLE(SndPlaySmf) {
    // cmd 0 = play, 1 = duration
    Song* s = parse_at(a[2]);
    if (!s) return sndErrorClass | 3;
    if (a[1] == 1) { if (a[3]) w32(a[3] + 4, s->duration); delete s; return 0; }
    ensure_started();
    uint32_t h = heap::alloc(8, true);
    { std::lock_guard<std::mutex> lk(g_mutex); g_songs[h] = s; }
    start_song(s, a[3], a[5]);   // completion callback is the first member of SndSmfCallbacksType
    return 0;
}
HLE(SndPlaySmfResource) {
    uint32_t hnd = dm::find_resource_any(a[0], (uint16_t)a[1]);
    if (!hnd) return dmErrResourceNotFound;
    Song* s = parse_at(heap::deref(hnd));
    if (!s) return sndErrorClass | 3;
    ensure_started();
    uint32_t h = heap::alloc(8, true);
    { std::lock_guard<std::mutex> lk(g_mutex); g_songs[h] = s; }
    start_song(s, 0, 0);
    return 0;
}
