// sr-recomp: a loadout of the player's choosing for the raids (sr-config.ini loadout = 1 and the loadout_* keys).
//
// The game's raid table (core/romfs.c RAIDTABLE, a BYML: one dictionary per raid) can fix what the player carries in a
// raid:
//   ChallengeWeapon      {Type, Lv, Affix[3], AffixLv[3]}     the weapon, its level, three bonuses and their levels
//   ChallengeGadgetA/L/R {Type, Parts[]}                      a gadget per slot and the parts fitted to it (paths)
//   ChallengeStyle, ChallengePlayerLevel
// Types are the 32-bit MurmurHash3 of the game's internal names ("Invalid": none). loadout_apply writes the loadout the
// settings give into every raid (loadout_raids = free: all but the game's challenge raids), and core/romfs.c serves the
// table so written in the game's place (loadout_overlay). The save is not touched; the game reads the table at start.
//
// The game gives a raid these fields only when the raid's DungeonType is Challenge or ChallengeEx (main+0x29807dc:
// the row's type, then main+0x297a800 which empties the loadout the player came with and fills it from the row). The
// patch set `loadout` (srrecomp/patches) takes that test out, and core/romfs.c turns the set on with the table. So a
// raid gives the row whole: a slot the settings do not name is what the game's row has there, which is nothing in
// an ordinary raid (no weapon, an empty slot, the row's style), not what the player carries in the hub.
//
//   loadout = 1 and loadout_table = 1
//   loadout_weapon = Shooter_A          the weapon's row name in the game's weapon table; unset: the row's own
//   loadout_weapon_lv = 50
//   loadout_affix = Name:3,Name:1,#123456:2     up to three bonuses with their levels; "#n" gives the hash itself
//   loadout_gadget_a = Boomerang        the gadget of slot A (also _l, _r); "none": an empty slot; unset: the row's own
//   loadout_parts_a = Boom_01_SizeUpXZ_16,Boom_02_SizeUpY_05       its parts, by the names of the game's part files
//   loadout_style = Agility             Default, Agility, Strength, Technique; unset: the row's own
//   loadout_player_lv = 0               1 or more; unset or less: the player's own level
//   loadout_raids = all | free
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/cfg.h"
#include "../core/host.h"
#include "byml.h"

// MurmurHash3 (x86, 32 bits, seed 0) of a name: the game's ids.
uint32_t loadout_hash(const char *s) {
    const uint32_t c1 = 0xcc9e2d51, c2 = 0x1b873593;
    size_t len = strlen(s), n = len & ~(size_t)3;
    uint32_t h = 0, k;
    for (size_t i = 0; i < n; i += 4) {
        memcpy(&k, s + i, 4);
        k *= c1, k = k << 15 | k >> 17, k *= c2;
        h ^= k, h = h << 13 | h >> 19, h = h * 5 + 0xe6546b64;
    }
    k = 0;
    switch (len & 3) {
    case 3: k ^= (uint32_t)(uint8_t)s[n + 2] << 16;   // fall through
    case 2: k ^= (uint32_t)(uint8_t)s[n + 1] << 8;    // fall through
    case 1:
        k ^= (uint8_t)s[n];
        k *= c1, k = k << 15 | k >> 17, k *= c2, h ^= k;
    }
    h ^= (uint32_t)len;
    h ^= h >> 16, h *= 0x85ebca6b, h ^= h >> 13, h *= 0xc2b2ae35, h ^= h >> 16;
    return h;
}

static const char *setting(const char *name) {
    const char *v = cfg_get(name);
    return v && *v ? v : NULL;
}

// The table is rewritten only when asked by name (loadout_table = 1): loadout = 1 alone is the live loadout
// (loadout_live.c), which needs no table and works in every raid.
int loadout_wanted(void) {
    const char *on = cfg_get("SR_LOADOUT"), *table = cfg_get("SR_LOADOUT_TABLE");
    return on && on[0] == '1' && table && table[0] == '1';
}

// "#123" is the hash itself, anything else a name.
static uint32_t id_of(const char *name) { return name[0] == '#' ? (uint32_t)strtoul(name + 1, NULL, 10) : loadout_hash(name); }

// A scalar like the one the table has at this place (its type), with another value; `fallback` when there is none.
static BymlNode *like(Byml *b, const BymlNode *was, uint8_t fallback, uint32_t raw) {
    return byml_scalar(b, was && !was->items && !was->str && !was->blob ? was->type : fallback, raw);
}

// The next item of a comma-separated list into out (trimmed); NULL at its end.
static const char *next_item(const char *list, char *out, size_t cap) {
    while (*list == ' ' || *list == ',') list++;
    if (!*list) return NULL;
    size_t n = strcspn(list, ",");
    size_t keep = n < cap - 1 ? n : cap - 1;
    memcpy(out, list, keep);
    out[keep] = 0;
    while (keep && out[keep - 1] == ' ') out[--keep] = 0;
    return list + n;
}

