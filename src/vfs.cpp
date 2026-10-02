// VFS Manager / Expansion Manager HLE: one SD card volume backed by a host directory.
#include "palmos.h"
#include "audio.h"
#include <cstdio>
#include <filesystem>
#include <map>
#include <system_error>

namespace fs = std::filesystem;

constexpr uint16_t VOL_REF = 1, SLOT_REF = 1;
constexpr uint16_t vfsErrFileGeneric = vfsErrorClass | 2, vfsErrFileBadRef = vfsErrorClass | 3,
                   vfsErrFileAlreadyExists = vfsErrorClass | 6, vfsErrFileEOF = vfsErrorClass | 7,
                   vfsErrFileNotFound = vfsErrorClass | 8, vfsErrVolumeBadRef = vfsErrorClass | 9,
                   vfsErrBadName = vfsErrorClass | 14, vfsErrUnimplemented = vfsErrorClass | 16,
                   vfsErrIsADirectory = vfsErrorClass | 18, vfsErrDirectoryNotFound = vfsErrorClass | 19,
                   vfsErrNotADirectory = vfsErrorClass | 17, vfsErrDirNotEmpty = vfsErrorClass | 13;
constexpr uint16_t expErrEnumerationEmpty = expErrorClass | 13;
constexpr uint32_t vfsIteratorStop = 0xFFFFFFFF;

struct OpenFile {
    fs::path host;
    FILE* f = nullptr;      // null for directories
    bool dir = false;
    std::vector<fs::path> entries;
};
static std::map<uint32_t, OpenFile> g_files;
static uint32_t g_next_ref = 0x1000;

static fs::path card_root() { return fs::path(g.opt.card_dir); }

// Resolve a Palm VFS path ("/PALM/Programs/x") to a host path, matching case-insensitively.
static fs::path resolve(const std::string& vpath) {
    fs::path cur = card_root();
    std::string p = vpath;
    for (auto& c : p) if (c == '\\') c = '/';
    size_t i = 0;
    while (i < p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string part = p.substr(i, j - i);
        i = j + 1;
        if (part.empty() || part == ".") continue;
        fs::path next = cur / part;
        std::error_code ec;
        if (!fs::exists(next, ec) && fs::is_directory(cur, ec)) {
            for (auto& e : fs::directory_iterator(cur, ec)) {
                std::string n = e.path().filename().string();
                if (_stricmp(n.c_str(), part.c_str()) == 0) { next = e.path(); break; }
            }
        }
        cur = next;
    }
    return cur;
}

static uint32_t palm_time(fs::file_time_type t) {
    auto sys = std::chrono::time_point_cast<std::chrono::seconds>(t - fs::file_time_type::clock::now() + std::chrono::system_clock::now());
    return (uint32_t)(sys.time_since_epoch().count() + 2082844800LL);
}

HLE(VFSVolumeEnumerate) {
    uint32_t it = r32(a[1]);
    if (it == 0) {
        std::error_code ec;
        // The Zodiac always has a volume (its internal card, 'twS0' in the ROM): create an
        // empty card directory rather than report none (FrodoZ quits without a volume).
        if (!fs::is_directory(card_root(), ec) && !fs::create_directories(card_root(), ec)) {
            w16(a[0], 0); w32(a[1], vfsIteratorStop); return expErrEnumerationEmpty;
        }
        w16(a[0], VOL_REF);
        w32(a[1], vfsIteratorStop);
        return 0;
    }
    w16(a[0], 0);   // vfsInvalidVolRef
    w32(a[1], vfsIteratorStop);
    return expErrEnumerationEmpty;
}
HLE(VFSVolumeInfo) {
    if ((a[0] & 0xFFFF) != VOL_REF) return vfsErrVolumeBadRef;
    uint32_t p = a[1];
    w32(p, 0); w32(p + 4, fourcc("vfat")); w32(p + 8, fourcc("vfat")); w32(p + 12, fourcc("libs"));
    w16(p + 16, 0); w16(p + 18, SLOT_REF); w32(p + 20, fourcc("sdig")); w32(p + 24, 0);
    return 0;
}
HLE(VFSVolumeGetLabel) { if (a[1] && a[2]) wstr(a[1], std::string("SD CARD").substr(0, (a[2] & 0xFFFF) - 1)); return 0; }
HLE(VFSVolumeSize) {
    std::error_code ec;
    auto sp = fs::space(card_root(), ec);
    uint64_t total = 1024ull * 1024 * 1024, freeb = 512ull * 1024 * 1024;
    if (!ec) { total = std::min<uint64_t>(sp.capacity, 0xFFFFFFFF); freeb = std::min<uint64_t>(sp.available, 0xFFFFFFFF); }
    if (a[1]) w32(a[1], (uint32_t)(total - std::min(total, freeb)));
    if (a[2]) w32(a[2], (uint32_t)total);
    return 0;
}
HLE(ExpSlotEnumerate) {
    uint32_t it = r32(a[1]);
    if (it == 0) { w16(a[0], SLOT_REF); w32(a[1], vfsIteratorStop); return 0; }
    w16(a[0], 0);
    w32(a[1], vfsIteratorStop);
    return expErrEnumerationEmpty;
}
HLE(ExpCardPresent) { return (a[0] & 0xFFFF) == SLOT_REF ? 0 : (expErrorClass | 3); }
HLE(ExpCardInfo) {
    uint32_t p = a[1];   // capabilityFlags, manufacturerStr[32], productStr[32], deviceClassStr[32], deviceUniqueIDStr[32]
    mem_fill(p, 0, 4 + 32 * 4);
    w32(p, 1);
    wstr(p + 4, "Ecliptic"); wstr(p + 36, "Virtual SD"); wstr(p + 68, "SD"); wstr(p + 100, "0001");
    return 0;
}
HLE(TwGetSlotNumberForVolume) { return (a[0] & 0xFFFF) == VOL_REF ? 1 : (uint32_t)-1; }
HLE(TwGetSlotRefNumForSlot) { return (int16_t)a[0] == 1 ? SLOT_REF : 0; }

