// Palm OS constants and the host-side database model shared by the HLE modules.
#pragma once
#include "emu.h"
#include <string>
#include <vector>

constexpr uint16_t errNone = 0;
constexpr uint16_t memErrorClass = 0x0100, dmErrorClass = 0x0200, sysErrorClass = 0x0500,
                   sndErrorClass = 0x0800, expErrorClass = 0x2900, vfsErrorClass = 0x2A00;
constexpr uint16_t memErrChunkLocked = memErrorClass | 1, memErrNotEnoughSpace = memErrorClass | 2,
                   memErrInvalidParam = memErrorClass | 3;
constexpr uint16_t dmErrMemError = dmErrorClass | 1, dmErrIndexOutOfRange = dmErrorClass | 2,
                   dmErrInvalidParam = dmErrorClass | 3, dmErrReadOnly = dmErrorClass | 4,
                   dmErrCantOpen = dmErrorClass | 6, dmErrCantFind = dmErrorClass | 7,
                   dmErrNotRecordDB = dmErrorClass | 12, dmErrNotResourceDB = dmErrorClass | 13,
                   dmErrResourceNotFound = dmErrorClass | 16, dmErrNoOpenDatabase = dmErrorClass | 17,
                   dmErrUniqueIDNotFound = dmErrorClass | 24, dmErrAlreadyExists = dmErrorClass | 25;
constexpr uint16_t ftrErrNoSuchFeature = 0x0C00 | 2;
constexpr uint16_t sysErrParamErr = sysErrorClass | 3, sysErrNotAllowed = sysErrorClass | 4,
                   sysErrLibNotFound = sysErrorClass | 10;

constexpr uint16_t dmHdrAttrResDB = 0x0001;
constexpr uint8_t dmRecAttrDelete = 0x80, dmRecAttrDirty = 0x40, dmRecAttrBusy = 0x20, dmRecAttrSecret = 0x10;

inline uint32_t fourcc(const char* s) { return (uint32_t)(uint8_t)s[0] << 24 | (uint8_t)s[1] << 16 | (uint8_t)s[2] << 8 | (uint8_t)s[3]; }
std::string fourcc_str(uint32_t v);

struct DbRec {
    uint32_t type = 0;   // resource type (resource DBs)
    uint16_t id = 0;     // resource id (resource DBs)
    uint32_t h = 0;      // MemHandle with the data (0 = empty record)
    uint8_t attr = 0;    // record attributes + category (record DBs)
    uint32_t uid = 0;    // unique id (record DBs)
};

struct Db {
    std::string name;
    uint32_t type = 0, creator = 0;
    uint16_t attr = 0, version = 0;
    uint32_t crDate = 0, modDate = 0, bckDate = 0, modNum = 0;
    uint32_t appInfoH = 0, sortInfoH = 0;
    uint32_t uidSeed = 0;
    std::vector<DbRec> recs;
    uint32_t dbH = 0;          // opaque MemHandle identifying the database
    int openCount = 0;
    bool deleted = false;
    bool dirty = false;
    std::string save_path;     // host file written back on exit ("" = never saved)
    bool res() const { return attr & dmHdrAttrResDB; }
};

namespace dm {
Db* load_file(const std::string& path, bool persistent);  // PRC/PDB -> new Db
Db* by_handle(uint32_t dbH);
Db* by_name(const std::string& name);
Db* by_open_ref(uint32_t ref);
uint32_t open(Db* db, uint16_t mode);   // returns DmOpenRef
Db* app_db();
void set_app_db(Db* db);
const std::vector<Db*>& all();
uint32_t find_resource_any(uint32_t type, uint16_t id);  // searches open DBs, newest first
void save_all();
void set_ram_dir(const std::string& dir);
}
