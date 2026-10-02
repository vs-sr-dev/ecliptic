// Data Manager + Memory Manager HLE.
#include "palmos.h"
#include <algorithm>
#include <cstdio>
#include <ctime>
#include <map>
#include <filesystem>

namespace fs = std::filesystem;
using namespace layout;

std::string fourcc_str(uint32_t v) {
    std::string s;
    for (int i = 3; i >= 0; i--) { char c = (char)(v >> (i * 8)); s += (c >= 32 && c < 127) ? c : '?'; }
    return s;
}
static uint32_t palm_now() { return (uint32_t)(time(nullptr) + 2082844800u); }
static uint32_t be32(const uint8_t* p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t* p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put32(std::vector<uint8_t>& v, uint32_t x) { for (int i = 3; i >= 0; i--) v.push_back((uint8_t)(x >> (i * 8))); }
static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back((uint8_t)(x >> 8)); v.push_back((uint8_t)x); }

struct OpenRef { Db* db; uint16_t mode; };
static std::vector<Db*> g_dbs;
static std::map<uint32_t, OpenRef> g_open;
static std::vector<uint32_t> g_open_order;   // open refs, oldest first
static uint32_t g_next_open = OPAQUE_OPEN + 0x10;
static Db* g_app = nullptr;
static uint16_t g_last_err = 0;
static std::string g_ram_dir = "ram";

namespace dm {
const std::vector<Db*>& all() { return g_dbs; }
Db* app_db() { return g_app; }
void set_app_db(Db* db) { g_app = db; }
void set_ram_dir(const std::string& d) { g_ram_dir = d; }

static Db* add(Db* db) {
    db->dbH = OPAQUE_DB + (uint32_t)(g_dbs.size() + 1) * 0x10;
    g_dbs.push_back(db);
    return db;
}
Db* by_handle(uint32_t dbH) {
    for (Db* d : g_dbs) if (d->dbH == dbH && !d->deleted) return d;
    return nullptr;
}
Db* by_name(const std::string& n) {
    for (Db* d : g_dbs) if (!d->deleted && d->name == n) return d;
    return nullptr;
}
Db* by_open_ref(uint32_t ref) {
    auto it = g_open.find(ref);
    return it == g_open.end() ? nullptr : it->second.db;
}

static uint32_t new_chunk(const uint8_t* data, uint32_t len) {
    uint32_t h = heap::new_handle(len);
    if (h && len) mem_write(heap::deref(h), data, len);
    return h;
}

Db* load_file(const std::string& path, bool persistent) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    std::vector<uint8_t> d;
    fseek(f, 0, SEEK_END); d.resize(ftell(f)); fseek(f, 0, SEEK_SET);
    size_t got = fread(d.data(), 1, d.size(), f);
    fclose(f);
    if (got != d.size() || d.size() < 78) return nullptr;
    Db* db = new Db;
    db->name = std::string((const char*)d.data(), strnlen((const char*)d.data(), 32));
    db->attr = be16(&d[32]); db->version = be16(&d[34]);
    db->crDate = be32(&d[36]); db->modDate = be32(&d[40]); db->bckDate = be32(&d[44]); db->modNum = be32(&d[48]);
    uint32_t appInfo = be32(&d[52]), sortInfo = be32(&d[56]);
    db->type = be32(&d[60]); db->creator = be32(&d[64]); db->uidSeed = be32(&d[68]);
    uint16_t n = be16(&d[76]);
    bool res = db->attr & dmHdrAttrResDB;
    std::vector<uint32_t> offs;
    size_t p = 78;
    for (uint16_t i = 0; i < n; i++) {
        DbRec r;
        if (res) { r.type = be32(&d[p]); r.id = be16(&d[p + 4]); offs.push_back(be32(&d[p + 6])); p += 10; }
        else { offs.push_back(be32(&d[p])); r.attr = d[p + 4]; r.uid = (uint32_t)d[p + 5] << 16 | d[p + 6] << 8 | d[p + 7]; p += 8; }
        db->recs.push_back(r);
    }
    auto end_of = [&](uint32_t start) {
        uint32_t e = (uint32_t)d.size();
        for (uint32_t o : offs) if (o > start && o < e) e = o;
        if (appInfo > start && appInfo < e) e = appInfo;
        if (sortInfo > start && sortInfo < e) e = sortInfo;
        return e;
    };
    for (uint16_t i = 0; i < n; i++) {
        uint32_t s = offs[i], e = end_of(s);
        if (s > d.size()) s = e = (uint32_t)d.size();
        db->recs[i].h = new_chunk(&d[s], e - s);
    }
    if (appInfo) db->appInfoH = new_chunk(&d[appInfo], end_of(appInfo) - appInfo);
    if (sortInfo) db->sortInfoH = new_chunk(&d[sortInfo], end_of(sortInfo) - sortInfo);
    if (persistent) db->save_path = path;
    return add(db);
}

