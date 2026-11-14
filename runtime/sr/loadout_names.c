// sr-recomp: the names of the loadout tab, read from the player's dump (loadout_names.h).
//
// The text archive is a SARC of MSBT files. An MSBT ("MsgStdBn", little-endian, UTF-16) has sections of 16-byte
// headers (magic, size) padded to 16 bytes: LBL1 hashes the labels into buckets (count, offset), each label a length
// byte, its characters and the index of its text; TXT2 holds the texts (count, offsets), UTF-16 strings in which 0x0E
// starts a tag (group, type, size, then size bytes) and 0x0F closes one (group, type). The glossaries are keyed by the
// game's internal names: WeaponName "Roller_Heavy_F", GadgetName "Boomerang", RelicPower "Cmn_MultiJump", NpcName
// "Fuka", GadgetPartsType "<a gadget's short name>_<type>".
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "loadout_names.h"

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

static char *lo_dup_n(const char *s, size_t n) {
    char *d = malloc(n + 1);
    if (d) memcpy(d, s, n), d[n] = 0;
    return d;
}
static char *lo_dup(const char *s) { return s ? lo_dup_n(s, strlen(s)) : NULL; }

// ---- SARC: one member by name
static const uint8_t *sarc_member(const uint8_t *a, size_t n, const char *member, size_t *size) {
    if (n < 0x20 || memcmp(a, "SARC", 4)) return NULL;
    uint32_t hdr = le16(a + 4), data = le32(a + 0xc);
    if (hdr + 12 > n || memcmp(a + hdr, "SFAT", 4)) return NULL;
    uint32_t count = le16(a + hdr + 6), nodes = hdr + le16(a + hdr + 4);
    size_t names = nodes + (size_t)count * 16;
    if (names + 8 > n || memcmp(a + names, "SFNT", 4)) return NULL;
    size_t nbase = names + le16(a + names + 4);
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *e = a + nodes + i * 16;
        size_t p = nbase + (size_t)(le32(e + 4) & 0xffffff) * 4;
        uint32_t s = le32(e + 8), t = le32(e + 12);
        if (p < n && memchr(a + p, 0, n - p) && !strcmp((const char *)a + p, member) && t >= s && data + (size_t)t <= n) {
            *size = t - s;
            return a + data + s;
        }
    }
    return NULL;
}

// ---- MSBT: label -> text (UTF-8)
typedef struct { char *label, *text; } Msg;
typedef struct { Msg *m; int n; } Msgs;

static char *utf8_of(const uint8_t *s, size_t n) {   // n: bytes up to the section's end
    char *out = malloc(n * 2 + 1);   // UTF-16 to UTF-8: at most 3 bytes per 2
    size_t o = 0;
    if (!out) return NULL;
    for (size_t i = 0; i + 1 < n;) {
        uint32_t c = le16(s + i);
        if (!c) break;
        if (c == 0x0E) {   // a tag: its parameters are not text
            if (i + 8 > n) break;
            i += 8 + le16(s + i + 6);
            continue;
        }
        if (c == 0x0F) {
            i += 6;
            continue;
        }
        i += 2;
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
            uint32_t lo = le16(s + i);
            if (lo >= 0xDC00 && lo < 0xE000) c = 0x10000 + ((c - 0xD800) << 10) + (lo - 0xDC00), i += 2;
        }
        if (c < 0x80) out[o++] = (char)c;
        else if (c < 0x800) out[o++] = (char)(0xC0 | c >> 6), out[o++] = (char)(0x80 | (c & 0x3F));
        else if (c < 0x10000) out[o++] = (char)(0xE0 | c >> 12), out[o++] = (char)(0x80 | (c >> 6 & 0x3F)), out[o++] = (char)(0x80 | (c & 0x3F));
        else out[o++] = (char)(0xF0 | c >> 18), out[o++] = (char)(0x80 | (c >> 12 & 0x3F)), out[o++] = (char)(0x80 | (c >> 6 & 0x3F)), out[o++] = (char)(0x80 | (c & 0x3F));
    }
    out[o] = 0;
    return out;
}

