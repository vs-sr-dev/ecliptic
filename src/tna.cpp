// Tapwave Native Application modules: loading ARMC/ARMZ code resources and calling their
// PACE Native Object entry point (TwLoadModule / PceNativeCall semantics).
#include "palmos.h"
#include "tna.h"
#include <zlib.h>

static uint32_t g_call68k = 0, g_emul_state = 0;
static bool g_launched = false;

static std::vector<uint8_t> gunzip(const uint8_t* p, uint32_t n) {
    std::vector<uint8_t> out;
    z_stream z{};
    inflateInit2(&z, 16 + MAX_WBITS);
    z.next_in = const_cast<uint8_t*>(p);
    z.avail_in = n;
    uint8_t buf[65536];
    int r;
    do {
        z.next_out = buf;
        z.avail_out = sizeof buf;
        r = inflate(&z, Z_NO_FLUSH);
        out.insert(out.end(), buf, buf + (sizeof buf - z.avail_out));
    } while (r == Z_OK);
    inflateEnd(&z);
    if (r != Z_STREAM_END) LOG("!! gunzip failed (%d)", r);
    return out;
}

namespace tna {

void init(uint32_t call68k) {
    g_call68k = call68k;
    g_emul_state = heap::alloc(64, true);
}

bool has_native_code(Db* db, uint16_t id) {
    for (auto& r : db->recs)
        if (r.id == id && (r.type == fourcc("ARMC") || r.type == fourcc("ARMZ"))) return true;
    return false;
}

uint32_t load(Db* db, uint32_t rsrcType, uint16_t id) {
    // rsrcType 0 = 'ARMC', or its gzipped form 'ARMZ' (HighPerformanceGames.txt, TwLoadModule)
    uint32_t armc = fourcc("ARMC"), armz = fourcc("ARMZ");
    std::vector<uint8_t> code;
    bool found = false;
    for (int pass = 0; pass < 2 && !found; pass++) {
        uint32_t want = rsrcType ? rsrcType : pass == 0 ? armc : armz;
        for (auto& r : db->recs) {
            if (r.id != id || r.type != want) continue;
            uint32_t p = heap::deref(r.h), n = heap::size(p);
            if (want == armz) code = gunzip(host_ptr(p, n), n);
            else code.assign(host_ptr(p, n), host_ptr(p, n) + n);
            found = true;
            break;
        }
    }
    if (!found || code.size() < 72 || memcmp(&code[28], "ANWT", 4) != 0) {
        LOG("TwLoadModule: no TNA code resource #%u in '%s'", id, db->name.c_str());
        return 0;
    }
    uint32_t size = (uint32_t)code.size(), base = heap::alloc(size);
    if (!base) return 0;
    mem_write(base, code.data(), size);
    if (!g.code_base) { g.code_base = base; g.code_size = size; }
    LOG("TNA code: %u bytes at %08x", size, base);
    cpu_fix_bx_pc_veneers(base, size);
    if (!g.opt.blocks_file.empty() && base == g.code_base) cpu_trace_blocks(base, size);

    // 1st call: emulStateP == NULL, userData = TwGlue table (relocation + runtime init)
    uint32_t r = cpu_call(base, {0, g.glue_table, g_call68k});
    LOG("TNA init returned %u", r);
    return g.quit ? 0 : base;
}

uint32_t run(uint32_t entry, uint32_t userData) {
    // PceNativeCall: for a TNA with userData NULL this runs PilotMain(sysAppLaunchCmdNormalLaunch)
    g_launched = true;
    uint32_t r = cpu_call(entry, {g_emul_state, userData, g_call68k});
    LOG("PilotMain returned %u", r);
    return r;
}

bool launched() { return g_launched; }

}  // namespace tna