static bool save(Db* db) {
    if (db->save_path.empty()) return false;
    std::vector<uint8_t> out(32, 0);
    memcpy(out.data(), db->name.c_str(), std::min<size_t>(31, db->name.size()));
    put16(out, db->attr & ~0x8000); put16(out, db->version);
    put32(out, db->crDate); put32(out, db->modDate); put32(out, db->bckDate); put32(out, db->modNum);
    size_t appPos = out.size(); put32(out, 0); size_t sortPos = out.size(); put32(out, 0);
    put32(out, db->type); put32(out, db->creator); put32(out, db->uidSeed); put32(out, 0);
    put16(out, (uint16_t)db->recs.size());
    uint32_t hdr = (uint32_t)out.size() + (uint32_t)db->recs.size() * (db->res() ? 10 : 8) + 2;
    uint32_t off = hdr;
    auto sz = [](uint32_t h) { return h ? heap::size(heap::deref(h)) : 0u; };
    std::vector<uint32_t> blocks;
    if (db->appInfoH) { blocks.push_back(db->appInfoH); off += sz(db->appInfoH); }
    if (db->sortInfoH) { blocks.push_back(db->sortInfoH); off += sz(db->sortInfoH); }
    uint32_t cur = hdr;
    if (db->appInfoH) { for (int i = 0; i < 4; i++) out[appPos + i] = (uint8_t)(cur >> (24 - 8 * i)); cur += sz(db->appInfoH); }
    if (db->sortInfoH) { for (int i = 0; i < 4; i++) out[sortPos + i] = (uint8_t)(cur >> (24 - 8 * i)); cur += sz(db->sortInfoH); }
    for (auto& r : db->recs) {
        if (db->res()) { put32(out, r.type); put16(out, r.id); put32(out, cur); }
        else { put32(out, cur); out.push_back(r.attr & ~(dmRecAttrBusy)); out.push_back((uint8_t)(r.uid >> 16)); out.push_back((uint8_t)(r.uid >> 8)); out.push_back((uint8_t)r.uid); }
        cur += sz(r.h);
        blocks.push_back(r.h);
    }
    put16(out, 0);
    for (uint32_t h : blocks) {
        uint32_t s = sz(h);
        if (!s) continue;
        size_t pos = out.size();
        out.resize(pos + s);
        mem_read(heap::deref(h), &out[pos], s);
    }
    fs::create_directories(fs::path(db->save_path).parent_path());
    FILE* f = fopen(db->save_path.c_str(), "wb");
    if (!f) return false;
    fwrite(out.data(), 1, out.size(), f);
    fclose(f);
    return true;
}

void save_all() {
    for (Db* d : g_dbs) {
        if (d->deleted) { if (!d->save_path.empty()) fs::remove(d->save_path); continue; }
        if (d->dirty && save(d)) LOG("saved database '%s' -> %s", d->name.c_str(), d->save_path.c_str());
    }
}

uint32_t open(Db* db, uint16_t mode) {
    uint32_t ref = g_next_open;
    g_next_open += 0x10;
    g_open[ref] = {db, mode};
    g_open_order.push_back(ref);
    db->openCount++;
    return ref;
}

uint32_t find_resource_any(uint32_t type, uint16_t id) {
    for (auto it = g_open_order.rbegin(); it != g_open_order.rend(); ++it) {
        Db* db = g_open[*it].db;
        if (!db->res()) continue;
        for (auto& r : db->recs) if (r.type == type && r.id == id) return r.h;
    }
    return 0;
}
}  // namespace dm

using namespace dm;