HLE(VFSFileOpen) {
    if ((a[0] & 0xFFFF) != VOL_REF) return vfsErrVolumeBadRef;
    std::string vpath = rstr(a[1]);
    uint16_t mode = (uint16_t)a[2];
    fs::path hp = resolve(vpath);
    std::error_code ec;
    OpenFile of;
    of.host = hp;
    if (fs::is_directory(hp, ec)) {
        of.dir = true;
        for (auto& e : fs::directory_iterator(hp, ec)) of.entries.push_back(e.path());
    } else {
        bool exists = fs::exists(hp, ec);
        if (!exists && !(mode & 0x0008)) {
            LOG("VFSFileOpen('%s'): not found (host %s)", vpath.c_str(), hp.string().c_str());
            return vfsErrFileNotFound;
        }
        if (!exists) { FILE* c = fopen(hp.string().c_str(), "wb"); if (!c) return vfsErrDirectoryNotFound; fclose(c); }
        bool write = mode & 0x0004;
        const char* m = write ? ((mode & 0x0010) ? "w+b" : "r+b") : "rb";
        of.f = fopen(hp.string().c_str(), m);
        if (!of.f) return vfsErrFileGeneric;
    }
    uint32_t ref = g_next_ref++;
    g_files[ref] = of;
    w32(a[3], ref);
    return 0;
}
HLE(VFSFileClose) {
    auto it = g_files.find(a[0]);
    if (it == g_files.end()) return vfsErrFileBadRef;
    if (it->second.f) fclose(it->second.f);
    g_files.erase(it);
    return 0;
}
static OpenFile* file(uint32_t ref) {
    auto it = g_files.find(ref);
    return it == g_files.end() ? nullptr : &it->second;
}
HLE(VFSFileRead) {
    OpenFile* f = file(a[0]);
    if (!f) return vfsErrFileBadRef;
    if (!f->f) return vfsErrIsADirectory;
    uint32_t n = a[1];
    uint8_t* dst = n ? host_ptr(a[2], n) : nullptr;
    if (n && !dst) { LOG("!! VFSFileRead into unmapped buffer %08x", a[2]); return vfsErrFileGeneric; }
    size_t got = n ? fread(dst, 1, n, f->f) : 0;
    if (a[3]) w32(a[3], (uint32_t)got);
    return got < n ? vfsErrFileEOF : 0;
}
HLE(VFSFileReadData) {
    OpenFile* f = file(a[0]);
    if (!f || !f->f) return vfsErrFileBadRef;
    uint32_t n = a[1];
    size_t got = n ? fread(host_ptr(a[2] + a[3], n), 1, n, f->f) : 0;
    if (a[4]) w32(a[4], (uint32_t)got);
    return got < n ? vfsErrFileEOF : 0;
}
HLE(VFSFileWrite) {
    OpenFile* f = file(a[0]);
    if (!f || !f->f) return vfsErrFileBadRef;
    size_t put = a[1] ? fwrite(host_ptr(a[2], a[1]), 1, a[1], f->f) : 0;
    if (a[3]) w32(a[3], (uint32_t)put);
    return put < a[1] ? vfsErrFileGeneric : 0;
}
HLE(VFSFileSeek) {
    OpenFile* f = file(a[0]);
    if (!f || !f->f) return vfsErrFileBadRef;
    int origin = a[1] == 0 ? SEEK_SET : a[1] == 1 ? SEEK_CUR : SEEK_END;
    if (_fseeki64(f->f, (int32_t)a[2], origin) != 0) return vfsErrFileGeneric;
    long long pos = _ftelli64(f->f);
    _fseeki64(f->f, 0, SEEK_END);
    long long size = _ftelli64(f->f);
    if (pos > size) { _fseeki64(f->f, size, SEEK_SET); return vfsErrFileEOF; }
    _fseeki64(f->f, pos, SEEK_SET);
    return pos == size ? vfsErrFileEOF : 0;
}
HLE(VFSFileTell) {
    OpenFile* f = file(a[0]);
    if (!f || !f->f) return vfsErrFileBadRef;
    w32(a[1], (uint32_t)_ftelli64(f->f));
    return 0;
}
HLE(VFSFileSize) {
    OpenFile* f = file(a[0]);
    if (!f) return vfsErrFileBadRef;
    if (!f->f) { if (a[1]) w32(a[1], 0); return 0; }
    long long pos = _ftelli64(f->f);
    _fseeki64(f->f, 0, SEEK_END);
    if (a[1]) w32(a[1], (uint32_t)_ftelli64(f->f));
    _fseeki64(f->f, pos, SEEK_SET);
    return 0;
}
HLE(VFSFileEOF) {
    OpenFile* f = file(a[0]);
    if (!f || !f->f) return vfsErrFileBadRef;
    long long pos = _ftelli64(f->f);
    _fseeki64(f->f, 0, SEEK_END);
    long long size = _ftelli64(f->f);
    _fseeki64(f->f, pos, SEEK_SET);
    return pos >= size ? vfsErrFileEOF : 0;
}
HLE(VFSFileResize) {
    OpenFile* f = file(a[0]);
    if (!f || !f->f) return vfsErrFileBadRef;
    fflush(f->f);
    std::error_code ec;
    fs::resize_file(f->host, a[1], ec);
    return ec ? vfsErrFileGeneric : 0;
}
HLE(VFSFileGetAttributes) {
    OpenFile* f = file(a[0]);
    if (!f) return vfsErrFileBadRef;
    w32(a[1], f->dir ? 0x10 : 0x20);
    return 0;
}
HLE(VFSFileSetAttributes) { return 0; }
HLE(VFSFileGetDate) {
    OpenFile* f = file(a[0]);
    if (!f) return vfsErrFileBadRef;
    std::error_code ec;
    auto t = fs::last_write_time(f->host, ec);
    w32(a[2], ec ? 0 : palm_time(t));
    return 0;
}
HLE(VFSFileSetDate) { return 0; }
HLE(VFSFileCreate) {
    fs::path hp = resolve(rstr(a[1]));
    std::error_code ec;
    if (fs::exists(hp, ec)) return vfsErrFileAlreadyExists;
    if (!fs::is_directory(hp.parent_path(), ec)) return vfsErrDirectoryNotFound;
    FILE* f = fopen(hp.string().c_str(), "wb");
    if (!f) return vfsErrFileGeneric;
    fclose(f);
    return 0;
}
HLE(VFSFileDelete) {
    fs::path hp = resolve(rstr(a[1]));
    std::error_code ec;
    if (!fs::exists(hp, ec)) return vfsErrFileNotFound;
    if (!fs::remove(hp, ec)) return ec ? vfsErrDirNotEmpty : vfsErrFileGeneric;
    return 0;
}
HLE(VFSFileRename) {
    fs::path hp = resolve(rstr(a[1]));
    std::error_code ec;
    fs::rename(hp, hp.parent_path() / rstr(a[2]), ec);
    return ec ? vfsErrFileGeneric : 0;
}
HLE(VFSDirCreate) {
    if (g.opt.trace) LOG("  VFSDirCreate path='%s'", rstr(a[1]).c_str());
    fs::path hp = resolve(rstr(a[1]));
    std::error_code ec;
    if (fs::exists(hp, ec)) return vfsErrFileAlreadyExists;
    if (!fs::is_directory(hp.parent_path(), ec)) return vfsErrDirectoryNotFound;
    fs::create_directory(hp, ec);
    return ec ? vfsErrFileGeneric : 0;
}
HLE(VFSDirEntryEnumerate) {
    OpenFile* f = file(a[0]);
    if (!f) return vfsErrFileBadRef;
    if (!f->dir) return vfsErrNotADirectory;
    uint32_t it = r32(a[1]);
    if (it == vfsIteratorStop || it >= f->entries.size()) { w32(a[1], vfsIteratorStop); return expErrEnumerationEmpty; }
    const fs::path& e = f->entries[it];
    std::error_code ec;
    uint32_t info = a[2];
    w32(info, fs::is_directory(e, ec) ? 0x10 : 0x20);
    uint32_t nameP = r32(info + 4);
    uint16_t len = r16(info + 8);
    if (nameP && len) wstr(nameP, e.filename().string().substr(0, len - 1));
    w32(a[1], it + 1);   // the end is reported by the next call (like the FAT driver)
    return 0;
}
HLE(VFSGetDefaultDirectory) {
    std::string d = "/PALM/Launcher/";
    uint16_t cap = r16(a[3]);
    if (cap <= d.size()) return vfsErrorClass | 1;
    wstr(a[2], d);
    w16(a[3], (uint16_t)d.size() + 1);
    return 0;
}
HLE(VFSRegisterDefaultDirectory) { return 0; }
HLE(VFSUnregisterDefaultDirectory) { return 0; }
HLE(VFSCustomControl) { return vfsErrUnimplemented; }