static Msgs msbt(const uint8_t *m, size_t n) {
    Msgs r = {0, 0};
    if (!m || n < 0x20 || memcmp(m, "MsgStdBn", 8) || le16(m + 8) != 0xFEFF) return r;
    const uint8_t *lbl = NULL, *txt = NULL;
    size_t lbl_n = 0, txt_n = 0;
    for (size_t at = 0x20, k = 0, sections = le16(m + 0xe); k < sections && at + 16 <= n; k++) {
        size_t size = le32(m + at + 4), body = at + 16;
        if (body + size > n) break;
        if (!memcmp(m + at, "LBL1", 4)) lbl = m + body, lbl_n = size;
        if (!memcmp(m + at, "TXT2", 4)) txt = m + body, txt_n = size;
        at = body + size + (-size & 15);
    }
    if (!lbl || !txt || lbl_n < 4 || txt_n < 4) return r;
    uint32_t texts = le32(txt);
    if (4 + (size_t)texts * 4 > txt_n) return r;
    uint32_t buckets = le32(lbl), total = 0;
    if (4 + (size_t)buckets * 8 > lbl_n) return r;
    for (uint32_t b = 0; b < buckets; b++) total += le32(lbl + 4 + b * 8);
    r.m = calloc(total ? total : 1, sizeof *r.m);
    if (!r.m) return r;
    for (uint32_t b = 0; b < buckets; b++) {
        size_t q = le32(lbl + 8 + b * 8);
        for (uint32_t c = le32(lbl + 4 + b * 8); c && q < lbl_n; c--) {
            size_t len = lbl[q];
            if (q + 1 + len + 4 > lbl_n) break;
            uint32_t idx = le32(lbl + q + 1 + len);
            if (idx < texts) {
                size_t from = le32(txt + 4 + idx * 4);
                if (from < txt_n) {
                    r.m[r.n].label = lo_dup_n((const char *)lbl + q + 1, len);
                    r.m[r.n].text = utf8_of(txt + from, txt_n - from);
                    if (r.m[r.n].label && r.m[r.n].text) r.n++;
                }
            }
            q += 1 + len + 4;
        }
    }
    return r;
}

static Msgs glossary(const uint8_t *sarc, size_t n, const char *member) {
    size_t mn = 0;
    const uint8_t *m = sarc_member(sarc, n, member, &mn);
    return msbt(m, mn);
}

static const char *text_of(const Msgs *g, const char *label) {
    for (int i = 0; i < g->n; i++)
        if (!strcmp(g->m[i].label, label)) return g->m[i].text;
    return NULL;
}

static void msgs_free(Msgs *g) {
    for (int i = 0; i < g->n; i++) free(g->m[i].label), free(g->m[i].text);
    free(g->m);
    g->m = NULL, g->n = 0;
}

// ---- weapons: "<Category>[_<kind>]_<A..F>", the categories in the game's own order
static const char *const kCategories[] = {"Shooter", "Roller", "Charger", "Blaster", "Brush", "Slosher",
                                          "Spinner", "Maneuver", "Shelter", "Saber", "Stringer"};
enum { N_CATEGORIES = sizeof kCategories / sizeof *kCategories };

static int weapon_category(const char *id) {
    size_t len = strlen(id);
    if (len < 3 || id[len - 2] != '_' || id[len - 1] < 'A' || id[len - 1] > 'F') return -1;
    for (size_t i = 0; i < len - 2; i++)
        if (!(id[i] == '_' || (id[i] >= 'A' && id[i] <= 'Z') || (id[i] >= 'a' && id[i] <= 'z'))) return -1;
    size_t cat_len = strcspn(id, "_");
    for (int c = 0; c < N_CATEGORIES; c++)
        if (strlen(kCategories[c]) == cat_len && !strncmp(id, kCategories[c], cat_len)) return c;
    return -1;
}

typedef struct { const char *id, *name; int key; } Sortable;

static int by_key_then_id(const void *a, const void *b) {
    const Sortable *x = a, *y = b;
    if (x->key != y->key) return x->key - y->key;
    return strcmp(x->id, y->id);
}

static int by_key_then_name(const void *a, const void *b) {
    const Sortable *x = a, *y = b;
    if (x->key != y->key) return x->key - y->key;
    return strcmp(x->name, y->name);
}

static int power_group(const char *id) {
    static const char *const groups[] = {"Cmn_", "Agi_", "Str_", "Tec_", "Wpn_"};
    for (int i = 0; i < 5; i++)
        if (!strncmp(id, groups[i], 4)) return i;
    return 5;
}