static Db* ref_db(uint32_t ref) {
    Db* db = by_open_ref(ref);
    if (!db) LOG("!! bad DmOpenRef %08x", ref);
    return db;
}
static uint32_t ret_err(uint16_t e) { g_last_err = e; return e; }
static void touch(Db* db) { db->dirty = true; db->modNum++; db->modDate = palm_now(); }

// ---- database level ------------------------------------------------------
HLE(DmGetLastErr) { return g_last_err; }
HLE(DmNumDatabases) { uint32_t n = 0; for (Db* d : all()) n += !d->deleted; return n; }
HLE(DmGetDatabase) {
    uint32_t i = 0;
    for (Db* d : all()) if (!d->deleted && i++ == (a[0] & 0xFFFF)) return d->dbH;
    return 0;
}
HLE(DmFindDatabase) {
    Db* d = by_name(rstr(a[0]));
    g_last_err = d ? 0 : dmErrCantFind;
    return d ? d->dbH : 0;
}
HLE(DmCreateDatabase) {
    std::string name = rstr(a[0]);
    if (by_name(name)) return ret_err(dmErrAlreadyExists);
    Db* db = new Db;
    db->name = name; db->creator = a[1]; db->type = a[2];
    db->attr = (a[3] & 0xFF) ? dmHdrAttrResDB : 0;
    db->crDate = db->modDate = palm_now();
    db->dirty = true;
    std::string safe;
    for (char c : name) safe += (isalnum((unsigned char)c) || c == '-' || c == '_' || c == ' ' || c == '.') ? c : '_';
    db->save_path = g_ram_dir + "/" + safe + (db->res() ? ".prc" : ".pdb");
    add(db);
    LOG("DmCreateDatabase '%s' %s/%s", name.c_str(), fourcc_str(db->type).c_str(), fourcc_str(db->creator).c_str());
    return ret_err(0);
}
HLE(DmDeleteDatabase) {
    Db* d = by_handle(a[0]);
    if (!d) return ret_err(dmErrCantFind);
    d->deleted = true;
    LOG("DmDeleteDatabase '%s'", d->name.c_str());
    return ret_err(0);
}
HLE(DmDatabaseInfo) {
    Db* d = by_handle(a[0]);
    if (!d) return ret_err(dmErrCantFind);
    if (a[1]) wstr(a[1], d->name);
    if (a[2]) w16(a[2], d->attr);
    if (a[3]) w16(a[3], d->version);
    if (a[4]) w32(a[4], d->crDate);
    if (a[5]) w32(a[5], d->modDate);
    if (a[6]) w32(a[6], d->bckDate);
    if (a[7]) w32(a[7], d->modNum);
    if (a[8]) w32(a[8], d->appInfoH);
    if (a[9]) w32(a[9], d->sortInfoH);
    if (a[10]) w32(a[10], d->type);
    if (a[11]) w32(a[11], d->creator);
    return ret_err(0);
}
HLE(DmSetDatabaseInfo) {
    Db* d = by_handle(a[0]);
    if (!d) return ret_err(dmErrCantFind);
    if (a[1]) d->name = rstr(a[1]);
    if (a[2]) d->attr = (d->attr & dmHdrAttrResDB) | (r16(a[2]) & ~dmHdrAttrResDB);
    if (a[3]) d->version = r16(a[3]);
    if (a[4]) d->crDate = r32(a[4]);
    if (a[5]) d->modDate = r32(a[5]);
    if (a[6]) d->bckDate = r32(a[6]);
    if (a[7]) d->modNum = r32(a[7]);
    if (a[8]) d->appInfoH = r32(a[8]);
    if (a[9]) d->sortInfoH = r32(a[9]);
    if (a[10]) d->type = r32(a[10]);
    if (a[11]) d->creator = r32(a[11]);
    d->dirty = true;
    return ret_err(0);
}
HLE(DmDatabaseSize) {
    Db* d = by_handle(a[0]);
    if (!d) return ret_err(dmErrCantFind);
    uint32_t total = 0;
    for (auto& r : d->recs) total += r.h ? heap::size(heap::deref(r.h)) : 0;
    if (a[1]) w32(a[1], (uint32_t)d->recs.size());
    if (a[2]) w32(a[2], total + 78 + (uint32_t)d->recs.size() * 10);
    if (a[3]) w32(a[3], total);
    return ret_err(0);
}
HLE(DmGetNextDatabaseByTypeCreator) {
    // DmSearchStateType: we keep our cursor in the first word.
    bool fresh = a[0] & 0xFF;
    uint32_t st = a[1], type = a[2], creator = a[3], outP = a[5];
    uint32_t start = fresh ? 0 : r32(st);
    auto& v = all();
    for (uint32_t i = start; i < v.size(); i++) {
        Db* d = v[i];
        if (d->deleted) continue;
        if (type && d->type != type) continue;
        if (creator && d->creator != creator) continue;
        w32(st, i + 1);
        if (outP) w32(outP, d->dbH);
        return ret_err(0);
    }
    if (outP) w32(outP, 0);
    return ret_err(dmErrCantFind);
}
HLE(DmOpenDatabase) {
    Db* d = by_handle(a[0]);
    if (!d) { g_last_err = dmErrCantOpen; return 0; }
    g_last_err = 0;
    return open(d, (uint16_t)a[1]);
}
HLE(DmOpenDatabaseByTypeCreator) {
    for (auto it = all().rbegin(); it != all().rend(); ++it) {
        Db* d = *it;
        if (!d->deleted && d->type == a[0] && d->creator == a[1]) { g_last_err = 0; return open(d, (uint16_t)a[2]); }
    }
    g_last_err = dmErrCantFind;
    return 0;
}
HLE(DmCloseDatabase) {
    auto it = g_open.find(a[0]);
    if (it == g_open.end()) return ret_err(dmErrInvalidParam);
    it->second.db->openCount--;
    g_open.erase(it);
    g_open_order.erase(std::remove(g_open_order.begin(), g_open_order.end(), a[0]), g_open_order.end());
    return ret_err(0);
}
HLE(DmOpenDatabaseInfo) {
    auto it = g_open.find(a[0]);
    if (it == g_open.end()) return ret_err(dmErrInvalidParam);
    if (a[1]) w32(a[1], it->second.db->dbH);
    if (a[2]) w16(a[2], (uint16_t)it->second.db->openCount);
    if (a[3]) w16(a[3], it->second.mode);
    if (a[4]) w8(a[4], it->second.db->res());
    return ret_err(0);
}
HLE(DmGetAppInfoH) { Db* d = ref_db(a[0]); return d ? d->appInfoH : 0; }
HLE(DmCreateDatabaseFromImage) {
    LOG("DmCreateDatabaseFromImage(%08x): not supported yet", a[0]);
    return ret_err(dmErrInvalidParam);
}

