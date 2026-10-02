// Sound Manager HLE: SndStream* (ARM callbacks pulled from the emulation thread), SndPlayResource,
// Tapwave volume APIs. Output: SDL queue-based device, 44.1 kHz stereo.
#include "audio.h"
#include "palmos.h"
#include <SDL2/SDL.h>
#include <cmath>
#include <deque>
#include <map>
#include <vector>

constexpr int OUT_RATE = 44100;
constexpr int TARGET_MS = 120;         // keep this much audio queued
constexpr int CHUNK = 512;             // output frames per mixing step

struct Stream {
    uint32_t ref, callback, userdata;
    uint32_t rate, type;
    bool stereo, variable, running = false, paused = false;
    int32_t volume = 1024, pan = 0;
    uint32_t buf = 0, bufFrames = 0;
    std::deque<float> fifo;            // interleaved L/R at the stream rate
    double pos = 0;                    // fractional read position (frames) in fifo
};
struct Voice { std::vector<float> lr; uint32_t rate; double pos = 0; float vol = 1; };

static SDL_AudioDeviceID g_dev = 0;
static std::map<uint32_t, Stream> g_streams;
static std::vector<Voice> g_voices;
static uint32_t g_next_ref = 0x5100;
static bool g_in_pump = false;
static uint64_t g_produced = 0;        // output frames generated (headless clock)
static uint32_t g_start_ms = 0;
static uint16_t g_master = 64;         // TwSnd primary volume 0..64
static bool g_mute = false;
static uint32_t g_last_pump = 0;
static uint32_t g_underruns = 0, g_chunks = 0, g_late = 0;

bool sems_all_free();   // vfs.cpp

namespace audio {

void init() {
    g_start_ms = now_ms();
    if (g.opt.headless) return;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) { LOG("SDL audio: %s", SDL_GetError()); return; }
    SDL_AudioSpec want{}, have{};
    want.freq = OUT_RATE; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 512;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) { LOG("SDL_OpenAudioDevice: %s", SDL_GetError()); return; }
    SDL_PauseAudioDevice(g_dev, 0);
}
void shutdown() {
    if (g_chunks) LOG("audio: %u chunks, %u stream underruns, %u device starvations", g_chunks, g_underruns, g_late);
    if (g_dev) SDL_CloseAudioDevice(g_dev);
    g_dev = 0;
}

static float sample(uint32_t addr, uint32_t type) {
    switch (type) {
        case 0x01: return (int8_t)r8(addr) / 128.0f;
        case 0x11: return ((int)r8(addr) - 128) / 128.0f;
        case 0x02: { uint16_t v = r16(addr); return (int16_t)(v >> 8 | v << 8) / 32768.0f; }
        case 0x12: return (int16_t)r16(addr) / 32768.0f;
        case 0x14: return (int32_t)r32(addr) / 2147483648.0f;
        case 0x04: return (int32_t)__builtin_bswap32(r32(addr)) / 2147483648.0f;
        case 0x34: { uint32_t v = r32(addr); float f; memcpy(&f, &v, 4); return f; }
        case 0x24: { uint32_t v = __builtin_bswap32(r32(addr)); float f; memcpy(&f, &v, 4); return f; }
    }
    return 0;
}
static int sample_bytes(uint32_t type) { return (type & 0x0F) == 1 ? 1 : (type & 0x0F) == 2 ? 2 : 4; }