// ---- gadget parts: "<short>_<NN>_<type>_<LL>"
typedef struct { char *type_id; int number, level; const char *file; } PartFile;

// Every character of `small` is in `big`, in order.
static int in_order(const char *small, const char *big) {
    while (*small && *big) small += *small == *big++;
    return !*small;
}

// The text of a part type's label; where the game spells the label otherwise than the files ("Foward"), the longest
// label of the same gadget that no part type has as its own and whose letters are in the type's in order.
static const char *part_text(const Msgs *g, const char *type_id, const PartFile *pf, int np) {
    const char *t = text_of(g, type_id);
    if (t) return t;
    size_t sl = strcspn(type_id, "_") + 1;
    int best = -1;
    for (int i = 0; i < g->n; i++) {
        const char *l = g->m[i].label;
        if (strncmp(l, type_id, sl) || !in_order(l + sl, type_id + sl)) continue;
        int owned = 0;
        for (int k = 0; k < np && !owned; k++) owned = !strcmp(pf[k].type_id, l);
        if (!owned && (best < 0 || strlen(l) > strlen(g->m[best].label))) best = i;
    }
    return best >= 0 ? g->m[best].text : NULL;
}

static int parse_part(const char *file, PartFile *out) {
    const char *a = strchr(file, '_');
    if (!a || a[1] < '0' || a[1] > '9') return 0;
    char *end;
    long number = strtol(a + 1, &end, 10);
    const char *last = strrchr(file, '_');
    if (*end != '_' || last <= end) return 0;
    const char *type = end + 1;   // after the number's '_'
    long level = strtol(last + 1, &end, 10);
    if (*end || last[1] < '0' || last[1] > '9' || last <= type) return 0;
    size_t short_len = (size_t)(a - file), type_len = (size_t)(last - type);
    out->type_id = malloc(short_len + 1 + type_len + 1);
    if (!out->type_id) return 0;
    memcpy(out->type_id, file, short_len);
    out->type_id[short_len] = '_';
    memcpy(out->type_id + short_len + 1, type, type_len);
    out->type_id[short_len + 1 + type_len] = 0;
    out->number = (int)number, out->level = (int)level, out->file = file;
    return 1;
}

static int by_part(const void *a, const void *b) {
    const PartFile *x = a, *y = b;
    int c = strcmp(x->type_id, y->type_id);
    if (c) return c;
    return x->level - y->level;
}

static int by_type_number(const void *a, const void *b) {
    const LoPartType *x = a, *y = b;
    int nx = 0, ny = 0;   // the number in the first file's name
    const char *px = strchr(x->parts[0], '_'), *py = strchr(y->parts[0], '_');
    if (px) nx = atoi(px + 1);
    if (py) ny = atoi(py + 1);
    return nx != ny ? nx - ny : strcmp(x->id, y->id);
}

static int by_gadget_row(const void *a, const void *b) {
    const LoGadgetRow *x = *(const LoGadgetRow *const *)a, *y = *(const LoGadgetRow *const *)b;
    if (x->tank != y->tank) return x->tank - y->tank;
    return x->order != y->order ? x->order - y->order : strcmp(x->id, y->id);
}