// ---- resources -------------------------------------------------------------
HLE(DmGetResource) {
    uint32_t h = find_resource_any(a[0], (uint16_t)a[1]);
    g_last_err = h ? 0 : dmErrResourceNotFound;
    if (!h) LOG("DmGetResource('%s', %u): not found", fourcc_str(a[0]).c_str(), a[1] & 0xFFFF);
    return h;
}
HLE(DmGet1Resource) {
    if (g_open_order.empty()) return 0;
    Db* d = g_open[g_open_order.back()].db;
    for (auto& r : d->recs) if (r.type == a[0] && r.id == (uint16_t)a[1]) return r.h;
    g_last_err = dmErrResourceNotFound;
    return 0;
}
HLE(DmReleaseResource) { return ret_err(0); }
HLE(DmNumResources) { Db* d = ref_db(a[0]); return d ? (uint32_t)d->recs.size() : 0; }
HLE(DmGetResourceIndex) {
    Db* d = ref_db(a[0]);
    uint16_t i = (uint16_t)a[1];
    if (!d || i >= d->recs.size()) { g_last_err = dmErrIndexOutOfRange; return 0; }
    return d->recs[i].h;
}
HLE(DmFindResource) {
    Db* d = ref_db(a[0]);
    if (!d) return 0xFFFF;
    for (size_t i = 0; i < d->recs.size(); i++) {
        auto& r = d->recs[i];
        if (a[3] ? r.h == a[3] : (r.type == a[1] && r.id == (uint16_t)a[2])) return (uint32_t)i;
    }
    return 0xFFFF;
}
HLE(DmFindResourceType) {
    Db* d = ref_db(a[0]);
    if (!d) return 0xFFFF;
    uint16_t n = (uint16_t)a[2];
    for (size_t i = 0; i < d->recs.size(); i++)
        if (d->recs[i].type == a[1] && n-- == 0) return (uint32_t)i;
    return 0xFFFF;
}
HLE(DmResourceInfo) {
    Db* d = ref_db(a[0]);
    uint16_t i = (uint16_t)a[1];
    if (!d || i >= d->recs.size()) return ret_err(dmErrIndexOutOfRange);
    if (a[2]) w32(a[2], d->recs[i].type);
    if (a[3]) w16(a[3], d->recs[i].id);
    if (a[4]) w32(a[4], d->recs[i].h);
    return ret_err(0);
}
HLE(DmSetResourceInfo) {
    Db* d = ref_db(a[0]);
    uint16_t i = (uint16_t)a[1];
    if (!d || i >= d->recs.size()) return ret_err(dmErrIndexOutOfRange);
    if (a[2]) d->recs[i].type = r32(a[2]);
    if (a[3]) d->recs[i].id = r16(a[3]);
    touch(d);
    return ret_err(0);
}
HLE(DmNewResource) {
    Db* d = ref_db(a[0]);
    if (!d) return 0;
    DbRec r; r.type = a[1]; r.id = (uint16_t)a[2]; r.h = heap::new_handle(a[3], true);
    d->recs.push_back(r);
    touch(d);
    return r.h;
}
HLE(DmAttachResource) {
    Db* d = ref_db(a[0]);
    if (!d) return ret_err(dmErrInvalidParam);
    DbRec r; r.h = a[1]; r.type = a[2]; r.id = (uint16_t)a[3];
    d->recs.push_back(r);
    touch(d);
    return ret_err(0);
}
HLE(DmDetachResource) {
    Db* d = ref_db(a[0]);
    uint16_t i = (uint16_t)a[1];
    if (!d || i >= d->recs.size()) return ret_err(dmErrIndexOutOfRange);
    if (a[2]) w32(a[2], d->recs[i].h);
    d->recs.erase(d->recs.begin() + i);
    touch(d);
    return ret_err(0);
}
HLE(DmRemoveResource) {
    Db* d = ref_db(a[0]);
    uint16_t i = (uint16_t)a[1];
    if (!d || i >= d->recs.size()) return ret_err(dmErrIndexOutOfRange);
    if (d->recs[i].h) heap::free_handle(d->recs[i].h);
    d->recs.erase(d->recs.begin() + i);
    touch(d);
    return ret_err(0);
}
HLE(DmResizeResource) {
    if (!heap::resize_handle(a[0], a[1])) { g_last_err = dmErrMemError; return 0; }
    return a[0];
}
HLE(DmSearchResource) {
    for (auto it = g_open_order.rbegin(); it != g_open_order.rend(); ++it) {
        Db* d = g_open[*it].db;
        for (size_t i = 0; i < d->recs.size(); i++) {
            auto& r = d->recs[i];
            if (a[2] ? r.h == a[2] : (r.type == a[0] && r.id == (uint16_t)a[1])) {
                if (a[3]) w32(a[3], *it);
                return (uint32_t)i;
            }
        }
    }
    return 0xFFFF;
}