// Ask the ARM callback for one buffer and append it to the fifo.
static bool refill(Stream& s) {
    static bool first = true;
    if (first) { first = false; LOG("audio: first callback for stream %x", s.ref); }
    int ch = s.stereo ? 2 : 1, sb = sample_bytes(s.type);
    uint32_t bytes = s.bufFrames * ch * sb;
    if (!s.buf) s.buf = heap::alloc(bytes, true);
    uint32_t frames = s.bufFrames;
    if (s.variable) {
        uint32_t szP = heap::alloc(4, true);
        w32(szP, bytes);
        cpu_call(s.callback, {s.userdata, s.ref, s.buf, szP});
        frames = r32(szP) / (ch * sb);
        heap::free(szP);
    } else {
        mem_fill(s.buf, 0, bytes);
        cpu_call(s.callback, {s.userdata, s.ref, s.buf, s.bufFrames});
    }
    if (frames > s.bufFrames) frames = s.bufFrames;
    for (uint32_t i = 0; i < frames; i++) {
        float l = sample(s.buf + (i * ch) * sb, s.type);
        float r = s.stereo ? sample(s.buf + (i * ch + 1) * sb, s.type) : l;
        s.fifo.push_back(l);
        s.fifo.push_back(r);
    }
    return frames > 0;
}

// Make sure every running stream can supply the next chunk. Streams with variable-size callbacks
// (e.g. Doom) only return what the game has produced so far; asking too far ahead starves them.
static bool streams_ready() {
    for (auto& [ref, s] : g_streams) {
        if (!s.running || s.paused) continue;
        double step = (double)s.rate / OUT_RATE;
        size_t need = (size_t)(s.pos + step * CHUNK) + 2;
        int guard = 0;
        while (s.fifo.size() / 2 < need && guard++ < 64) if (!refill(s) || g.quit) break;
        if (s.fifo.size() / 2 < need) return false;
    }
    return true;
}

static void mix_chunk(std::vector<float>& out) {
    out.assign(CHUNK * 2, 0.0f);
    for (auto& [ref, s] : g_streams) {
        if (!s.running || s.paused) continue;
        double step = (double)s.rate / OUT_RATE;
        float vol = s.volume / 1024.0f;
        float pl = s.pan > 0 ? 1.0f - s.pan / 1024.0f : 1.0f, pr = s.pan < 0 ? 1.0f + s.pan / 1024.0f : 1.0f;
        size_t avail = s.fifo.size() / 2;
        for (int i = 0; i < CHUNK; i++) {
            size_t i0 = (size_t)s.pos;
            if (i0 + 1 >= avail) { g_underruns++; break; }
            float f = (float)(s.pos - i0);
            out[i * 2] += (s.fifo[i0 * 2] * (1 - f) + s.fifo[i0 * 2 + 2] * f) * vol * pl;
            out[i * 2 + 1] += (s.fifo[i0 * 2 + 1] * (1 - f) + s.fifo[i0 * 2 + 3] * f) * vol * pr;
            s.pos += step;
        }
        size_t drop = std::min((size_t)s.pos, avail);
        s.fifo.erase(s.fifo.begin(), s.fifo.begin() + drop * 2);
        s.pos -= drop;
    }
    for (auto it = g_voices.begin(); it != g_voices.end();) {
        double step = (double)it->rate / OUT_RATE;
        size_t n = it->lr.size() / 2;
        for (int i = 0; i < CHUNK && (size_t)it->pos + 1 < n; i++) {
            size_t i0 = (size_t)it->pos;
            out[i * 2] += it->lr[i0 * 2] * it->vol;
            out[i * 2 + 1] += it->lr[i0 * 2 + 1] * it->vol;
            it->pos += step;
        }
        if ((size_t)it->pos + 1 >= n) it = g_voices.erase(it); else ++it;
    }
}