// row.ChallengeWeapon with the weapon of the settings. 1 when the row changed.
static int set_weapon(Byml *b, BymlNode *row) {
    const char *weapon = setting("SR_LOADOUT_WEAPON"), *lv = setting("SR_LOADOUT_WEAPON_LV"), *affix = setting("SR_LOADOUT_AFFIX");
    const BymlNode *was = byml_get(row, "ChallengeWeapon");
    if ((!weapon && !lv && !affix) || !was) return 0;
    BymlNode *w = byml_clone(b, was);
    if (!w) return 0;
    if (weapon) byml_set(w, "Type", like(b, byml_get(was, "Type"), BYML_UINT, id_of(weapon)));
    if (lv) byml_set(w, "Lv", like(b, byml_get(was, "Lv"), BYML_INT, (uint32_t)atoi(lv)));
    if (affix) {
        const BymlNode *wa = byml_get(was, "Affix"), *wl = byml_get(was, "AffixLv");
        const BymlNode *ea = wa && wa->n ? wa->items[0] : NULL, *el = wl && wl->n ? wl->items[0] : NULL;
        BymlNode *names[3], *levels[3];
        char item[96];
        const char *p = affix;
        for (int i = 0; i < 3; i++) {
            uint32_t id = loadout_hash("Invalid"), level = 1;
            if (p && (p = next_item(p, item, sizeof item))) {
                char *colon = strchr(item, ':');
                if (colon) *colon = 0, level = (uint32_t)atoi(colon + 1);
                id = id_of(item);
            }
            names[i] = like(b, ea, BYML_UINT, id), levels[i] = like(b, el, BYML_INT, level);
        }
        byml_set(w, "Affix", byml_array(b, names, 3));
        byml_set(w, "AffixLv", byml_array(b, levels, 3));
    }
    return byml_set(row, "ChallengeWeapon", w);
}

// row.ChallengeGadget<slot> with the gadget and the parts of the settings.
static int set_gadget(Byml *b, BymlNode *row, char slot) {
    char key[40], gname[40], pname[40];
    snprintf(key, sizeof key, "ChallengeGadget%c", slot);
    snprintf(gname, sizeof gname, "SR_LOADOUT_GADGET_%c", slot);
    snprintf(pname, sizeof pname, "SR_LOADOUT_PARTS_%c", slot);
    const char *gadget = setting(gname), *parts = setting(pname);
    const BymlNode *was = byml_get(row, key);
    if (!gadget || !was) return 0;   // parts alone say nothing: they belong to a gadget
    BymlNode *g = byml_clone(b, was);
    if (!g) return 0;
    int none = !strcmp(gadget, "none");
    byml_set(g, "Type", like(b, byml_get(was, "Type"), BYML_UINT, none ? loadout_hash("Invalid") : id_of(gadget)));
    BymlNode *list[16];
    uint32_t n = 0;
    char item[96], path[160];
    for (const char *p = none ? NULL : parts; p && n < 16 && (p = next_item(p, item, sizeof item));) {
        snprintf(path, sizeof path, "Work/Gyml/GadgetParts/%s.spl__GadgetPartsInfo.gyml", item);
        if ((list[n] = byml_string(b, path))) n++;
    }
    byml_set(g, "Parts", byml_array(b, list, n));
    return byml_set(row, key, g);
}

int loadout_apply(Byml *b) {
    BymlNode *root = byml_root(b);
    if (root->type != BYML_ARRAY) {
        rt_log("loadout: the raid table is not a list of raids; the raids keep their own loadouts");
        return 0;
    }
    const char *raids = setting("SR_LOADOUT_RAIDS"), *style = setting("SR_LOADOUT_STYLE"), *plv = setting("SR_LOADOUT_PLAYER_LV");
    int free_only = raids && !strcmp(raids, "free");
    const uint32_t challenge[] = {loadout_hash("Challenge"), loadout_hash("ChallengeEx")};
    int rows = 0, skipped = 0;
    for (uint32_t i = 0; i < root->n; i++) {
        BymlNode *row = root->items[i];
        if (row->type != BYML_DICT) continue;
        if (free_only) {   // the two types whose loadout the game gives by itself: its challenges
            const BymlNode *t = byml_get(row, "DungeonType");
            if (t && (t->raw == challenge[0] || t->raw == challenge[1])) {
                skipped++;
                continue;
            }
        }
        int changed = set_weapon(b, row);
        changed |= set_gadget(b, row, 'A') | set_gadget(b, row, 'L') | set_gadget(b, row, 'R');
        if (style) changed |= byml_set(row, "ChallengeStyle", like(b, byml_get(row, "ChallengeStyle"), BYML_UINT, id_of(style)));
        if (plv) changed |= byml_set(row, "ChallengePlayerLevel", like(b, byml_get(row, "ChallengePlayerLevel"), BYML_INT, (uint32_t)atoi(plv)));
        rows += changed;
    }
    if (!rows) rt_log("loadout: nothing to write (no loadout_* setting names a weapon, a gadget, a style or a level, or the "
                      "table has no such fields); the raids keep their own loadouts");
    else rt_log("loadout: written into %d raids%s", rows, skipped ? " (the game's challenge raids left as they are)" : "");
    return rows;
}

// The unpacked raid table with the loadout written in: a new table (malloc'd), or NULL when nothing changed or the
// result is larger than the original (the game sizes its buffer for the original; core/romfs.c says so).
uint8_t *loadout_table_build(const uint8_t *raw, size_t n, size_t *out_n) {
    Byml *b = byml_parse(raw, n);
    if (!b) {
        rt_log("loadout: the raid table is not laid out as expected; the raids keep their own loadouts");
        return NULL;
    }
    uint8_t *out = loadout_apply(b) ? byml_write(b, out_n) : NULL;
    byml_free(b);
    if (out && *out_n > n) {
        rt_log("loadout: the table with the loadout is %zu bytes, the game's is %zu and its buffer is sized for that; "
               "fewer or shorter parts may fit. The raids keep their own loadouts", *out_n, n);
        free(out);
        out = NULL;
    }
    return out;
}