// ---- records ---------------------------------------------------------------
HLE(DmNumRecords) { Db* d = ref_db(a[0]); return d ? (uint32_t)d->recs.size() : 0; }
HLE(DmNumRecordsInCategory) {
    Db* d = ref_db(a[0]);
    if (!d) return 0;
    uint16_t cat = (uint16_t)a[1], n = 0;
    for (auto& r : d->recs) if (!(r.attr & dmRecAttrDelete) && (cat == 0xFFFF || (r.attr & 0x0F) == cat)) n++;
    return n;
}
static uint32_t rec_handle(uint32_t ref, uint32_t idx, bool busy) {
    Db* d = ref_db(ref);
    if (!d || (idx & 0xFFFF) >= d->recs.size()) { g_last_err = dmErrIndexOutOfRange; return 0; }
    DbRec& r = d->recs[idx & 0xFFFF];
    if (busy) r.attr |= dmRecAttrBusy;
    g_last_err = 0;
    return r.h;
}
HLE(DmQueryRecord) { return rec_handle(a[0], a[1], false); }
HLE(DmGetRecord) { return rec_handle(a[0], a[1], true); }
HLE(DmReleaseRecord) {
    Db* d = ref_db(a[0]);
    uint16_t i = (uint16_t)a[1];
    if (!d || i >= d->recs.size()) return ret_err(dmErrIndexOutOfRange);
    d->recs[i].attr &= ~dmRecAttrBusy;
    if (a[2] & 0xFF) { d->recs[i].attr |= dmRecAttrDirty; touch(d); }
    return ret_err(0);
}
HLE(DmNewRecord) {
    Db* d = ref_db(a[0]);
    if (!d) return 0;
    uint16_t at = a[1] ? r16(a[1]) : 0xFFFF;
    if (at > d->recs.size()) at = (uint16_t)d->recs.size();
    DbRec r; r.h = heap::new_handle(a[2], true); r.attr = dmRecAttrBusy | dmRecAttrDirty; r.uid = ++d->uidSeed;
    d->recs.insert(d->recs.begin() + at, r);
    if (a[1]) w16(a[1], at);
    touch(d);
    return r.h;
}
HLE(DmAttachRecord) {
    Db* d = ref_db(a[0]);
    if (!d) return ret_err(dmErrInvalidParam);
    uint16_t at = a[1] ? r16(a[1]) : 0xFFFF;
    if (a[3] && at < d->recs.size()) {      // replace
        w32(a[3], d->recs[at].h);
        d->recs[at].h = a[2];
    } else {
        if (at > d->recs.size()) at = (uint16_t)d->recs.size();
        DbRec r; r.h = a[2]; r.attr = dmRecAttrDirty; r.uid = ++d->uidSeed;
        d->recs.insert(d->recs.begin() + at, r);
        if (a[1]) w16(a[1], at);
    }
    touch(d);
    return ret_err(0);
}
HLE(DmDetachRecord) {
    Db* d = ref_db(a[0]);
    uint16_t i = (uint16_t)a[1];
    if (!d || i >= d->recs.size()) return ret_err(dmErrIndexOutOfRange);
    if (a[2]) w32(a[2], d->recs[i].h);
    d->recs.erase(d->recs.begin() + i);
    touch(d);
    return ret_err(0);
}
static uint32_t remove_rec(uint32_t ref, uint32_t idx, int mode) {   // 0 remove, 1 delete, 2 archive
    Db* d = ref_db(ref);
    uint16_t i = (uint16_t)idx;
    if (!d || i >= d->recs.size()) return ret_err(dmErrIndexOutOfRange);
    DbRec& r = d->recs[i];
    if (mode == 0) {
        if (r.h) heap::free_handle(r.h);
        d->recs.erase(d->recs.begin() + i);
    } else {
        if (mode == 1 && r.h) { heap::free_handle(r.h); r.h = 0; }
        r.attr |= dmRecAttrDelete;
    }
    touch(d);
    return ret_err(0);
}
HLE(DmRemoveRecord) { return remove_rec(a[0], a[1], 0); }
HLE(DmDeleteRecord) { return remove_rec(a[0], a[1], 1); }
HLE(DmArchiveRecord) { return remove_rec(a[0], a[1], 2); }
HLE(DmResizeRecord) {
    Db* d = ref_db(a[0]);
    uint16_t i = (uint16_t)a[1];
    if (!d || i >= d->recs.size()) { g_last_err = dmErrIndexOutOfRange; return 0; }
    DbRec& r = d->recs[i];
    if (!r.h) r.h = heap::new_handle(a[2], true);
    else if (!heap::resize_handle(r.h, a[2])) { g_last_err = dmErrMemError; return 0; }
    touch(d);
    return r.h;
}
HLE(DmRecordInfo) {
    Db* d = ref_db(a[0]);
    uint16_t i = (uint16_t)a[1];
    if (!d || i >= d->recs.size()) return ret_err(dmErrIndexOutOfRange);
    if (a[2]) w16(a[2], d->recs[i].attr);
    if (a[3]) w32(a[3], d->recs[i].uid);
    if (a[4]) w32(a[4], d->recs[i].h);
    return ret_err(0);
}
HLE(DmSetRecordInfo) {
    Db* d = ref_db(a[0]);
    uint16_t i = (uint16_t)a[1];
    if (!d || i >= d->recs.size()) return ret_err(dmErrIndexOutOfRange);
    if (a[2]) d->recs[i].attr = (uint8_t)r16(a[2]);
    if (a[3]) d->recs[i].uid = r32(a[3]);
    touch(d);
    return ret_err(0);
}
HLE(DmFindRecordByID) {
    Db* d = ref_db(a[0]);
    if (!d) return ret_err(dmErrInvalidParam);
    for (size_t i = 0; i < d->recs.size(); i++)
        if (d->recs[i].uid == a[1]) { if (a[2]) w16(a[2], (uint16_t)i); return ret_err(0); }
    return ret_err(dmErrUniqueIDNotFound);
}
HLE(DmMoveRecord) {
    Db* d = ref_db(a[0]);
    uint16_t from = (uint16_t)a[1], to = (uint16_t)a[2];
    if (!d || from >= d->recs.size() || to > d->recs.size()) return ret_err(dmErrIndexOutOfRange);
    DbRec r = d->recs[from];
    d->recs.erase(d->recs.begin() + from);
    if (to > from) to--;
    d->recs.insert(d->recs.begin() + to, r);
    touch(d);
    return ret_err(0);
}
HLE(DmQueryNextInCategory) {
    Db* d = ref_db(a[0]);
    if (!d) return 0;
    uint16_t cat = (uint16_t)a[2];
    for (uint16_t i = r16(a[1]); i < d->recs.size(); i++) {
        auto& r = d->recs[i];
        if (r.attr & dmRecAttrDelete) continue;
        if (cat != 0xFFFF && (r.attr & 0x0F) != cat) continue;
        w16(a[1], i);
        return r.h;
    }
    g_last_err = dmErrIndexOutOfRange;
    return 0;
}
HLE(DmSeekRecordInCategory) {
    Db* d = ref_db(a[0]);
    if (!d) return ret_err(dmErrInvalidParam);
    int idx = r16(a[1]), off = (uint16_t)a[2], dir = (int16_t)a[3];
    uint16_t cat = (uint16_t)a[4];
    auto match = [&](int i) { auto& r = d->recs[i]; return !(r.attr & dmRecAttrDelete) && (cat == 0xFFFF || (r.attr & 0x0F) == cat); };
    int i = idx;
    if (off == 0) {
        for (; i >= 0 && i < (int)d->recs.size(); i += dir ? dir : 1) if (match(i)) { w16(a[1], (uint16_t)i); return ret_err(0); }
        return ret_err(dmErrIndexOutOfRange);
    }
    while (off > 0) {
        i += dir;
        if (i < 0 || i >= (int)d->recs.size()) return ret_err(dmErrIndexOutOfRange);
        if (match(i)) off--;
    }
    w16(a[1], (uint16_t)i);
    return ret_err(0);
}
HLE(DmPositionInCategory) {
    Db* d = ref_db(a[0]);
    if (!d) return 0;
    uint16_t cat = (uint16_t)a[2], n = 0;
    for (uint16_t i = 0; i < (uint16_t)a[1] && i < d->recs.size(); i++)
        if (cat == 0xFFFF || (d->recs[i].attr & 0x0F) == cat) n++;
    return n;
}
HLE(DmMoveCategory) {
    Db* d = ref_db(a[0]);
    if (!d) return ret_err(dmErrInvalidParam);
    for (auto& r : d->recs) if ((r.attr & 0x0F) == (a[2] & 0x0F)) r.attr = (r.attr & 0xF0) | (a[1] & 0x0F);
    touch(d);
    return ret_err(0);
}
HLE(DmResetRecordStates) { Db* d = ref_db(a[0]); if (d) for (auto& r : d->recs) r.attr &= ~dmRecAttrBusy; return ret_err(0); }
HLE(DmRemoveSecretRecords) { return ret_err(0); }
HLE(DmNewHandle) { return heap::new_handle(a[1], true); }

