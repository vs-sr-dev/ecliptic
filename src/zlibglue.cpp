// zlib exported through TwGlue. The z_stream lives in emulated memory (ARM layout, 56 bytes);
// each call mirrors it into a host z_stream and writes the results back.
#include "palmos.h"
#include <map>
#include <zlib.h>

// ARM z_stream offsets
enum { ZS_NEXT_IN = 0, ZS_AVAIL_IN = 4, ZS_TOTAL_IN = 8, ZS_NEXT_OUT = 12, ZS_AVAIL_OUT = 16, ZS_TOTAL_OUT = 20,
       ZS_MSG = 24, ZS_STATE = 28, ZS_DATA_TYPE = 44, ZS_ADLER = 48 };

struct HostZ { z_stream z{}; bool deflating = false; };
static std::map<uint32_t, HostZ*> g_z;
static const char* kVersion = "1.1.4";   // what the Tapwave SDK zlib.h reports
static uint32_t g_version_str = 0;

static void load_in(uint32_t s, z_stream& z) {
    uint32_t ni = r32(s + ZS_NEXT_IN), ai = r32(s + ZS_AVAIL_IN), no = r32(s + ZS_NEXT_OUT), ao = r32(s + ZS_AVAIL_OUT);
    z.next_in = ai ? host_ptr(ni, ai) : (Bytef*)host_ptr(ni ? ni : layout::RAM_BASE);
    z.avail_in = ai;
    z.next_out = ao ? host_ptr(no, ao) : (Bytef*)host_ptr(no ? no : layout::RAM_BASE);
    z.avail_out = ao;
}
static void store_out(uint32_t s, z_stream& z) {
    uint32_t ni = r32(s + ZS_NEXT_IN), ai = r32(s + ZS_AVAIL_IN), no = r32(s + ZS_NEXT_OUT), ao = r32(s + ZS_AVAIL_OUT);
    uint32_t used_in = ai - z.avail_in, used_out = ao - z.avail_out;
    w32(s + ZS_NEXT_IN, ni + used_in);
    w32(s + ZS_AVAIL_IN, z.avail_in);
    w32(s + ZS_TOTAL_IN, r32(s + ZS_TOTAL_IN) + used_in);
    w32(s + ZS_NEXT_OUT, no + used_out);
    w32(s + ZS_AVAIL_OUT, z.avail_out);
    w32(s + ZS_TOTAL_OUT, r32(s + ZS_TOTAL_OUT) + used_out);
    w32(s + ZS_MSG, 0);
    w32(s + ZS_DATA_TYPE, (uint32_t)z.data_type);
    w32(s + ZS_ADLER, (uint32_t)z.adler);
}
static HostZ* get(uint32_t s) { auto it = g_z.find(s); return it == g_z.end() ? nullptr : it->second; }
static HostZ* fresh(uint32_t s, bool deflating) {
    if (HostZ* old = get(s)) { if (old->deflating) deflateEnd(&old->z); else inflateEnd(&old->z); delete old; }
    HostZ* h = new HostZ;
    h->deflating = deflating;
    g_z[s] = h;
    w32(s + ZS_TOTAL_IN, 0); w32(s + ZS_TOTAL_OUT, 0); w32(s + ZS_MSG, 0);
    w32(s + ZS_STATE, s);   // non-null marker
    return h;
}
static void drop(uint32_t s) {
    HostZ* h = get(s);
    if (!h) return;
    delete h;
    g_z.erase(s);
    w32(s + ZS_STATE, 0);
}

