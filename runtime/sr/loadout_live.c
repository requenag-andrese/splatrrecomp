// sr-recomp: the loadout the player carries, changed while the game runs (sr-config.ini loadout = 1 and the loadout_*
// keys below). No restart, no table of the game rewritten, and the save is not touched.
//
// The game keeps what each player carries in one object: a manager it points to from a fixed place holds four player
// slots, the first is the local player, and its loadout object has the tank, the weapon with its level and powers, the
// relic powers and four gadget slots (L, R, A and the Bot Buddy's Showstopper) with the parts fitted to each. The
// player's actor follows that object: with bit 0 of the slot's flags set, the game equips what the object says within a
// frame or two. The game fills the object from the save when it loads it and again when a raid or an island starts.
//
// So this keeps the loadout the settings ask for apart and writes it over the game's: a thread looks at the object 20
// times a second, and when the fields it owns are not what they should be (the game filled the object again, or a
// setting changed) it writes them and sets the flag. What the game had there is remembered, so a key taken away, or
// loadout = 0, gives the player's own back at once. The Gear screen of the game shows what the save has, not this.
//
// Names are the game's internal ones and are looked up in the game's own name tables in memory, so none of them is
// kept here; the number the object holds is a name's place in its table less one (-1: none).
//
//   loadout = 1
//   loadout_weapon = Roller_Heavy_F            a weapon (20 kinds, A to C the three variants, D to F the same as Primo)
//   loadout_weapon_lv = 50
//   loadout_weapon_powers = Wpn_ToFrozen:3,none,none     up to three powers of the weapon with their levels
//   loadout_tank = Speed                       Default, Speed, Power, Tactical (or the game's Agility, Strength, Technique)
//   loadout_gadget_l = Boomerang               the gadget of L (also _r, _a); none: an empty slot
//   loadout_parts_l = Boom_11_Multi_02,Boom_01_SizeUpXZ_16    its parts, up to nine, by the names of the game's part
//                                              files; unset: the slot's own when the gadget is the one it had, else none
//   loadout_buddy = Frye                       Shiver, Frye, BigMan: the Bot Buddy, through its Showstopper
//   loadout_powers = Cmn_MultiJump:3,none      up to five relic powers with their levels
//                                              (in both lists "none" empties a slot, "own" leaves it as the game has it)
// A key that is not set leaves that part as the game has it. The object's layout and the places below are those of
// version 1.1.0 of the game; the tables are checked before anything is written, and another version stays untouched.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/cfg.h"
#include "../core/dispatch.h"
#include "../core/host.h"
#include "../core/platform/platform.h"
#include "loadout_live.h"

uint32_t loadout_hash(const char *s);   // loadout.c: the game's id of a name

// ---- the fields this may write, in one fixed order (the object's layout, as offsets from its start)
enum {
    OBJ_TANK = 0x10c,
    OBJ_WEAPON = 0x7f8,           // type, then level
    OBJ_WEAPON_POWER = 0x820,     // three elements of 0x80: (type, level) at +0x70
    OBJ_RELIC_POWER = 0x528,      // five elements of 0x78: (type, level) at +0x70
    OBJ_GADGET = 0x9d0,           // four elements of 0xb8: type +0x70, parts count +0x88, part ids +0x8c, "empty" +0xb0
    OBJ_GADGET_EXTRA = 0xcc8,     // four elements of 0xa8 beside the gadgets: a count +0x80 that follows the parts'
};

// F_GADGET + slot * G_N + ...
enum { G_TYPE, G_EMPTY, G_COUNT, G_EXTRA_COUNT, G_PART, G_N = G_PART + LIVE_PARTS };
enum {
    F_TANK,
    F_WEAPON,
    F_WEAPON_LV,
    F_WEAPON_POWER,                                    // (type, level) x 3
    F_RELIC_POWER = F_WEAPON_POWER + 2 * LIVE_WEAPON_POWERS,   // (type, level) x 5
    F_GADGET = F_RELIC_POWER + 2 * LIVE_RELIC_POWERS,
    F_COUNT = F_GADGET + LIVE_GADGETS * G_N,
};
_Static_assert(F_COUNT == LIVE_FIELDS, "loadout_live.h LIVE_FIELDS");

