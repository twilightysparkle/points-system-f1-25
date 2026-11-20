// Custom Points System, in-game part: picks a points table at random at the start of every session and writes it into the
// loaded game database (Gdb_HashMap_Insert, the call the Swofty core uses for its own database edits). The race results then
// score with it. Settings (swofty.json, binds native): random_mode off|history|all|chaos, random_fl, random_sprint.
#include <swofty/db.hpp>
#include <swofty/events.hpp>
#include <swofty/hud.hpp>
#include <swofty/mixin.hpp>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

SWOFTY_SETTING(std::string_view, random_mode, "off");
SWOFTY_SETTING(bool, random_fl, false);
SWOFTY_SETTING(bool, random_sprint, true);

namespace {

using Insert_t = void *(void *db, uint64_t key, uint8_t tag, char *text);

constexpr const char *INSERT_SIG =
    "41 57 41 56 41 55 41 54 56 57 55 53 48 81 EC D8 00 00 00 C5 F8 29 B4 24 C0 00 00 00 4C 89 CE 44 88 44 24 37";
constexpr const char *GLOBAL_SIG = "56 57 55 53 49 B8 25 23 22 84 E4 9C F2 CB 48 8B 15 ?? ?? ?? ?? 44 0F B6 11";

using Lookup_t = void **(const char *path);
constexpr const char *LOOKUP_SIG = "56 57 55 53 49 B8 25 23 22 84 E4 9C F2 CB";
Insert_t *insert_fn;
Lookup_t *lookup_fn;
uintptr_t image_base;
void **g_db;
std::mt19937 rng{std::random_device{}()};

struct Table {
    const char *name;
    std::vector<int> pts;
};

const std::vector<Table> HISTORY = {
    {"Modern 25-18-15", {25, 18, 15, 12, 10, 8, 6, 4, 2, 1}},
    {"2003-09 10-8-6", {10, 8, 6, 5, 4, 3, 2, 1}},
    {"1991-2002 10-6-4", {10, 6, 4, 3, 2, 1}},
    {"1961-90 9-6-4", {9, 6, 4, 3, 2, 1}},
    {"1950s 8-6-4", {8, 6, 4, 3, 2}},
    {"Double points", {50, 36, 30, 24, 20, 16, 12, 8, 4, 3, 2, 1}},
    {"Half points", {13, 9, 8, 6, 5, 4, 3, 2, 1}},
};
const std::vector<Table> FUN = {
    {"MotoGP style", {25, 20, 16, 13, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1}},
    {"Everyone scores", {23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1}},
    {"Winning matters", {35, 18, 15, 12, 10, 8, 6, 4, 2, 1}},
    {"Podium only", {5, 3, 1}},
    {"Winner takes all", {1}},
    {"Upside down", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23}},
};
const std::vector<Table> SPRINTS = {
    {"Sprint 8-1", {8, 7, 6, 5, 4, 3, 2, 1}},
    {"Sprint 3-2-1", {3, 2, 1}},
    {"Sprint F2 15-12", {15, 12, 10, 8, 6, 4, 2, 1}},
    {"Sprint 10-8-6", {10, 8, 6, 5, 4, 3, 2, 1}},
};

struct Fl {
    const char *name;
    int bonus;
    const char *rule;
};
const Fl FLS[] = {{"no fastest lap point", 0, "None"},
                  {"+1 fastest lap (top 10)", 1, "BestAndTop10"},
                  {"+1 fastest lap (best of top 10)", 1, "BestOfTop10"},
                  {"+2 fastest lap (top 10)", 2, "BestAndTop10"}};

std::string join(const std::vector<int> &l, double scale = 1.0)
{
    std::string s;
    for (size_t i = 0; i < 23; i++) {
        int v = i < l.size() ? int(l[i] * scale + 0.5) : 0;
        if (i)
            s += ',';
        s += std::to_string(v);
    }
    return s;
}

template <class T> const T &pick(const std::vector<T> &v)
{
    return v[std::uniform_int_distribution<size_t>(0, v.size() - 1)(rng)];
}