static void sort_records(Db* d, uint32_t compar, uint32_t other) {
    // Comparison callbacks take (rec1, rec2, other, info1, info2, appInfoH).
    uint32_t info = heap::alloc(16, true);
    std::stable_sort(d->recs.begin(), d->recs.end(), [&](const DbRec& x, const DbRec& y) {
        uint32_t px = x.h ? heap::deref(x.h) : 0, py = y.h ? heap::deref(y.h) : 0;
        w8(info, x.attr); w32(info + 4, x.uid); w32(info + 8, y.uid);
        int32_t r = (int16_t)cpu_call(compar, {px, py, other & 0xFFFF, info, info, d->appInfoH});
        return r < 0;
    });
    heap::free(info);
}
HLE(DmQuickSort) { Db* d = ref_db(a[0]); if (d) { sort_records(d, a[1], a[2]); touch(d); } return ret_err(0); }
HLE(DmInsertionSort) { Db* d = ref_db(a[0]); if (d) { sort_records(d, a[1], a[2]); touch(d); } return ret_err(0); }
HLE(DmFindSortPosition) {
    Db* d = ref_db(a[0]);
    if (!d) return 0;
    uint32_t lo = 0, hi = (uint32_t)d->recs.size();
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        uint32_t pm = d->recs[mid].h ? heap::deref(d->recs[mid].h) : 0;
        int32_t r = (int16_t)cpu_call(a[3], {a[1], pm, a[4] & 0xFFFF, a[2], 0, d->appInfoH});
        if (r < 0) hi = mid; else lo = mid + 1;
    }
    return lo;
}