uint32_t loadout_live_offset(int field) {
    if (field == F_TANK) return OBJ_TANK;
    if (field == F_WEAPON) return OBJ_WEAPON;
    if (field == F_WEAPON_LV) return OBJ_WEAPON + 4;
    if (field < F_RELIC_POWER) return OBJ_WEAPON_POWER + (uint32_t)(field - F_WEAPON_POWER) / 2 * 0x80 + 0x70 + (uint32_t)(field - F_WEAPON_POWER) % 2 * 4;
    if (field < F_GADGET) return OBJ_RELIC_POWER + (uint32_t)(field - F_RELIC_POWER) / 2 * 0x78 + 0x70 + (uint32_t)(field - F_RELIC_POWER) % 2 * 4;
    uint32_t slot = (uint32_t)(field - F_GADGET) / G_N, k = (uint32_t)(field - F_GADGET) % G_N, at = OBJ_GADGET + slot * 0xb8;
    if (k == G_TYPE) return at + 0x70;
    if (k == G_EMPTY) return at + 0xb0;
    if (k == G_COUNT) return at + 0x88;
    if (k == G_EXTRA_COUNT) return OBJ_GADGET_EXTRA + slot * 0xa8 + 0x80;
    return at + 0x8c + (k - G_PART) * 4;
}

static const char *setting(const char *name) {
    const char *v = cfg_get(name);
    return v && *v ? v : NULL;
}

int loadout_live_wanted(void) {
    const char *on = cfg_get("SR_LOADOUT");
    return on && on[0] == '1';
}

// The next item of a comma list, trimmed, into out; NULL at the end.
static const char *next_item(const char *list, char *out, size_t cap) {
    while (*list == ' ' || *list == ',') list++;
    if (!*list) return NULL;
    size_t n = 0;
    while (*list && *list != ',') {
        if (n + 1 < cap) out[n++] = *list;
        list++;
    }
    while (n && out[n - 1] == ' ') n--;
    out[n] = 0;
    return list;
}

static int is_none(const char *s) { return !strcmp(s, "none") || !strcmp(s, "None") || !strcmp(s, "Invalid"); }

// A name's number in one of the game's tables; "none" is -1. Returns 0 and says so once when the game has no such name.
static int number(const LiveNames *names, int table, const char *key, const char *name, uint32_t *out) {
    if (is_none(name)) {
        *out = (uint32_t)-1;
        return 1;
    }
    int n = names->index_of(names->ctx, table, name);
    if (n < 0) {
        rt_log("loadout: %s: the game has no \"%s\"; left as it is", key, name);
        return 0;
    }
    *out = (uint32_t)n;
    return 1;
}

// "Name:Lv, Name, none" into (type, level) pairs from `first` on, at most `slots`.
static void powers(const LiveNames *names, const char *key, uint32_t *target, int first, int slots) {
    const char *list = setting(key);
    char item[96];
    for (int i = 0; list && i < slots && (list = next_item(list, item, sizeof item)); i++) {
        if (!strcmp(item, "own")) continue;   // this slot stays as the game has it
        uint32_t lv = 1, type;
        char *colon = strrchr(item, ':');
        if (colon) {
            *colon = 0;
            lv = (uint32_t)strtoul(colon + 1, NULL, 10);
        }
        if (!number(names, LIVE_TABLE_POWER, key, item, &type)) continue;
        target[first + 2 * i] = type;
        target[first + 2 * i + 1] = lv ? lv : 1;
    }
}

