// sr-recomp: reads the game's fonts and menu pictures out of the RomFS for the launcher and the menu (game_art.h).
//
//   Font/Font.Nin_Ounce_NVN2.bfarc.zs                      zstd, then a SARC with scft/*.bfotf
//   UI/LayoutArchive/Boot.Product.110.Nin_Ounce_NVN2.blarc.zs  zstd, then a SARC with timg/__Combined.bntx
//
// A .bfotf is an 8-byte header and big-endian 32-bit words XORed with a key: first word ^ key = 0x7F9A0218, second word ^
// key = the font's size. A BNTX holds block-linear textures; the ones wanted here are BC4 (format 0x1D01) and ASTC
// (0x2D06 to 0x3A06, by block size).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fs.h"
#include "game_art.h"
#include "host.h"
#include "platform/platform.h"
#include "tex.h"

uint8_t *game_art_unzstd(const void *src, size_t n, size_t *out);   // game_art_zstd.c

static uint32_t le32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }
static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static void put_be16(uint8_t *p, int v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }

// The whole file, unpacked; free() it. A dump that is absent gives NULL here, not the fatal error of fs_init. The RomFS
// is a file (the NSP read in place) or a folder (the builder's game folder: romfs/romfs.bin, main.c portable_path).
static uint8_t *read_zs(const char *path, size_t *out) {
    const char *rf = rt_config()->romfs;
    if (!rf || !(plat_file_exists(rf) || plat_dir_exists(rf))) return NULL;
    fs_init();
    uint64_t off = 0, size = 0;
    int found = 0;
    for (uint64_t i = 0, n = romfs_file_count(); i < n && !found; i++) {
        const char *name = romfs_entry(i, &off, &size);
        found = name && !strcmp(name, path);
    }
    if (!found) {
        rt_log("game art: %s is not in the RomFS", path);
        return NULL;
    }
    uint64_t at = 0;
    PlatFile *f = romfs_raw_place(off, &at);
    uint8_t *raw = f ? malloc(size) : NULL;
    uint8_t *data = NULL;
    for (uint64_t got = 0; raw && got < size;) {
        uint64_t left = size - got;
        int64_t r = plat_file_pread(f, raw + got, left > (1u << 24) ? (1u << 24) : (uint32_t)left, at + got);
        if (r <= 0) {
            free(raw);
            raw = NULL;
            break;
        }
        got += (uint64_t)r;
    }
    if (raw) {
        data = game_art_unzstd(raw, (size_t)size, out);
        free(raw);
    }
    if (!data) rt_log("game art: cannot read %s", path);
    return data;
}

// The bytes of one member of a SARC; NULL when it is not there.
static const uint8_t *sarc_find(const uint8_t *a, size_t n, const char *member, size_t *size) {
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
        if (p < n && !strcmp((const char *)a + p, member) && t >= s && data + (size_t)t <= n) {
            *size = t - s;
            return a + data + s;
        }
    }
    return NULL;
}

uint8_t *game_art_font(const char *name, size_t *size) {
    size_t n = 0, wn = 0;
    uint8_t *a = read_zs("Font/Font.Nin_Ounce_NVN2.bfarc.zs", &n);
    if (!a) return NULL;
    char member[96];
    snprintf(member, sizeof member, "scft/%s.bfotf", name);
    const uint8_t *w = sarc_find(a, n, member, &wn);
    uint8_t *font = NULL;
    if (w && wn > 8) {
        uint32_t key = be32(w) ^ 0x7F9A0218u, len = be32(w + 4) ^ key;
        if (len <= wn - 8 && (font = malloc(wn))) {
            size_t words = (wn - 8) / 4;
            for (size_t i = 0; i < words; i++) {
                uint32_t v = be32(w + 8 + i * 4) ^ key;
                font[i * 4] = (uint8_t)(v >> 24), font[i * 4 + 1] = (uint8_t)(v >> 16), font[i * 4 + 2] = (uint8_t)(v >> 8);
                font[i * 4 + 3] = (uint8_t)v;
            }
            *size = len;
        }
    }
    free(a);
    if (!font) {
        rt_log("game art: no font %s", name);
        return NULL;
    }
    // the em square as the line: head.unitsPerEm (+18) into hhea.ascender (+4) and hhea.descender (+6)
    uint32_t tables = be16(font + 4);
    uint8_t *head = NULL, *hhea = NULL;
    for (uint32_t i = 0; i < tables && 12 + (i + 1) * 16 <= *size; i++) {
        const uint8_t *t = font + 12 + i * 16;
        uint32_t at = be32(t + 8);
        if (at + 36 > *size) continue;
        if (!memcmp(t, "head", 4)) head = font + at;
        if (!memcmp(t, "hhea", 4)) hhea = font + at;
    }
    if (head && hhea) {
        int em = be16(head + 18);
        put_be16(hhea + 4, em * 86 / 100);
        put_be16(hhea + 6, -(em * 14 / 100));
    }
    return font;
}

// ---- masks
enum { MASKS_MAX = 100 };
static GameMask masks[MASKS_MAX];
static int nmasks;

const GameMask *game_art_mask(const char *name) {
    for (int i = 0; i < nmasks; i++)
        if (!strcmp(masks[i].name, name)) return &masks[i];
    return NULL;
}

void game_art_masks_free(void) {
    for (int i = 0; i < nmasks; i++) free(masks[i].px);
    memset(masks, 0, sizeof masks);
    nmasks = 0;
}

