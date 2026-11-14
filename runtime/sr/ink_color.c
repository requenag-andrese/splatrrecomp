// sr-recomp: the player's and the enemies' ink colours (sr-config.ini ink_color and enemy_ink_color, SR_INK_COLOR and
// SR_ENEMY_INK_COLOR; "rrggbb" or "#rrggbb", unset = the game's own).
//
// The game keeps its ink colours in one parameter table of the RomFS (core/romfs.c INKTABLE, a BYML): an array of
// colour sets, each a dictionary with the teams Alpha, Bravo and Charlie, each with a TeamColor {R, G, B, A} of floats.
// The player paints with Alpha of the set chosen in the game's own options (the sets whose row name starts with
// "Hunt"); the enemies paint with Bravo. The brighter and darker tones are not stored: the game derives them from
// TeamColor. ink_table_patch writes the wanted colours over those floats in the unpacked table, in place: Alpha of
// every Hunt set (so the colour holds whichever set the save has chosen), Bravo of every set. core/romfs.c serves the
// result in the table's place (ink_overlay), so the game reads it at start like its own.
//
// BYML here: little-endian "YB", the header's three offsets (key table, string table, root), containers 0xC0 (array:
// count, a type byte per element, then a 32-bit value per element) and 0xC1 (dictionary: count, then per entry a 24-bit
// key index, a type byte and a 32-bit value), string tables 0xC2 (count, offsets from the table's start), a float as
// type 0xD2 with its bits in the value. A container's value is its offset in the file, and a writer may point two
// entries at one container: a float this file would change that something else reaches too stops the patch.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../core/cfg.h"
#include "../core/host.h"

int ink_color_parse(const char *s, float rgb[3]) {
    if (!s) return 0;
    if (*s == '#') s++;
    if (strlen(s) != 6 || strspn(s, "0123456789abcdefABCDEF") != 6) return 0;
    unsigned long v = strtoul(s, NULL, 16);
    for (int c = 0; c < 3; c++) rgb[c] = (float)((v >> (16 - 8 * c)) & 255) / 255.0f;
    return 1;
}

int ink_wanted(void) {
    float rgb[3];
    return ink_color_parse(cfg_get("SR_INK_COLOR"), rgb) || ink_color_parse(cfg_get("SR_ENEMY_INK_COLOR"), rgb);
}

typedef struct {
    uint8_t *d;
    size_t n;
    uint32_t keys, strs;   // the two string tables' offsets
} By;

static uint32_t u32(const By *b, size_t o) {
    return o + 4 <= b->n ? (uint32_t)b->d[o] | (uint32_t)b->d[o + 1] << 8 | (uint32_t)b->d[o + 2] << 16 | (uint32_t)b->d[o + 3] << 24
                         : 0;
}

static uint32_t count_of(const By *b, size_t node) { return u32(b, node) >> 8; }
static int type_of(const By *b, size_t node) { return node < b->n ? b->d[node] : 0; }

// String i of a string table (NUL-terminated inside the file), or NULL.
static const char *table_str(const By *b, uint32_t table, uint32_t i) {
    if (type_of(b, table) != 0xC2 || i >= count_of(b, table)) return NULL;
    size_t at = (size_t)table + u32(b, (size_t)table + 4 + 4 * (size_t)i);
    return at < b->n && memchr(b->d + at, 0, b->n - at) ? (const char *)b->d + at : NULL;
}

// The entry of a dictionary whose key is `key`: its type, and the offset of its 32-bit value (0: none).
static size_t dict_find(const By *b, size_t node, const char *key, int *type) {
    if (type_of(b, node) != 0xC1) return 0;
    uint32_t n = count_of(b, node);
    for (uint32_t i = 0; i < n; i++) {
        size_t e = node + 4 + 8 * (size_t)i;
        if (e + 8 > b->n) return 0;
        const char *name = table_str(b, b->keys, u32(b, e) & 0xFFFFFF);
        if (name && !strcmp(name, key)) {
            *type = b->d[e + 3];
            return e + 4;
        }
    }
    return 0;
}

enum { MAX_SLOTS = 256 };
typedef struct {
    size_t at;       // the float's offset
    float value;
    int wanted, reached;   // times the patch reaches it, times anything does (count_reached)
} Slot;
typedef struct {
    Slot s[MAX_SLOTS];
    int n, bad;
} Slots;

static void slot_add(Slots *t, size_t at, float value) {
    for (int i = 0; i < t->n; i++) {
        if (t->s[i].at != at) continue;
        if (t->s[i].value != value) t->bad = 1;   // one float asked to be two colours (the player's and the enemies')
        t->s[i].wanted++;
        return;
    }
    if (t->n == MAX_SLOTS) {
        t->bad = 1;
        return;
    }
    t->s[t->n++] = (Slot){at, value, 1, 0};
}

// <row>.<team>.TeamColor.{R,G,B} of one colour set.
static void want_team(const By *b, size_t row, const char *team, const float rgb[3], Slots *t) {
    int type = 0;
    size_t v = dict_find(b, row, team, &type);
    if (!v || type != 0xC1) return;
    v = dict_find(b, u32(b, v), "TeamColor", &type);
    if (!v || type != 0xC1) return;
    size_t colour = u32(b, v);
    static const char *const names[3] = {"R", "G", "B"};
    for (int c = 0; c < 3; c++) {
        size_t f = dict_find(b, colour, names[c], &type);
        if (f && type == 0xD2) slot_add(t, f, rgb[c]);
        else t->bad = 1;   // a colour without a float channel: not the table this was written for
    }
}

