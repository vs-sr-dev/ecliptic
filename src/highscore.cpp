// TwHighScore HLE: local high score pools, persisted to ram/highscores.txt.
#include "palmos.h"
#include <algorithm>
#include <cstdio>
#include <map>
#include <vector>

constexpr uint32_t twHighScoreErrorBase = 0x7000 + 0x200;
constexpr uint32_t twHighScoreErrNotRegistered = twHighScoreErrorBase + 1;

struct Score { uint32_t score, checksum, date; std::string user; };
struct Table {
    uint16_t keepLocal = 10, keepServer = 0;
    bool report = false;
    std::string name;
    std::vector<Score> scores;   // sorted, best first
};
static std::map<std::pair<uint32_t, uint16_t>, Table> g_tables;
static bool g_loaded = false;
static const char* kFile = "ram/highscores.txt";

static void load() {
    if (g_loaded) return;
    g_loaded = true;
    FILE* f = fopen(kFile, "r");
    if (!f) return;
    char line[512];
    Table* cur = nullptr;
    while (fgets(line, sizeof line, f)) {
        unsigned cr, type, kl, ks, rep, sc, ck, dt;
        char name[256] = {0}, user[256] = {0};
        if (sscanf(line, "T %x %u %u %u %u %255[^\n]", &cr, &type, &kl, &ks, &rep, name) >= 5) {
            cur = &g_tables[{cr, (uint16_t)type}];
            cur->keepLocal = (uint16_t)kl; cur->keepServer = (uint16_t)ks; cur->report = rep; cur->name = name;
        } else if (cur && sscanf(line, "S %u %u %u %255[^\n]", &sc, &ck, &dt, user) >= 3) {
            cur->scores.push_back({sc, ck, dt, user});
        }
    }
    fclose(f);
}
static void save() {
    FILE* f = fopen(kFile, "w");
    if (!f) return;
    for (auto& [k, t] : g_tables) {
        fprintf(f, "T %08x %u %u %u %u %s\n", k.first, k.second, t.keepLocal, t.keepServer, t.report ? 1 : 0, t.name.c_str());
        for (auto& s : t.scores) fprintf(f, "S %u %u %u %s\n", s.score, s.checksum, s.date, s.user.c_str());
    }
    fclose(f);
}
static Table* find(uint32_t cr, uint32_t type) {
    load();
    auto it = g_tables.find({cr, (uint16_t)type});
    return it == g_tables.end() ? nullptr : &it->second;
}

HLE(TwHighScoreRegister) {
    load();
    Table& t = g_tables[{a[0], (uint16_t)a[1]}];
    t.keepLocal = (uint16_t)a[2];
    t.keepServer = (uint16_t)a[3];
    t.report = a[4] & 0xFF;
    t.name = rstr(a[5], 255);
    if (t.scores.size() > t.keepLocal) t.scores.resize(t.keepLocal);
    save();
    return 0;
}
HLE(TwHighScoreUnregister) {
    load();
    g_tables.erase({a[0], (uint16_t)a[1]});
    save();
    return 0;
}
HLE(TwHighScoreReport) {
    Table* t = find(a[0], a[1]);
    if (!t) return twHighScoreErrNotRegistered;
    uint32_t p = a[2];
    Score s{r32(p), r32(p + 4), r32(p + 8), r32(p + 12) ? rstr(r32(p + 12), 255) : ""};
    t->scores.push_back(s);
    std::stable_sort(t->scores.begin(), t->scores.end(), [](const Score& x, const Score& y) { return x.score > y.score; });
    if (t->scores.size() > t->keepLocal) t->scores.resize(t->keepLocal);
    save();
    return 0;
}
HLE(TwHighScoreGetSummary) {
    Table* t = find(a[0], a[1]);
    if (!t) return 0;
    uint32_t n = 44 + (uint32_t)t->name.size() + 1;
    uint32_t p = heap::alloc(n, true);
    if (!p) return 0;
    w16(p, t->keepLocal); w16(p + 2, t->keepServer); w8(p + 4, t->report);
    w32(p + 8, 0);                       // timeSynced
    w32(p + 12, p + 44);                 // gameName (inside the chunk)
    w16(p + 16, (uint16_t)t->scores.size());
    w16(p + 18, 0);
    w32(p + 20, t->scores.empty() ? 0 : t->scores.front().score);
    w32(p + 24, t->scores.empty() ? 0 : t->scores.back().score);
    w32(p + 28, 0); w32(p + 32, 0);
    wstr(p + 44, t->name);
    return p;
}
HLE(TwHighScoreGetDetails) {
    Table* t = find(a[0], a[1]);
    uint16_t idx = (uint16_t)a[3];
    if (!t || (a[2] & 0xFFFF) != 0 || idx >= t->scores.size()) return 0;
    const Score& s = t->scores[idx];
    uint32_t p = heap::alloc(16 + (uint32_t)s.user.size() + 1, true);
    if (!p) return 0;
    w32(p, s.score); w32(p + 4, s.checksum); w32(p + 8, s.date);
    w32(p + 12, s.user.empty() ? 0 : p + 16);
    wstr(p + 16, s.user);
    return p;
}
HLE(TwHighScoreGetTournament) { if (a[2]) w8(a[2], 0); return 0; }