static void gadget(const LiveNames *names, const uint32_t *game, uint32_t *target, int slot, const char *gadget_key,
                   const char *parts_key) {
    uint32_t *t = target + F_GADGET + slot * G_N;
    const uint32_t *g = game + F_GADGET + slot * G_N;
    const char *name = setting(gadget_key), *parts = parts_key ? setting(parts_key) : NULL;
    uint32_t type;
    if (name && number(names, LIVE_TABLE_GADGET, gadget_key, name, &type)) {
        t[G_TYPE] = type;
        t[G_EMPTY] = type == (uint32_t)-1;
        if (type != g[G_TYPE]) {   // another gadget: the parts of the one that was there do not fit it
            t[G_COUNT] = t[G_EXTRA_COUNT] = 0;
            memset(t + G_PART, 0, LIVE_PARTS * sizeof *t);
        }
    }
    if (parts && t[G_TYPE] != (uint32_t)-1) {
        char item[96];
        uint32_t n = 0;
        memset(t + G_PART, 0, LIVE_PARTS * sizeof *t);
        while (n < LIVE_PARTS && (parts = next_item(parts, item, sizeof item)))
            if (!is_none(item)) t[G_PART + n++] = item[0] == '#' ? (uint32_t)strtoul(item + 1, NULL, 10) : loadout_hash(item);
        t[G_COUNT] = t[G_EXTRA_COUNT] = n;
    }
}

// What the object's fields should hold: the game's own (`game`, LIVE_FIELDS values) with what the settings ask for
// written over them. Returns how many fields differ from the game's.
int loadout_live_compose(const LiveNames *names, const uint32_t *game, uint32_t *target) {
    memcpy(target, game, LIVE_FIELDS * sizeof *target);
    if (!loadout_live_wanted()) return 0;
    const char *v;
    uint32_t n;
    if ((v = setting("SR_LOADOUT_TANK"))) {
        static const struct { const char *name; uint32_t tank; } tanks[] = {
            {"Default", 0}, {"Speed", 1}, {"Agility", 1}, {"Power", 2}, {"Strength", 2}, {"Tactical", 3}, {"Technique", 3}};
        size_t i = 0;
        while (i < sizeof tanks / sizeof *tanks && strcmp(tanks[i].name, v)) i++;
        if (i < sizeof tanks / sizeof *tanks) target[F_TANK] = tanks[i].tank;
        else rt_log("loadout: loadout_tank: \"%s\" is not Default, Speed, Power or Tactical; left as it is", v);
    }
    if ((v = setting("SR_LOADOUT_WEAPON")) && number(names, LIVE_TABLE_WEAPON, "loadout_weapon", v, &n)) target[F_WEAPON] = n;
    if ((v = setting("SR_LOADOUT_WEAPON_LV")) && strtoul(v, NULL, 10) >= 1) target[F_WEAPON_LV] = (uint32_t)strtoul(v, NULL, 10);
    powers(names, "SR_LOADOUT_WEAPON_POWERS", target, F_WEAPON_POWER, LIVE_WEAPON_POWERS);
    powers(names, "SR_LOADOUT_POWERS", target, F_RELIC_POWER, LIVE_RELIC_POWERS);
    gadget(names, game, target, 0, "SR_LOADOUT_GADGET_L", "SR_LOADOUT_PARTS_L");
    gadget(names, game, target, 1, "SR_LOADOUT_GADGET_R", "SR_LOADOUT_PARTS_R");
    gadget(names, game, target, 2, "SR_LOADOUT_GADGET_A", "SR_LOADOUT_PARTS_A");
    if ((v = setting("SR_LOADOUT_BUDDY"))) {   // the Bot Buddy rides with its Showstopper, the fourth gadget
        static const struct { const char *name, *special; } buddies[] = {
            {"Shiver", "SpSharkCharge"}, {"Fuka", "SpSharkCharge"},   {"Frye", "SpMorayShower"},      {"Utsuho", "SpMorayShower"},
            {"BigMan", "SpMantaSurfing"}, {"Big Man", "SpMantaSurfing"}, {"Mantaro", "SpMantaSurfing"}};
        size_t i = 0;
        while (i < sizeof buddies / sizeof *buddies && strcmp(buddies[i].name, v)) i++;
        if (i == sizeof buddies / sizeof *buddies) rt_log("loadout: loadout_buddy: \"%s\" is not Shiver, Frye or BigMan; left as it is", v);
        else if (number(names, LIVE_TABLE_GADGET, "loadout_buddy", buddies[i].special, &n)) target[F_GADGET + 3 * G_N + G_TYPE] = n;
    }
    int changed = 0;
    for (int i = 0; i < LIVE_FIELDS; i++) changed += target[i] != game[i];
    return changed;
}