// ---- writing into storage chunks ----------------------------------------
HLE(DmWrite) { mem_write(a[0] + a[1], host_ptr(a[2], a[3]), a[3]); return 0; }
HLE(DmSet) { mem_fill(a[0] + a[1], (uint8_t)a[3], a[2]); return 0; }
HLE(DmStrCopy) { wstr(a[0] + a[1], rstr(a[2])); return 0; }
HLE(DmWriteCheck) { return 0; }

// ---- Memory Manager --------------------------------------------------------
HLE(MemPtrNew) {
    uint32_t p = heap::alloc(a[0]);
    if (!p) LOG("MemPtrNew(%u) failed", a[0]);
    return p;
}
HLE(MemPtrFree) { if (a[0]) heap::free(a[0]); return 0; }
HLE(MemPtrSize) { return heap::size(a[0]); }
HLE(MemPtrResize) { return heap::resize(a[0], a[1]) ? 0 : memErrNotEnoughSpace; }
HLE(MemHandleNew) { return heap::new_handle(a[0]); }
HLE(MemHandleFree) { if (a[0]) heap::free_handle(a[0]); return 0; }
HLE(MemHandleLock) {
    uint32_t p = heap::lock(a[0]);
    if (!p) LOG("!! MemHandleLock(%08x): bad handle", a[0]);
    return p;
}
HLE(MemHandleUnlock) { heap::unlock(a[0]); return 0; }
HLE(MemHandleSize) { return heap::size(heap::deref(a[0])); }
HLE(MemHandleResize) { return heap::resize_handle(a[0], a[1]) ? 0 : memErrNotEnoughSpace; }
HLE(MemMove) { if (a[2]) memmove(host_ptr(a[0], a[2]), host_ptr(a[1], a[2]), a[2]); return 0; }
HLE(MemSet) { mem_fill(a[0], (uint8_t)a[2], a[1]); return 0; }
HLE(MemCmp) {
    int r = memcmp(host_ptr(a[0], a[2]), host_ptr(a[1], a[2]), a[2]);
    return (uint32_t)(r < 0 ? -1 : r > 0 ? 1 : 0);
}
HLE(MemHeapFreeBytes) {
    if (a[1]) w32(a[1], heap::free_bytes());
    if (a[2]) w32(a[2], heap::largest_free());
    return 0;
}
HLE(MemHeapSize) { return RAM_SIZE; }
HLE(MemHeapCheck) { return 0; }
HLE(MemHeapCompact) { return 0; }
HLE(MemHeapScramble) { return 0; }
HLE(MemDebugMode) { return 0; }
HLE(MemSetDebugMode) { return 0; }
HLE(MemHandleOwner) { return 0; }
HLE(MemHandleSetOwner) { return 0; }
HLE(MemPtrOwner) { return 0; }
HLE(MemPtrSetOwner) { return 0; }

Db* dm_register(Db* d) { return dm::add(d); }