LoNames *lo_names_build(const uint8_t *sarc, size_t n, const char *const *parts, int n_parts, const LoGadgetRow *gadgets,
                        int n_gadgets) {
    Msgs weapons = glossary(sarc, n, "Glossary/WeaponName.msbt");
    if (!weapons.n) return NULL;
    Msgs cats = glossary(sarc, n, "Glossary/WeaponCategory.msbt"), gnames = glossary(sarc, n, "Glossary/GadgetName.msbt");
    Msgs powers = glossary(sarc, n, "Glossary/RelicPower.msbt"), npcs = glossary(sarc, n, "Glossary/NpcName.msbt");
    Msgs system = glossary(sarc, n, "Glossary/SystemName.msbt"), ptypes = glossary(sarc, n, "Glossary/GadgetPartsType.msbt");
    Msgs pdescs = glossary(sarc, n, "CommonMsg/Gadget/GadgetPartsTypeExp.msbt");
    LoNames *r = calloc(1, sizeof *r);
    Sortable *s = calloc((size_t)(weapons.n + powers.n + 1), sizeof *s);
    if (!r || !s) goto done;

    // weapons, by category, kind and letter
    int k = 0;
    for (int i = 0; i < weapons.n; i++) {
        int c = weapon_category(weapons.m[i].label);
        if (c >= 0) s[k++] = (Sortable){weapons.m[i].label, weapons.m[i].text, c};
    }
    qsort(s, (size_t)k, sizeof *s, by_key_then_id);
    r->weapons = calloc((size_t)k + 1, sizeof *r->weapons);
    r->weapon_cat = calloc((size_t)k + 1, sizeof *r->weapon_cat);
    r->cats = calloc(N_CATEGORIES, sizeof *r->cats);
    int cat_index[N_CATEGORIES];
    for (int c = 0; c < N_CATEGORIES; c++) cat_index[c] = -1;
    for (int i = 0; r->weapons && r->weapon_cat && r->cats && i < k; i++) {
        int c = s[i].key;
        if (cat_index[c] < 0) {
            const char *cn = text_of(&cats, kCategories[c]);
            cat_index[c] = r->n_cats;
            r->cats[r->n_cats++] = (LoName){lo_dup(kCategories[c]), lo_dup(cn ? cn : kCategories[c])};
        }
        r->weapons[r->n_weapons] = (LoName){lo_dup(s[i].id), lo_dup(s[i].name)};
        r->weapon_cat[r->n_weapons++] = cat_index[c];
    }

    // relic powers by group (common, then each tank's) and name, then the weapons' own
    k = 0;
    for (int i = 0; i < powers.n; i++)
        if (power_group(powers.m[i].label) < 5) s[k++] = (Sortable){powers.m[i].label, powers.m[i].text, power_group(powers.m[i].label)};
    qsort(s, (size_t)k, sizeof *s, by_key_then_name);
    r->powers = calloc((size_t)k + 1, sizeof *r->powers);
    for (int i = 0; r->powers && i < k; i++) {
        r->powers[r->n_powers++] = (LoName){lo_dup(s[i].id), lo_dup(s[i].name)};
        if (s[i].key < 4) r->n_relic_powers = r->n_powers;
    }

    // the tanks and the Bot Buddies
    static const char *const tank_ids[4] = {"Default", "Speed", "Power", "Tactical"};
    static const char *const tank_labels[4] = {NULL, "PlayerStyle_Agility_00", "PlayerStyle_Strength_00", "PlayerStyle_Technique_00"};
    for (int t = 0; t < 4; t++) r->tanks[t] = (LoName){tank_ids[t], tank_labels[t] ? lo_dup(text_of(&system, tank_labels[t])) : NULL};
    static const char *const buddy_ids[3] = {"Shiver", "Frye", "BigMan"}, *const buddy_labels[3] = {"Fuka", "Utsuho", "Mantaro"};
    for (int b = 0; b < 3; b++) {
        const char *name = text_of(&npcs, buddy_labels[b]);
        r->buddies[b] = (LoName){buddy_ids[b], lo_dup(name && *name ? name : buddy_ids[b])};
    }

    // gadgets with their part types
    {
        const LoGadgetRow **rows = calloc((size_t)n_gadgets + 1, sizeof *rows);
        PartFile *pf = calloc((size_t)n_parts + 1, sizeof *pf);
        int np = 0, ng = 0;
        for (int i = 0; pf && i < n_parts; i++) np += parse_part(parts[i], &pf[np]);
        if (pf) qsort(pf, (size_t)np, sizeof *pf, by_part);
        for (int i = 0; rows && i < n_gadgets; i++)
            if (gadgets[i].tank < 3 && text_of(&gnames, gadgets[i].id)) rows[ng++] = &gadgets[i];
        if (rows) qsort(rows, (size_t)ng, sizeof *rows, by_gadget_row);
        r->gadgets = calloc((size_t)ng + 1, sizeof *r->gadgets);
        for (int g = 0; r->gadgets && pf && g < ng; g++) {
            LoGadget *G = &r->gadgets[r->n_gadgets++];
            G->id = lo_dup(rows[g]->id);
            G->name = lo_dup(text_of(&gnames, rows[g]->id));
            size_t sl = strlen(rows[g]->short_name);
            G->types = calloc((size_t)np + 1, sizeof *G->types);
            for (int i = 0; G->types && i < np;) {
                int j = i;
                while (j < np && !strcmp(pf[j].type_id, pf[i].type_id)) j++;
                if (!strncmp(pf[i].type_id, rows[g]->short_name, sl) && pf[i].type_id[sl] == '_') {
                    LoPartType *T = &G->types[G->n_types++];
                    const char *tn = part_text(&ptypes, pf[i].type_id, pf, np), *td = part_text(&pdescs, pf[i].type_id, pf, np);
                    T->id = lo_dup(pf[i].type_id);
                    T->name = lo_dup(tn ? tn : pf[i].type_id);
                    T->desc = lo_dup(td);
                    T->parts = calloc((size_t)(j - i), sizeof *T->parts);
                    for (int q = i; T->parts && q < j; q++) T->parts[T->n++] = lo_dup(pf[q].file);
                    if (!T->parts) G->n_types--;
                }
                i = j;
            }
            if (G->types) qsort(G->types, (size_t)G->n_types, sizeof *G->types, by_type_number);
        }
        for (int i = 0; pf && i < np; i++) free(pf[i].type_id);
        free(pf);
        free(rows);
    }

done:
    free(s);
    msgs_free(&weapons), msgs_free(&cats), msgs_free(&gnames), msgs_free(&powers), msgs_free(&npcs);
    msgs_free(&system), msgs_free(&ptypes), msgs_free(&pdescs);
    return r;
}

