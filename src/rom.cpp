// Zodiac ROM image: mapped at 0x20000000 like on the device; its databases are registered
// read-only so system resources (and the "DAL" database DRM checks look for) are available.
#include "palmos.h"
#include <cstdio>

Db* dm_register(Db* d);

static bool printable(uint32_t a, int n) {
    for (int i = 0; i < n; i++) {
        uint8_t c = g.rom[a + i];
        if (c < 0x20 || c >= 0x7F) return false;
    }
    return true;
}
static uint32_t rd(uint32_t off) { uint32_t v; memcpy(&v, g.rom + off, 4); return v; }
static uint16_t rd16(uint32_t off) { uint16_t v; memcpy(&v, g.rom + off, 2); return v; }

// ROM chunks are preceded by an 8-byte header: size (24 bits, includes the header) and a
// 4-bit size adjustment in the top byte.
static uint32_t chunk_size(uint32_t ptr) {
    uint32_t off = ptr - layout::ROM_BASE;
    if (off < 8 || off >= layout::ROM_SIZE) return 0;
    uint32_t w = rd(off - 8), size = w & 0xFFFFFF, adj = (w >> 24) & 0xF;
    return size >= 8 + adj ? size - 8 - adj : 0;
}

bool rom_load(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    size_t n = fread(g.rom, 1, layout::ROM_SIZE - 0x1000, f);
    fclose(f);
    int count = 0;
    // Database headers (LE): name[32], attr@0x20, version@0x22, type@0x3C, creator@0x40,
    // record list: next@0x48, count@0x4C, entries@0x50 (12 bytes for resources, 8 for records).
    for (uint32_t i = 0; i + 0x60 < n; i += 4) {
        const char* name = (const char*)g.rom + i;
        size_t len = strnlen(name, 32);
        if (len == 0 || len == 32 || !printable(i, (int)len)) continue;
        if (!printable(i + 0x3C, 4) || !printable(i + 0x40, 4)) continue;
        uint32_t next = rd(i + 0x48);
        uint16_t num = rd16(i + 0x4C), attr = rd16(i + 0x20);
        if (next != 0 || num == 0 || num > 3000) continue;
        bool res = attr & 1;
        uint32_t esz = res ? 12 : 8, ent = i + 0x50;
        if (ent + num * esz > n) continue;
        bool ok = true;
        for (uint32_t k = 0; k < num && ok; k++) {
            uint32_t ptr = rd(ent + k * esz + (res ? 8 : 0));
            if (ptr && (ptr < layout::ROM_BASE || ptr >= layout::ROM_BASE + n)) ok = false;
            if (res && !printable(ent + k * esz, 4)) ok = false;
        }
        if (!ok) continue;
        Db* db = new Db;
        db->name.assign(name, len);
        db->attr = attr | 0x0002;   // read-only
        db->version = rd16(i + 0x22);
        db->type = rd(i + 0x3C);
        db->creator = rd(i + 0x40);
        for (uint32_t k = 0; k < num; k++) {
            uint32_t e = ent + k * esz;
            DbRec r;
            uint32_t ptr;
            if (res) {
                r.type = rd(e);
                r.id = rd16(e + 4);
                ptr = rd(e + 8);
            } else {
                ptr = rd(e);
                r.attr = g.rom[e + 4];
                r.uid = g.rom[e + 5] << 16 | g.rom[e + 6] << 8 | g.rom[e + 7];
            }
            r.h = ptr ? heap::new_foreign_handle(ptr, chunk_size(ptr)) : 0;
            db->recs.push_back(r);
        }
        dm_register(db);
        count++;
        i += 0x4C;
    }
    LOG("ROM %s: %d databases", path.c_str(), count);
    return true;
}

// ---- ROM code that games call directly ------------------------------------------------------
// Homebrew titles (Noiz2sa, Spout, Thruster, Outpost, Another World) unlock TwGfx, which the
// 1.1 ROM restricts to signed applications: they copy the prologue of the ROM's TwGfxOpen and
// jump past its caller check to 0x200995F0, with aResult in r6 and aInfo in r7. We run our
// TwGfxOpen there and resume at the function's own epilogue (add sp; pop; bx lr) at 0x200995E4.
struct RomEntry { uint32_t addr; const char* hle; int regs[4]; int nregs; uint32_t resume; };
static const RomEntry kRomEntries[] = {
    {0x200995F0, "TwGfxOpen", {6, 7}, 2, 0x200995E4},
};

static void hook_rom_entry(uc_engine*, uint64_t address, uint32_t, void* user) {
    const RomEntry& e = *(const RomEntry*)user;
    uint32_t v[12] = {};
    for (int i = 0; i < e.nregs; i++) v[i] = reg(e.regs[i]);
    Args a{};
    a.ext = v;
    HleFn fn = glue_hle(e.hle);
    uint32_t r = fn ? fn(a) : 0;
    if (g.opt.trace) LOG("[%7u] ROM %08x -> %s = %x", now_ms(), e.addr, e.hle, r);
    set_reg(0, r);
    set_reg(15, e.resume);
}

void rom_hook_entries() {
    for (auto& e : kRomEntries) {
        uc_hook h;
        uc_hook_add(g.uc, &h, UC_HOOK_CODE, (void*)hook_rom_entry, (void*)&e, e.addr, e.addr);
    }
}