uint64_t fnv64(const std::string &s)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (unsigned char c : s) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

// Record class -> tag. The core's KB vtable table (gamedb.cpp KB_VT) is for another exe build, so the build's shift is
// measured on a record whose tag the SDK reads (an int), then every KB entry is moved by it. -1 = unknown class.
const uintptr_t KB_VT[19] = {0x5bee788, 0x5bee798, 0x5bee6b8, 0x5bee6b8, 0x5bee6b8, 0x56e6518, 0x5bee7a8,
                             0x5bee7b8, 0x5bee7c8, 0x5bee7d8, 0,         0x57a6730, 0x5bee7e8, 0x5bee6c8,
                             0x5bee7f8, 0,         0,         0x5bee6b8, 0x5bee6c8};
intptr_t vt_shift;
bool shift_known;

uintptr_t vt_at(const std::string &path)
{
    if (!lookup_fn)
        return 0;
    void **slot = lookup_fn(path.c_str());
    if (!slot || !*slot)
        return 0;
    return *reinterpret_cast<uintptr_t *>(*slot) - image_base;
}

void measure_shift()
{
    if (shift_known)
        return;
    const char *probe = "game_setup.session_standings.session_standing_preset_0.stat_id";
    swofty::db::Value v{};
    uintptr_t vt = vt_at(probe);
    if (vt && swofty::db::get(probe, &v) == swofty::db::Result::ok && v.tag == 0) {
        vt_shift = intptr_t(vt) - intptr_t(KB_VT[0]);
        shift_known = true;
    }
    swofty::log("[points-random] vtable shift from %s: %s (int class at base+0x%llx, KB base+0x%llx)", probe,
                shift_known ? "measured" : "NOT measured", (unsigned long long)vt, (unsigned long long)KB_VT[0]);
}

int tag_of_record(const std::string &path, uintptr_t *vt_out)
{
    uintptr_t vt = vt_at(path);
    if (vt_out)
        *vt_out = vt;
    if (!vt)
        return -1;
    swofty::db::Value v{};
    if (swofty::db::get(path, &v) == swofty::db::Result::ok)
        return v.tag;          // the SDK knows this class (int, float, string, enum, id, bool)
    measure_shift();
    if (!shift_known)
        return -1;
    for (int t = 0; t < 19; t++)
        if (KB_VT[t] && uintptr_t(intptr_t(KB_VT[t]) + vt_shift) == vt)
            return t;
    return -1;
}

// Overwrites an existing record, keeping its class. false when the key is missing, its class is unknown or the write changed it.
int writes_failed;

bool put(const std::string &path, const std::string &text)
{
    uintptr_t vt = 0;
    int tag = tag_of_record(path, &vt);
    if (tag < 0) {
        swofty::log("[points-random] %s: no usable record (vtable base+0x%llx), not written", path.c_str(),
                    (unsigned long long)vt);
        writes_failed++;
        return false;
    }
    std::string buf = text;
    insert_fn(*g_db, fnv64(path), uint8_t(tag), buf.data());
    int tag2 = tag_of_record(path, nullptr);
    if (tag2 != tag) {
        swofty::log("[points-random] %s: class changed by the write (tag %d -> %d)", path.c_str(), tag, tag2);
        writes_failed++;
        return false;
    }
    return true;
}

const char *RACE = "game_setup.session_standings.session_standing_preset_0";
const char *SPRINT = "game_setup.session_standings.session_standing_preset_11";
const double REDUCED[3] = {0.75, 0.5, 0.25};

std::string last_name, last_detail;
bool have_table = false;

void write_race(const std::vector<int> &l)
{
    put(std::string(RACE) + ".points", join(l));
    for (int i = 0; i < 3; i++)
        put(std::string(RACE) + ".reduced_points_data_" + std::to_string(i) + ".points", join(l, REDUCED[i]));
}