void lo_names_icons(LoNames *L, const char *const *icons, int n) {
    char *used = calloc((size_t)n + 1, 1);
    for (int pass = 0; L && used && pass < 2; pass++)   // first the exact names, then the near ones from what is left
        for (int g = 0; g < L->n_gadgets; g++)
            for (int t = 0; t < L->gadgets[g].n_types; t++) {
                LoPartType *T = &L->gadgets[g].types[t];
                if (pass == 0) T->icon = NULL;
                if (T->icon) continue;
                size_t sl = strcspn(T->id, "_") + 1;
                int best = -1;
                for (int i = 0; i < n; i++) {
                    int fits = pass == 0 ? !strcmp(icons[i], T->id)
                                         : !used[i] && !strncmp(icons[i], T->id, sl) && in_order(icons[i] + sl, T->id + sl);
                    if (fits && (best < 0 || strlen(icons[i]) > strlen(icons[best]))) best = i;
                }
                if (best >= 0) T->icon = icons[best], used[best] = 1;
            }
    free(used);
}

const char *lo_names_mals(const char *code) {
    static const struct { const char *code, *mals; } map[] = {
        {"en-US", "USen"}, {"en-GB", "EUen"}, {"ja", "JPja"}, {"fr", "EUfr"}, {"fr-CA", "USfr"}, {"de", "EUde"},
        {"it", "EUit"}, {"es", "EUes"}, {"es-419", "USes"}, {"nl", "EUnl"}, {"zh-Hans", "CNzh"}, {"zh-CN", "CNzh"},
        {"zh-Hant", "TWzh"}, {"zh-TW", "TWzh"}, {"ko", "KRko"}, {"pt-BR", "USpt"}, {"pt", "USpt"}};
    for (size_t i = 0; code && i < sizeof map / sizeof *map; i++)
        if (!strcmp(code, map[i].code)) return map[i].mals;
    return "USen";
}

#ifndef LO_NAMES_NO_GAME
#include "../core/fs.h"
#include "../core/host.h"
#include "byml.h"

uint8_t *game_art_read(const char *path, size_t *out);   // core/game_art.c: a RomFS file, unpacked
const char *ov_language(void);                         // core/ov_system.c: the game's language

uint32_t loadout_hash(const char *s);   // loadout.c

// The RomFS path that starts with prefix and ends with suffix, or NULL.
static const char *romfs_find(const char *prefix, const char *suffix) {
    size_t pl = strlen(prefix), sl = strlen(suffix);
    uint64_t off, size;
    for (uint64_t i = 0, n = romfs_file_count(); i < n; i++) {
        const char *p = romfs_entry(i, &off, &size);
        size_t len = p ? strlen(p) : 0;
        if (len >= pl + sl && !strncmp(p, prefix, pl) && !strcmp(p + len - sl, suffix)) return p;
    }
    return NULL;
}