void pump() {
    if (g_in_pump || g.quit) return;
    uint32_t t = now_ms();
    if (t - g_last_pump < 4) return;
    g_last_pump = t;
    bool any = !g_voices.empty();
    for (auto& [r, s] : g_streams) any |= s.running && !s.paused;
    if (!any) {
        if (!g_dev) g_produced = (uint64_t)(t - g_start_ms) * OUT_RATE / 1000;
        return;
    }
    if (!sems_all_free()) return;   // the game is inside a critical section
    g_in_pump = true;
    std::vector<float> mix;
    std::vector<int16_t> pcm(CHUNK * 2);
    float master = g_mute ? 0.0f : g_master / 64.0f;
    for (int iter = 0; iter < 16; iter++) {
        uint64_t queued_frames;
        if (g_dev) queued_frames = SDL_GetQueuedAudioSize(g_dev) / 4;
        else {
            uint64_t consumed = (uint64_t)(now_ms() - g_start_ms) * OUT_RATE / 1000;
            queued_frames = g_produced > consumed ? g_produced - consumed : 0;
            if (g_produced < consumed) g_produced = consumed;
        }
        if (queued_frames >= (uint64_t)OUT_RATE * TARGET_MS / 1000) break;
        if (g_dev && queued_frames == 0 && g_chunks) g_late++;   // device ran dry before we refilled
        // Not enough game audio yet: wait for the next pump unless the device is about to run dry.
        if (!streams_ready() && queued_frames >= (uint64_t)OUT_RATE * 20 / 1000) break;
        mix_chunk(mix);
        g_chunks++;
        for (int i = 0; i < CHUNK * 2; i++) {
            float v = mix[i] * master;
            v = v > 1 ? 1 : v < -1 ? -1 : v;
            pcm[i] = (int16_t)(v * 32767);
        }
        if (g_dev) SDL_QueueAudio(g_dev, pcm.data(), (Uint32)(pcm.size() * 2));
        g_produced += CHUNK;
        if (g.quit) break;
    }
    g_in_pump = false;
}

void play_pcm(const std::vector<float>& lr, uint32_t rate, float vol) {
    Voice v;
    v.lr = lr; v.rate = rate; v.vol = vol;
    g_voices.push_back(std::move(v));
}

}  // namespace audio

// ---- SndStream ------------------------------------------------------------------
static uint32_t create(uint32_t refP, uint32_t mode, uint32_t rate, uint32_t type, uint32_t width, uint32_t cb,
                       uint32_t user, uint32_t frames, bool variable) {
    if (mode != 1) { LOG("SndStreamCreate: input streams not supported"); return sndErrorClass | 1; }
    Stream s;
    s.ref = g_next_ref++;
    s.callback = cb; s.userdata = user; s.rate = rate ? rate : 22050; s.type = type ? type : 0x12;
    s.stereo = width == 1; s.variable = variable;
    s.bufFrames = frames ? frames : 1024;
    if (variable) s.bufFrames = frames ? frames / ((s.stereo ? 2 : 1) * (s.type & 0x0F ? (s.type & 0x0F) : 2)) : 1024;
    if (s.bufFrames < 64) s.bufFrames = 64;
    if (s.bufFrames > 16384) s.bufFrames = 16384;
    g_streams[s.ref] = s;
    w32(refP, s.ref);
    LOG("SndStreamCreate: ref %x %u Hz type %02x %s, %u frames/buffer%s", s.ref, s.rate, s.type,
        s.stereo ? "stereo" : "mono", s.bufFrames, variable ? " (variable)" : "");
    return 0;
}
HLE(SndStreamCreate) { return create(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], false); }
HLE(SndStreamCreateExtended) {
    if (a[2] != 0) { LOG("SndStreamCreateExtended: format '%s' not supported", fourcc_str(a[2]).c_str()); return sndErrorClass | 1; }
    return create(a[0], a[1], a[3], a[4], a[5], a[6], a[7], a[8], true);
}
static Stream* st(uint32_t ref) { auto it = g_streams.find(ref); return it == g_streams.end() ? nullptr : &it->second; }
HLE(SndStreamDelete) {
    Stream* s = st(a[0]);
    if (!s) return sndErrorClass | 2;
    if (s->buf) heap::free(s->buf);
    g_streams.erase(a[0]);
    return 0;
}
HLE(SndStreamStart) { Stream* s = st(a[0]); if (!s) return sndErrorClass | 2; s->running = true; s->paused = false; return 0; }
HLE(SndStreamStop) { Stream* s = st(a[0]); if (!s) return sndErrorClass | 2; s->running = false; s->fifo.clear(); s->pos = 0; return 0; }
HLE(SndStreamPause) { Stream* s = st(a[0]); if (!s) return sndErrorClass | 2; s->paused = a[1] & 0xFF; return 0; }
HLE(SndStreamSetVolume) { Stream* s = st(a[0]); if (!s) return sndErrorClass | 2; s->volume = (int32_t)a[1]; return 0; }
HLE(SndStreamGetVolume) { Stream* s = st(a[0]); if (!s) return sndErrorClass | 2; w32(a[1], (uint32_t)s->volume); return 0; }
HLE(SndStreamSetPan) { Stream* s = st(a[0]); if (!s) return sndErrorClass | 2; s->pan = (int32_t)a[1]; return 0; }
HLE(SndStreamGetPan) { Stream* s = st(a[0]); if (!s) return sndErrorClass | 2; w32(a[1], (uint32_t)s->pan); return 0; }
HLE(SndStreamDeviceControl) { return 0; }