// One BC4 block into 16 pixels at rows of `stride`, clipped to the picture.
static void bc4_block(const uint8_t *b, uint8_t *dst, int stride, int cols, int rows) {
    int r0 = b[0], r1 = b[1];
    uint8_t pal[8] = {(uint8_t)r0, (uint8_t)r1};
    if (r0 > r1) {
        for (int i = 1; i < 7; i++) pal[i + 1] = (uint8_t)(((7 - i) * r0 + i * r1) / 7);
    } else {
        for (int i = 1; i < 5; i++) pal[i + 1] = (uint8_t)(((5 - i) * r0 + i * r1) / 5);
        pal[6] = 0, pal[7] = 255;
    }
    uint64_t bits = le32(b + 2) | (uint64_t)le16(b + 6) << 32;
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++)
            if (x < cols && y < rows) dst[y * stride + x] = pal[bits >> (3 * (y * 4 + x)) & 7];
}

static int decode_mask(const uint8_t *b, size_t n, const uint8_t *brti, const char *name) {
    uint32_t fmt = le32(brti + 0x1c), w = le32(brti + 0x24), h = le32(brti + 0x28), lay = le32(brti + 0x34);
    uint32_t size = le32(brti + 0x50);
    uint64_t ptrs = le64(brti + 0x70);
    static const uint8_t kAstc[][2] = {{4, 4}, {5, 4}, {5, 5}, {6, 5}, {6, 6}, {8, 5}, {8, 6}, {8, 8}, {10, 5}, {10, 6}, {10, 8}, {10, 10}, {12, 10}, {12, 12}};
    uint32_t family = fmt >> 8;
    int astc = family >= 0x2D && family <= 0x3A;
    if ((fmt != 0x1D01 && !astc) || ptrs + 8 > n) {
        rt_log("game art: %s has format %#x", name, fmt);
        return 0;
    }
    uint64_t data = le64(b + ptrs);
    if (data + size > n || nmasks == MASKS_MAX) return 0;
    if (astc) {
        uint32_t aw = kAstc[family - 0x2D][0], ah = kAstc[family - 0x2D][1], cols = (w + aw - 1) / aw, rows = (h + ah - 1) / ah;
        uint8_t *blocks = malloc((size_t)cols * rows * 16), *rgba = malloc((size_t)w * h * 4);
        if (!blocks || !rgba) {
            free(blocks), free(rgba);
            return 0;
        }
        for (uint32_t y = 0; y < rows; y++)
            for (uint32_t x = 0; x < cols; x++) {
                uint64_t o = nvn_bl_offset(x * 16, y, 0, cols * 16, rows, lay, 0);
                if (o + 16 <= size) memcpy(blocks + ((size_t)y * cols + x) * 16, b + data + o, 16);
                else memset(blocks + ((size_t)y * cols + x) * 16, 0, 16);
            }
        astc_decode_ldr(blocks, w, h, aw, ah, (fmt & 0xff) == 6, rgba);
        free(blocks);
        masks[nmasks++] = (GameMask){name, (int)w, (int)h, 4, rgba};
        return 1;
    }
    uint8_t *px = malloc((size_t)w * h);
    if (!px) return 0;
    uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
    for (uint32_t y = 0; y < bh; y++)
        for (uint32_t x = 0; x < bw; x++) {
            uint64_t o = nvn_bl_offset(x * 8, y, 0, bw * 8, bh, lay, 0);
            if (o + 8 <= size) bc4_block(b + data + o, px + (size_t)y * 4 * w + x * 4, (int)w, (int)(w - x * 4), (int)(h - y * 4));
        }
    masks[nmasks++] = (GameMask){name, (int)w, (int)h, 1, px};
    return 1;
}

int game_art_masks_load(const char *const *names) {
    game_art_masks_free();
    size_t n = 0, bn = 0;
    const uint64_t t0 = plat_now_ms();
    uint8_t *a = read_zs("UI/LayoutArchive/Boot.Product.110.Nin_Ounce_NVN2.blarc.zs", &n);
    if (!a) return 0;
    const uint8_t *b = sarc_find(a, n, "timg/__Combined.bntx", &bn);
    int ok = b && bn > 0x40 && !memcmp(b, "BNTX", 4);
    if (ok) {
        uint32_t count = le32(b + 0x24);
        uint64_t table = le64(b + 0x28);
        for (int k = 0; names[k]; k++) {
            int found = 0;
            for (uint32_t i = 0; i < count && table + (i + 1) * 8ull <= bn && !found; i++) {
                uint64_t p = le64(b + table + i * 8ull);
                if (p + 0x78 > bn || memcmp(b + p, "BRTI", 4)) break;
                uint64_t np = le64(b + p + 0x60);
                if (np + 2 > bn) continue;
                size_t len = le16(b + np);
                if (len == strlen(names[k]) && np + 2 + len <= bn && !memcmp(b + np + 2, names[k], len))
                    found = decode_mask(b, bn, b + p, names[k]);
            }
            if (!found) {
                rt_log("game art: no mask %s", names[k]);
                ok = 0;
            }
        }
    }
    free(a);
    if (!ok) game_art_masks_free();
    else rt_log("game art: %d pictures in %llu ms", nmasks, (unsigned long long)(plat_now_ms() - t0));
    return ok;
}