// One look at the object's fields (`now`): what to write, if anything. `st` carries the game's own values between
// looks; `fresh` says the settings changed since the last look. Returns 1 when `out` (LIVE_FIELDS values) is to be
// written and the flag set.
int loadout_live_step(LiveState *st, const LiveNames *names, const uint32_t *now, int fresh, uint32_t *out) {
    if (fresh) st->dirty = 1;
    if (!st->seen) {
        memcpy(st->game, now, sizeof st->game);
        st->seen = 1;
    } else if (memcmp(now, st->target, sizeof st->target)) {
        // Not what was written: the game filled the object again. A second look confirms it, so a fill caught half
        // way is not taken for the game's values.
        if (!st->settling || memcmp(now, st->settle, sizeof st->settle)) {
            memcpy(st->settle, now, sizeof st->settle);
            st->settling = 1;
            return 0;
        }
        memcpy(st->game, now, sizeof st->game);
    } else if (!st->dirty) {
        return 0;
    }
    st->settling = st->dirty = 0;
    loadout_live_compose(names, st->game, st->target);
    if (!memcmp(now, st->target, sizeof st->target)) return 0;
    memcpy(out, st->target, sizeof st->target);
    return 1;
}

#ifdef _WIN32
#include <windows.h>

// ---- version 1.1.0 of the game: where things are in main's image
enum {
    MAIN_MANAGER = 0x4e00a60,   // pointer to the manager of the players' loadouts
    MANAGER_SLOT0 = 0x68,       // the local player's slot; the loadout object is at +0x10
    SLOT_VALID = 4,             // byte: 1 once the slot holds a player
    SLOT_PRESET = 8,            // the kind of loadout the game itself imposed (a challenge, the tutorial); 0: none
    SLOT_OBJECT = 0x10,
    SLOT_FLAGS = 0x1040,        // byte: bit 0 "the loadout changed, equip it"
    SLOT_SIZE = 0x1068,
};
static const uint32_t main_table[LIVE_TABLES] = {
    [LIVE_TABLE_WEAPON] = 0x4a58ff0, [LIVE_TABLE_GADGET] = 0x4a38498, [LIVE_TABLE_POWER] = 0x4b48120};

static uint64_t main_base, main_size;