HLE(zlibVersion) {
    if (!g_version_str) { g_version_str = heap::alloc(16, true); wstr(g_version_str, kVersion); }
    return g_version_str;
}
HLE(inflateInit_) { HostZ* h = fresh(a[0], false); return (uint32_t)inflateInit(&h->z); }
HLE(inflateInit2_) { HostZ* h = fresh(a[0], false); return (uint32_t)inflateInit2(&h->z, (int)a[1]); }
HLE(inflate) {
    HostZ* h = get(a[0]);
    if (!h) return (uint32_t)Z_STREAM_ERROR;
    load_in(a[0], h->z);
    int r = inflate(&h->z, (int)a[1]);
    store_out(a[0], h->z);
    return (uint32_t)r;
}
HLE(inflateEnd) { HostZ* h = get(a[0]); if (!h) return (uint32_t)Z_STREAM_ERROR; int r = inflateEnd(&h->z); drop(a[0]); return (uint32_t)r; }
HLE(inflateReset) {
    HostZ* h = get(a[0]);
    if (!h) return (uint32_t)Z_STREAM_ERROR;
    w32(a[0] + ZS_TOTAL_IN, 0); w32(a[0] + ZS_TOTAL_OUT, 0);
    return (uint32_t)inflateReset(&h->z);
}
HLE(inflateSync) { HostZ* h = get(a[0]); if (!h) return (uint32_t)Z_STREAM_ERROR; load_in(a[0], h->z); int r = inflateSync(&h->z); store_out(a[0], h->z); return (uint32_t)r; }
HLE(inflateSetDictionary) { HostZ* h = get(a[0]); if (!h) return (uint32_t)Z_STREAM_ERROR; return (uint32_t)inflateSetDictionary(&h->z, host_ptr(a[1], a[2]), a[2]); }

HLE(deflateInit_) { HostZ* h = fresh(a[0], true); return (uint32_t)deflateInit(&h->z, (int)a[1]); }
HLE(deflateInit2_) { HostZ* h = fresh(a[0], true); return (uint32_t)deflateInit2(&h->z, (int)a[1], (int)a[2], (int)a[3], (int)a[4], (int)a[5]); }
HLE(deflate) {
    HostZ* h = get(a[0]);
    if (!h) return (uint32_t)Z_STREAM_ERROR;
    load_in(a[0], h->z);
    int r = deflate(&h->z, (int)a[1]);
    store_out(a[0], h->z);
    return (uint32_t)r;
}
HLE(deflateEnd) { HostZ* h = get(a[0]); if (!h) return (uint32_t)Z_STREAM_ERROR; int r = deflateEnd(&h->z); drop(a[0]); return (uint32_t)r; }
HLE(deflateReset) { HostZ* h = get(a[0]); if (!h) return (uint32_t)Z_STREAM_ERROR; w32(a[0] + ZS_TOTAL_IN, 0); w32(a[0] + ZS_TOTAL_OUT, 0); return (uint32_t)deflateReset(&h->z); }
HLE(deflateParams) { HostZ* h = get(a[0]); if (!h) return (uint32_t)Z_STREAM_ERROR; return (uint32_t)deflateParams(&h->z, (int)a[1], (int)a[2]); }
HLE(deflateSetDictionary) { HostZ* h = get(a[0]); if (!h) return (uint32_t)Z_STREAM_ERROR; return (uint32_t)deflateSetDictionary(&h->z, host_ptr(a[1], a[2]), a[2]); }
HLE(deflateCopy) {
    HostZ* src = get(a[1]);
    if (!src) return (uint32_t)Z_STREAM_ERROR;
    mem_write(a[0], host_ptr(a[1], 56), 56);
    HostZ* h = fresh(a[0], true);
    return (uint32_t)deflateCopy(&h->z, &src->z);
}

HLE(compress) {
    uLongf n = r32(a[1]);
    int r = compress(host_ptr(a[0], (uint32_t)n), &n, host_ptr(a[2], a[3]), a[3]);
    w32(a[1], (uint32_t)n);
    return (uint32_t)r;
}
HLE(compress2) {
    uLongf n = r32(a[1]);
    int r = compress2(host_ptr(a[0], (uint32_t)n), &n, host_ptr(a[2], a[3]), a[3], (int)a[4]);
    w32(a[1], (uint32_t)n);
    return (uint32_t)r;
}
HLE(uncompress) {
    uLongf n = r32(a[1]);
    int r = uncompress(host_ptr(a[0], (uint32_t)n), &n, host_ptr(a[2], a[3]), a[3]);
    w32(a[1], (uint32_t)n);
    return (uint32_t)r;
}
HLE(adler32) { return (uint32_t)adler32(a[0], a[2] ? host_ptr(a[1], a[2]) : nullptr, a[2]); }