// Every way the file reaches each slot, by walking all of it.
static void count_reached(const By *b, size_t node, int depth, Slots *t) {
    int type = type_of(b, node);
    if (depth > 12 || (type != 0xC0 && type != 0xC1)) return;
    uint32_t n = count_of(b, node);
    size_t values = type == 0xC0 ? node + 4 + (((size_t)n + 3) & ~(size_t)3) : 0;
    for (uint32_t i = 0; i < n; i++) {
        size_t tp = type == 0xC0 ? node + 4 + i : node + 4 + 8 * (size_t)i + 3;
        size_t vp = type == 0xC0 ? values + 4 * (size_t)i : node + 4 + 8 * (size_t)i + 4;
        if (vp + 4 > b->n) return;
        int et = b->d[tp];
        if (et == 0xC0 || et == 0xC1) count_reached(b, u32(b, vp), depth + 1, t);
        else if (et == 0xD2)
            for (int k = 0; k < t->n; k++)
                if (t->s[k].at == vp) t->s[k].reached++;
    }
}

int ink_table_patch(uint8_t *raw, size_t n) {
    float player[3], enemy[3];
    int has_player = ink_color_parse(cfg_get("SR_INK_COLOR"), player);
    int has_enemy = ink_color_parse(cfg_get("SR_ENEMY_INK_COLOR"), enemy);
    By b = {raw, n, 0, 0};
    if (n < 16 || memcmp(raw, "YB", 2)) {
        rt_log("ink: the colour table is not a little-endian BYML; the ink keeps the game's colours");
        return 0;
    }
    b.keys = u32(&b, 4), b.strs = u32(&b, 8);
    size_t root = u32(&b, 12);
    if (type_of(&b, root) != 0xC0) {
        rt_log("ink: the colour table is not a list of sets; the ink keeps the game's colours");
        return 0;
    }
    Slots *t = calloc(1, sizeof *t);
    if (!t) return 0;
    uint32_t rows = count_of(&b, root);
    size_t values = root + 4 + (((size_t)rows + 3) & ~(size_t)3);
    int hunts = 0;
    for (uint32_t i = 0; i < rows && values + 4 * ((size_t)i + 1) <= n; i++) {
        if (raw[root + 4 + i] != 0xC1) continue;
        size_t row = u32(&b, values + 4 * (size_t)i);
        int type = 0;
        size_t id = dict_find(&b, row, "__RowId", &type);
        const char *name = id && type == 0xA0 ? table_str(&b, b.strs, u32(&b, id)) : NULL;
        const char *leaf = name && strrchr(name, '/') ? strrchr(name, '/') + 1 : name;
        int hunt = leaf && !strncmp(leaf, "Hunt", 4);
        hunts += hunt;
        if (has_player && hunt) want_team(&b, row, "Alpha", player, t);
        if (has_enemy) want_team(&b, row, "Bravo", enemy, t);
    }
    count_reached(&b, root, 0, t);
    int shared = 0;
    for (int k = 0; k < t->n; k++) shared |= t->s[k].reached != t->s[k].wanted;
    int set = 0;
    if (t->bad || shared || !t->n || (has_player && !hunts)) {
        rt_log("ink: the colour table is not laid out as expected (%d sets of the player's, %d values%s%s); the ink "
               "keeps the game's colours", hunts, t->n, shared ? ", some shared with other entries" : "",
               t->bad ? ", some not a colour" : "");
    } else {
        for (int k = 0; k < t->n; k++, set++) memcpy(raw + t->s[k].at, &t->s[k].value, 4);   // little-endian host
    }
    free(t);
    return set;
}

// raw[0..n) as a zstd frame the game's decompressor takes like any other: the magic, a header that states the content
// size (single segment, a 4-byte size), then raw blocks of up to 128 KiB (a 3-byte header each: the last-block bit,
// type 0 and the size). No compressor needed; the table is a few KiB.
void *ink_table_frame(const uint8_t *raw, size_t n, size_t *out_n) {
    enum { BLOCK = 1 << 17 };
    size_t blocks = n ? (n + BLOCK - 1) / BLOCK : 1;
    uint8_t *out = n <= 0xFFFFFFFFu ? malloc(9 + 3 * blocks + n) : NULL, *p = out;
    if (!out) return NULL;
    static const uint8_t magic[5] = {0x28, 0xB5, 0x2F, 0xFD, 0xA0};   // 0xA0: a 4-byte content size, single segment
    memcpy(p, magic, 5), p += 5;
    for (int k = 0; k < 4; k++) *p++ = (uint8_t)(n >> (8 * k));
    for (size_t at = 0, b = 0; b < blocks; b++) {
        size_t len = n - at < BLOCK ? n - at : BLOCK;
        uint32_t head = (uint32_t)(len << 3) | (b + 1 == blocks);
        *p++ = (uint8_t)head, *p++ = (uint8_t)(head >> 8), *p++ = (uint8_t)(head >> 16);
        if (len) memcpy(p, raw + at, len);
        p += len, at += len;
    }
    *out_n = (size_t)(p - out);
    return out;
}