// ---- Tapwave data directories --------------------------------------------------
static std::string data_dir_for(Db* db) {
    if (db) {
        for (auto& r : db->recs)
            if (r.type == fourcc("Twdp") && r.id == 0 && r.h) {
                std::string s = rstr(heap::deref(r.h), 256);
                if (!s.empty()) return s;
            }
    }
    std::string name = db ? db->name : "";
    std::string suffix = "-" + (db ? fourcc_str(db->creator) : std::string("????"));
    // Names that already end in "-<creator>" are used as they are (e.g. "StreetHoops-STRT").
    // TODO: confirm against the ROM implementation.
    if (name.size() >= suffix.size() && name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0) suffix.clear();
    // no trailing slash: callers append "/" themselves
    return "/PALM/Programs/" + name + suffix;
}
HLE(TwGetDBDataDirectory) {
    Db* db = a[0] ? dm::by_handle(a[0]) : dm::app_db();   // NULL = current application
    std::string d = data_dir_for(db);
    LOG("TwGetDBDataDirectory -> %s", d.c_str());
    if (a[1]) w16(a[1], VOL_REF);
    uint16_t maxLen = (uint16_t)a[3];
    if (a[2] && maxLen) { if (d.size() >= maxLen) return vfsErrorClass | 1; wstr(a[2], d); }
    return 0;
}
HLE(TwGetPRCDataDirectory) {
    OpenFile* f = file(a[0]);
    std::string d = "/PALM/Programs/";
    if (f) d += f->host.stem().string() + "/";
    uint16_t maxLen = (uint16_t)a[2];
    if (a[1] && maxLen) wstr(a[1], d.substr(0, maxLen - 1));
    return 0;
}

// ---- semaphores / threads (single-threaded emulation) ------------------------------
static std::map<uint32_t, int32_t> g_sems, g_sem_init;
static uint32_t g_next_sem = 1;
bool sems_all_free() {
    for (auto& [s, v] : g_sems) if (v < g_sem_init[s]) return false;
    return true;
}
HLE(TwSemaphoreCreate) { uint32_t s = g_next_sem++; g_sems[s] = g_sem_init[s] = (int32_t)a[1]; w32(a[0], s); return 0; }
HLE(TwSemaphoreDelete) { g_sems.erase(a[0]); return 0; }
HLE(TwSemaphoreAcquire) {
    auto it = g_sems.find(a[0]);
    if (it == g_sems.end()) return sysErrParamErr;
    it->second--;   // never blocks: there is only one thread
    return 0;
}
HLE(TwSemaphoreRelease) {
    auto it = g_sems.find(a[0]);
    if (it != g_sems.end()) it->second++;
    if (sems_all_free()) audio::pump();   // safe point: no critical section is active
    return 0;
}
HLE(TwThreadCurrent) { return 1; }