static int readable(uint64_t at, uint64_t n, int writable) {
    MEMORY_BASIC_INFORMATION mi;
    if (!at || !VirtualQuery((void *)(uintptr_t)at, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & PAGE_GUARD)) return 0;
    DWORD p = mi.Protect & 0xFF;
    if (writable ? p != PAGE_READWRITE && p != PAGE_EXECUTE_READWRITE
                 : p != PAGE_READONLY && p != PAGE_READWRITE && p != PAGE_EXECUTE_READ && p != PAGE_EXECUTE_READWRITE)
        return 0;
    return (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize >= at + n;
}

// Entry i of a name table of the game (pointers to strings inside main's image), NULL past its end.
static const char *table_name(int table, int i) {
    uint64_t at = main_base + main_table[table] + (uint64_t)i * 8, p;
    if (at + 8 > main_base + main_size || !readable(at, 8, 0)) return NULL;
    memcpy(&p, (void *)(uintptr_t)at, 8);
    if (p < main_base || p >= main_base + main_size - 64 || !readable(p, 64, 0)) return NULL;
    const char *s = (const char *)(uintptr_t)p;
    size_t n = 0;
    while (n < 63 && s[n] > 32 && s[n] < 127) n++;
    return n && !s[n] ? s : NULL;
}

static int index_of(void *ctx, int table, const char *name) {
    (void)ctx;
    const char *s;
    for (int i = 1; i < 512 && (s = table_name(table, i)); i++)
        if (!strcmp(s, name)) return i - 1;
    return -1;
}

// The tables are where this version has them: each starts with the game's "none".
static int known_version(void) {
    for (int t = 0; t < LIVE_TABLES; t++) {
        const char *first = table_name(t, 0);
        if (!first || strcmp(first, "Invalid") || !table_name(t, 1)) return 0;
    }
    return 1;
}

static void live_thread(void *arg) {
    (void)arg;
    static const LiveNames names = {index_of, NULL};
    static LiveState st;
    static uint32_t now[LIVE_FIELDS], out[LIVE_FIELDS];
    unsigned seen_version = 0, applied = 0;
    int checked = 0, preset_said = 0, burst = 0, quiet = 0;
    for (;; plat_sleep_ms(50)) {
        uint64_t ptr_at = main_base + MAIN_MANAGER, manager;
        if (!readable(ptr_at, 8, 0)) continue;   // the game's image is not there yet
        memcpy(&manager, (void *)(uintptr_t)ptr_at, 8);
        uint64_t slot = manager + MANAGER_SLOT0, object = slot + SLOT_OBJECT;
        if (!manager || !readable(slot, SLOT_SIZE, 1)) continue;
        const uint8_t *s = (const uint8_t *)(uintptr_t)slot;
        if (s[SLOT_VALID] != 1) continue;
        if (!checked) {
            checked = 1;
            if (!known_version()) {
                rt_log("loadout: this version of the game is not the one the live loadout knows (1.1.0); nothing is changed");
                return;
            }
            rt_log("loadout: live: the player's loadout object is at %#llx", (unsigned long long)object);
        }
        uint32_t preset;
        memcpy(&preset, s + SLOT_PRESET, 4);
        if (preset) {   // a challenge's or the tutorial's own loadout: the game's, left alone
            if (!preset_said) rt_log("loadout: live: the game imposes a loadout here (kind %u); left alone", preset);
            preset_said = 1;
            st.seen = 0;
            continue;
        }
        preset_said = 0;
        for (int i = 0; i < LIVE_FIELDS; i++) memcpy(now + i, (void *)(uintptr_t)(object + loadout_live_offset(i)), 4);
        unsigned version = cfg_version();
        int fresh = version != seen_version || !st.seen;
        seen_version = version;
        if (!loadout_live_step(&st, &names, now, fresh, out)) {
            if (++quiet >= 40) burst = 0;   // two seconds without a write
            continue;
        }
        quiet = 0;
        if (++burst > 8) {   // the game keeps putting something back (a value it does not take): no fight over it
            rt_log("loadout: live: the game changed the loadout back %d times in a row; waiting 10 s", burst - 1);
            plat_sleep_ms(10000);
            burst = 0;
            st.seen = 0;
            continue;
        }
        for (int i = 0; i < LIVE_FIELDS; i++)
            if (out[i] != now[i]) memcpy((void *)(uintptr_t)(object + loadout_live_offset(i)), out + i, 4);
        ((uint8_t *)(uintptr_t)slot)[SLOT_FLAGS] |= 1;
        if (applied++ < 3 || applied % 20 == 0) rt_log("loadout: live: written (%u times so far)", applied);
    }
}

void loadout_live_start(void) {
    for (int i = 0; i < rt_layout_count; i++)
        if (!strcmp(rt_layout[i].name, "main")) main_base = rt_layout[i].base, main_size = rt_layout[i].size;
    if (!main_base || main_size <= MAIN_MANAGER) return;
    plat_thread_spawn(live_thread, NULL, "loadout", 0);
}
#else
void loadout_live_start(void) {}
#endif