// ---- one-shot WAV playback ----------------------------------------------------------
bool decode_wav(uint32_t p, uint32_t maxLen, std::vector<float>& lr, uint32_t& rate) {
    if (r32(p) != 0x46464952 || r32(p + 8) != 0x45564157) return false;   // "RIFF" "WAVE"
    uint32_t end = p + 8 + r32(p + 4), q = p + 12;
    if (maxLen && end > p + maxLen) end = p + maxLen;
    uint16_t fmt = 0, ch = 1, bits = 16;
    rate = 22050;
    while (q + 8 <= end) {
        uint32_t id = r32(q), len = r32(q + 4);
        if (id == 0x20746D66) { fmt = r16(q + 8); ch = r16(q + 10); rate = r32(q + 12); bits = r16(q + 22); }
        else if (id == 0x61746164) {   // data
            if (fmt != 1) { LOG("WAV format %u not supported", fmt); return false; }
            uint32_t bps = bits / 8, frames = len / (bps * ch);
            for (uint32_t i = 0; i < frames; i++) {
                uint32_t s0 = q + 8 + i * bps * ch;
                float l = bits == 8 ? ((int)r8(s0) - 128) / 128.0f : (int16_t)r16(s0) / 32768.0f;
                float r = ch > 1 ? (bits == 8 ? ((int)r8(s0 + 1) - 128) / 128.0f : (int16_t)r16(s0 + 2) / 32768.0f) : l;
                lr.push_back(l); lr.push_back(r);
            }
            return true;
        }
        q += 8 + len + (len & 1);
    }
    return false;
}
HLE(SndPlayResource) {
    std::vector<float> lr;
    uint32_t rate;
    if (!decode_wav(a[0], 0, lr, rate)) { LOG("SndPlayResource: unsupported sound at %08x", a[0]); return sndErrorClass | 3; }
    int32_t vol = (int32_t)a[1];
    float v = vol < 0 ? 1.0f : vol / 1024.0f;
    audio::play_pcm(lr, rate, v);
    return 0;
}

// ---- Tapwave primary volume ------------------------------------------------------------
HLE(TwSndGetVolume) { return g_master; }
HLE(TwSndSetVolume) { if (a[0] > 64) return sysErrParamErr; g_master = (uint16_t)a[0]; return 0; }
HLE(TwSndSetMute) { g_mute = a[0] & 0xFF; return 0; }
HLE(TwSndGetMute) { if (a[0]) w32(a[0], 0); return g_mute; }
HLE(TwSndGetBassBoost) { return 0; }
HLE(TwSndSetBassBoost) { return 0; }
HLE(TwSndPlaySystemSound) { return 0; }
HLE(TwSndSetAlarmPlaying) { return 0; }
HLE(SndGetDefaultVolume) {
    if (a[0]) w16(a[0], 64);
    if (a[1]) w16(a[1], 64);
    if (a[2]) w16(a[2], 64);
    return 0;
}
HLE(SndSetDefaultVolume) { return 0; }
HLE(SndDoCmd) { return 0; }