void roll()
{
    std::string_view mode = random_mode;
    if (mode == "off")
        return;
    if (!insert_fn || !lookup_fn || !g_db || !*g_db) {
        swofty::log("[points-random] game database functions not found; nothing randomised");
        return;
    }
    writes_failed = 0;
    std::vector<int> pts;
    std::string name;
    if (mode == "chaos") {
        std::uniform_int_distribution<int> n(6, 15);
        int cnt = n(rng);
        std::vector<int> pool;
        for (int i = 1; i <= 40; i++)
            pool.push_back(i);
        std::shuffle(pool.begin(), pool.end(), rng);
        pts.assign(pool.begin(), pool.begin() + cnt);
        name = "Chaos";
    } else {
        std::vector<Table> all = HISTORY;
        if (mode == "all")
            all.insert(all.end(), FUN.begin(), FUN.end());
        const Table &t = pick(all);
        pts = t.pts;
        name = t.name;
    }
    write_race(pts);
    std::string detail;
    for (size_t i = 0; i < pts.size() && i < 10; i++)
        detail += (i ? "-" : "") + std::to_string(pts[i]);
    if (pts.size() > 10)
        detail += "...";
    if (random_fl) {
        const Fl &f = FLS[std::uniform_int_distribution<size_t>(0, 3)(rng)];
        put(std::string(RACE) + ".fastest_lap_bonus", std::to_string(f.bonus));
        put(std::string(RACE) + ".fastest_lap_rule", f.rule);
        put(std::string(RACE) + ".reduced_points_data_0.fastest_lap_bonus", f.bonus ? "1" : "0");
        put(std::string(RACE) + ".reduced_points_data_0.fastest_lap_rule", f.rule);
        detail += std::string(", ") + f.name;
    }
    if (random_sprint) {
        const Table &s = pick(SPRINTS);
        put(std::string(SPRINT) + ".points", join(s.pts));
        detail += std::string(", ") + s.name;
    }
    last_name = name;
    last_detail = detail;
    have_table = true;
    if (writes_failed) {
        have_table = false;
        swofty::log("[points-random] %d write(s) failed; this session may score with a mixed table", writes_failed);
    }
    swofty::log("[points-random] this session scores: %s (%s)", name.c_str(), detail.c_str());
}

void on_session_start(const SwoftyEvent *, void *)
{
    std::string_view m = random_mode;
    swofty::log("[points-random] session.start (mode %.*s)", int(m.size()), m.data());
    roll();
}

void on_race_start(const SwoftyEvent *, void *)
{
    swofty::log("[points-random] race.start");
    if (have_table)
        swofty::hud::toast("Points: " + last_name, last_detail, 6.0f);
}

}

SWOFTY_ON_LOAD
{
    uintptr_t hit = 0;
    int n = swofty::api->scan(".xtls", INSERT_SIG, &hit);
    if (n >= 1)
        insert_fn = reinterpret_cast<Insert_t *>(hit);
    uintptr_t lh = 0;
    if (swofty::api->scan(".xtls", LOOKUP_SIG, &lh) >= 1)
        lookup_fn = reinterpret_cast<Lookup_t *>(lh);
    image_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    uintptr_t gh = 0;
    int m = swofty::api->scan(".xtls", GLOBAL_SIG, &gh);
    if (m >= 1) {
        int32_t disp = *reinterpret_cast<int32_t *>(gh + 0x11);
        g_db = reinterpret_cast<void **>(gh + 0x0e + 7 + disp);
    }
    std::string_view mode = random_mode;
    swofty::log("[points-random] loaded: Gdb_HashMap_Insert %d hit(s) at %p, Gdb_Lookup_Key %p, g_Gdb_Database slot %p (%d hit(s)), mode %.*s", n,
                reinterpret_cast<void *>(hit), reinterpret_cast<void *>(lh), static_cast<void *>(g_db), m, int(mode.size()), mode.data());
    if (swofty::events::available()) {
        swofty::events::subscribe(SWOFTY_EVENT_SESSION_START, on_session_start);
        swofty::events::subscribe(SWOFTY_EVENT_RACE_START, on_race_start);
    } else
        swofty::log("[points-random] events unavailable on this core; nothing will be randomised");
}

SWOFTY_ADDON()