static LoGadgetRow *gadget_rows(int *count) {
    *count = 0;
    const char *path = romfs_find("RSDB/CoopGadgetInfo.", ".byml.zs");
    size_t n = 0;
    uint8_t *raw = path ? game_art_read(path, &n) : NULL;
    Byml *b = raw ? byml_parse(raw, n) : NULL;
    BymlNode *root = b ? byml_root(b) : NULL;
    LoGadgetRow *rows = root && root->type == BYML_ARRAY ? calloc(root->n + 1, sizeof *rows) : NULL;
    uint32_t tanks[3] = {loadout_hash("Agility"), loadout_hash("Strength"), loadout_hash("Technique")};
    for (uint32_t i = 0; rows && i < root->n; i++) {
        BymlNode *id = byml_get(root->items[i], "__RowId"), *sn = byml_get(root->items[i], "ShortName");
        BymlNode *style = byml_get(root->items[i], "Style"), *order = byml_get(root->items[i], "UiOrder");
        if (!id || !sn || id->type != BYML_STRING || sn->type != BYML_STRING) continue;
        const char *name = strncmp(id->str, "CoopGadgetInfo_", 15) ? id->str : id->str + 15;
        int tank = 3;
        for (int t = 0; style && t < 3; t++)
            if (style->raw == tanks[t]) tank = t;
        rows[(*count)++] = (LoGadgetRow){lo_dup(name), lo_dup(sn->str), tank, order ? (int)order->raw : 0};
    }
    if (b) byml_free(b);
    free(raw);
    return rows;
}

const LoNames *lo_names(void) {
    static int tried;
    static LoNames *names;
    if (tried) return names;
    tried = 1;
    const char *rf = rt_config()->romfs;
    if (!rf || !*rf) return NULL;
    fs_init();
    char prefix[32];
    const char *mals = lo_names_mals(ov_language());
    snprintf(prefix, sizeof prefix, "Mals/%s.", mals);
    const char *path = romfs_find(prefix, ".sarc.zs");
    if (!path) path = romfs_find("Mals/USen.", ".sarc.zs");
    size_t n = 0;
    uint8_t *sarc = path ? game_art_read(path, &n) : NULL;
    int n_parts = 0, n_gadgets = 0;
    const char **parts = sarc ? calloc(4096, sizeof *parts) : NULL;
    static const char kPartDir[] = "Gyml/GadgetParts/", kPartExt[] = ".spl__GadgetPartsInfo.bgyml";
    uint64_t off, size;
    for (uint64_t i = 0, count = romfs_file_count(); parts && i < count && n_parts < 4096; i++) {
        const char *p = romfs_entry(i, &off, &size);
        size_t len = p ? strlen(p) : 0;
        if (len > sizeof kPartDir + sizeof kPartExt && !strncmp(p, kPartDir, sizeof kPartDir - 1) &&
            !strcmp(p + len - (sizeof kPartExt - 1), kPartExt))
            parts[n_parts++] = lo_dup_n(p + sizeof kPartDir - 1, len - (sizeof kPartDir - 1) - (sizeof kPartExt - 1));
    }
    LoGadgetRow *rows = sarc ? gadget_rows(&n_gadgets) : NULL;
    names = sarc ? lo_names_build(sarc, n, parts, n_parts, rows, n_gadgets) : NULL;
    static const char kIconDir[] = "UI/Tex/GadgetParts/", kIconExt[] = ".bntx.zs";
    const char **icons = names ? calloc(1024, sizeof *icons) : NULL;
    int n_icons = 0;
    for (uint64_t i = 0, count = romfs_file_count(); icons && i < count && n_icons < 1024; i++) {
        const char *p = romfs_entry(i, &off, &size);
        size_t len = p ? strlen(p) : 0;
        if (len > sizeof kIconDir + sizeof kIconExt && !strncmp(p, kIconDir, sizeof kIconDir - 1) &&
            !strcmp(p + len - (sizeof kIconExt - 1), kIconExt))
            icons[n_icons++] = lo_dup_n(p + sizeof kIconDir - 1, len - (sizeof kIconDir - 1) - (sizeof kIconExt - 1));
    }
    if (names) lo_names_icons(names, icons, n_icons);   // the names stay: the types point at them
    rt_log("loadout: names from %s: %d weapons, %d gadgets, %d powers, %d part files", path ? path : "(no text archive)",
           names ? names->n_weapons : 0, names ? names->n_gadgets : 0, names ? names->n_powers : 0, n_parts);
    free(sarc);   // the lists hold copies; the part names and gadget rows stay with them
    return names;
}
#endif
