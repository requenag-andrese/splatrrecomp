// sr-recomp: the ink colours changed while the game runs (ink_color, enemy_ink_color; the Ink tab of the menu).
//
// The game keeps its colour table in memory as it read it from the RomFS (the BYML bytes, unchanged), and reads the
// team colours from it again when it loads a place: the colours of the next raid or island are the table's. So a change
// is written into that copy, and the game shows it the next time it loads one. For the colours on screen to change at
// once too, the game's own thread then derives its tones from the table again (ink_live_game_frame) and the old tones
// are replaced by the new ones in the game's heap: the character, its ink and the rest hold their own copies.
//
// core/romfs.c hands this the game's own table, unpacked, at start (ink_live_table). The copy in guest memory is found
// by its first bytes (the header, key and string tables, which no colour touches) when a setting first changes; each
// change after that builds the table the settings ask for from the game's own (ink_table_patch on a copy) and writes
// the words that differ. A change back to the game's own colours writes the game's floats back.
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/cfg.h"
#include "../core/host.h"
#include "../core/platform/platform.h"

enum { SIGNATURE = 512, MAX_COPIES = 4 };

static uint8_t *g_table;       // the game's own table, unpacked
static size_t g_n;
static uint64_t g_copies[MAX_COPIES];
static int g_ncopies;
static volatile int g_reapply;   // the table was written: the game thread derives the colours from it again
static volatile int g_replace;   // the game derived new colours: the live thread replaces the old ones in its heap

// The game's own table (core/romfs.c, at start, before any patch). Kept for the live changes.
void ink_live_table(const uint8_t *raw, size_t n) {
    if (g_table || !raw || n < SIGNATURE) return;
    g_table = malloc(n);
    if (g_table) memcpy(g_table, raw, n), g_n = n;
}

// Colour floats differ between the copy in memory and the game's own table; everything else is the same.
static int same_but_floats(const uint8_t *mem) { return !memcmp(mem, g_table, SIGNATURE) && !memcmp(mem, "YB", 2); }

static void replace_copies(void);
#ifdef _WIN32
#include <windows.h>

static void scan(uint64_t base, uint64_t size) {
    uint64_t a = base, end = base + size;
    while (a < end && g_ncopies < MAX_COPIES) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        uint64_t lo = (uint64_t)(uintptr_t)mi.BaseAddress, hi = lo + mi.RegionSize;
        if (hi > end) hi = end;
        if (lo < a) lo = a;
        DWORD p = mi.Protect & 0xFF;
        if (mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) && (p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE) &&
            hi - lo >= g_n) {
            const uint8_t *m = (const uint8_t *)(uintptr_t)lo;
            for (uint64_t i = 0; i + g_n <= hi - lo && g_ncopies < MAX_COPIES; i += 4) {   // the game aligns its buffers
                const uint8_t *f = memchr(m + i, 'Y', (size_t)(hi - lo - g_n - i + 1));
                if (!f) break;
                i = (uint64_t)(f - m);
                if (i % 4 == 0 && same_but_floats(f)) g_copies[g_ncopies++] = lo + i;
                i &= ~(uint64_t)3;
            }
        }
        a = hi > a ? hi : a + 0x1000;
    }
}

static void find_copies(void) {
    const RtGuestLayout *L = mem_layout();
    uint64_t t0 = plat_now_ns();
    g_ncopies = 0;
    scan(L->heap.base, L->heap.size);
    scan(L->alias.base, L->alias.size);
    rt_log("ink: live: %d copies of the colour table in the game's memory (%.1f s)", g_ncopies,
           (double)(plat_now_ns() - t0) / 1e9);
}

// The table the settings ask for, written into every copy where it differs. 0 when no copy is there.
static int apply(void) {
    uint8_t *want = malloc(g_n);
    if (!want) return 0;
    memcpy(want, g_table, g_n);
    float rgb[3];
    if (ink_color_parse(cfg_get("SR_INK_COLOR"), rgb) || ink_color_parse(cfg_get("SR_ENEMY_INK_COLOR"), rgb))
        ink_table_patch(want, g_n);   // unset both: the game's own floats
    int written = 0;
    for (int pass = 0; pass < 2 && !written; pass++) {
        if (pass || !g_ncopies) find_copies();
        for (int k = 0; k < g_ncopies; k++) {
            uint8_t *mem = (uint8_t *)(uintptr_t)g_copies[k];
            if (!same_but_floats(mem)) continue;   // the game dropped or moved it: look again
            for (size_t i = 0; i + 4 <= g_n; i += 4)
                if (memcmp(mem + i, want + i, 4)) memcpy(mem + i, want + i, 4);
            written++;
        }
    }
    free(want);
    return written;
}

static void live_thread(void *arg) {
    (void)arg;
    char last[2][16] = {{0}, {0}};
    const char *keys[2] = {"SR_INK_COLOR", "SR_ENEMY_INK_COLOR"};
    for (int k = 0; k < 2; k++) snprintf(last[k], sizeof last[k], "%s", cfg_get(keys[k]) ? cfg_get(keys[k]) : "");
    unsigned seen = cfg_version();
    for (;; plat_sleep_ms(250)) {
        if (g_replace) replace_copies();
        if (cfg_version() == seen) continue;
        seen = cfg_version();
        int changed = 0;
        for (int k = 0; k < 2; k++) {
            const char *v = cfg_get(keys[k]) ? cfg_get(keys[k]) : "";
            if (strcmp(v, last[k])) changed = 1, snprintf(last[k], sizeof last[k], "%s", v);
        }
        if (!changed) continue;
        int n = apply();
        if (n) g_reapply = 1;
        rt_log(n ? "ink: live: colours written into the game's table (ink_color %s, enemy_ink_color %s); they show "
                   "the next time the game loads a place"
                 : "ink: live: the colour table is not in the game's memory (ink_color %s, enemy_ink_color %s); "
                   "the change shows after a restart",
               last[0][0] ? last[0] : "the game's", last[1][0] ? last[1] : "the game's");
    }
}

// main 1.1.0 (module 663c2e08...): the team colour manager and the game's own code that derives its tones. The pointer
// is the one main+0x2a82c50 (apply a colour set) reads; main+0x132e900 derives the same way after a set is chosen: the
// set's index (u16 at +0x104c) to an entry (main+0x132f2e0), the tones of that entry into +0x6c8 (main+0x132fd80), then
// an event with the tones (vtable main+0x4a38da8) sent through main+0x131fd20.
#define MAIN 0x7000000000ull
static const uint64_t MGR_PTR = MAIN + 0x4d6be78, FN_ENTRY = MAIN + 0x132f2e0, FN_DERIVE = MAIN + 0x132fd80,
                      FN_SEND = MAIN + 0x131fd20, EVENT_VTABLE = MAIN + 0x4a38da8;
enum { TONES_AT = 0x6c8, TONES_END = 0x1040 };   // what the game derives into (16-byte RGBA floats and more)
enum { PLAYER_COLOURS = 18 };                     // the player's 17 tones and the colour as given, from TONES_AT + 8
enum { MAX_COPIES_OF_ONE = 20000 };               // more copies than this: not only ours, left alone (the player's most: ~2000)

// The derived colours alone change nothing on screen: the character, its weapon, its ink and the rest each copied the
// tones they use when they were made. So the old tones are replaced by the new ones wherever they are in the game's
// heap, the exact three floats of each (a colour of the manager's block that changed). The replacement runs on the
// live thread (a few seconds of reading at most), not on the game's.
enum { MAX_PAIRS = 96 };
static uint32_t g_old[MAX_PAIRS][3], g_new[MAX_PAIRS][3];
static volatile int g_npairs;

static int readable(uint64_t a, size_t n) {
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi) || mi.State != MEM_COMMIT || (mi.Protect & PAGE_GUARD) ||
        (mi.Protect & 0xFF) == PAGE_NOACCESS)
        return 0;
    return a + n <= (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize;
}

static int is_unit(const float *f) {
    for (int k = 0; k < 4; k++)
        if (!(f[k] >= 0.0f && f[k] <= 1.0f)) return 0;
    return 1;
}

// Called by the game's own thread once a frame (core/ov_input.c, when the game reads the controller): after a table
// write, the game derives its colours from the table again, and the live thread replaces the old ones in its heap.
void ink_live_game_frame(Cpu *c) {
    if (!g_reapply || g_replace) return;
    g_reapply = 0;
    static int known = -1;
    if (known < 0) {   // this version's manager: its pointer, and the event's vtable pointing into the game's code
        uint64_t vt = 0;
        if (readable(EVENT_VTABLE, 8)) memcpy(&vt, (void *)(uintptr_t)EVENT_VTABLE, 8);
        known = readable(MGR_PTR, 8) && vt > MAIN && vt < MAIN + 0x4000000;
        if (!known) rt_log("ink: live: not the game version this knows; the change shows the next time a place loads");
    }
    if (!known) return;
    uint64_t mgr = *(volatile uint64_t *)(uintptr_t)MGR_PTR;
    if (!mgr || !readable(mgr + TONES_AT, TONES_END - TONES_AT)) return;
    static uint8_t before[TONES_END - TONES_AT];
    memcpy(before, (void *)(uintptr_t)(mgr + TONES_AT), sizeof before);
    uint16_t set = *(volatile uint16_t *)(uintptr_t)(mgr + 0x104c);
    uint64_t entry = rt_call(c, FN_ENTRY, 2, mgr, (uint64_t)set) & 0xffff;
    rt_call(c, FN_DERIVE, 4, mgr, mgr + TONES_AT, entry, (uint64_t)0);
    uint64_t sp = c->sp;
    c->sp = (sp - 0x40) & ~(uint64_t)15;   // the event on the guest stack, below the caller's frame
    uint64_t *ev = (uint64_t *)(uintptr_t)c->sp;
    ev[0] = EVENT_VTABLE, ev[1] = mgr + TONES_AT, ev[2] = 0;
    rt_call(c, FN_SEND, 1, (uint64_t)(uintptr_t)ev);
    c->sp = sp;
    const uint8_t *after = (const uint8_t *)(uintptr_t)(mgr + TONES_AT);
    int n = 0;
    // Only the player's block: its 17 tones and the colour as given, 8 bytes in. The rest of the manager holds other
    // values that change on their own; taken as colours, their three floats (0, 1 and the like) are everywhere in the
    // heap, and replacing them broke the game (the character kept its idle animation only, and could not swim).
    for (size_t o = 8; o + 16 <= 8 + PLAYER_COLOURS * 16 && n < MAX_PAIRS; o += 16) {
        float fo[4], fn[4];
        memcpy(fo, before + o, 16), memcpy(fn, after + o, 16);
        if (!is_unit(fo) || !is_unit(fn)) continue;
        float moved = 0;
        for (int k = 0; k < 3; k++) moved = fabsf(fo[k] - fn[k]) > moved ? fabsf(fo[k] - fn[k]) : moved;
        if (moved < 0.002f) continue;   // the same colour: nothing to replace
        if (fo[0] < 0.02f && fo[1] < 0.02f && fo[2] < 0.02f) continue;   // near black: too common to be ours
        int plain = 0;
        for (int k = 0; k < 3; k++) plain += fo[k] == 0.0f || fo[k] == 1.0f;
        if (plain >= 2) continue;   // two of 0 and 1: as common
        int dup = 0;
        for (int k = 0; k < n && !dup; k++) dup = !memcmp(g_old[k], fo, 12);
        if (dup) continue;
        memcpy(g_old[n], fo, 12), memcpy(g_new[n], fn, 12);
        n++;
    }
    g_npairs = n;
    if (n) g_replace = 1;
}

// One pass over a range: each copy of an enabled old colour counted in counts, and replaced when write is set.
static void replace_in(uint64_t base, uint64_t size, const uint32_t *bitmap, const int *on, int write, uint64_t *counts) {
    uint64_t a = base, end = base + size;
    int n = g_npairs;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        uint64_t lo = (uint64_t)(uintptr_t)mi.BaseAddress, hi = lo + mi.RegionSize;
        if (hi > end) hi = end;
        if (lo < a) lo = a;
        DWORD p = mi.Protect & 0xFF;
        if (mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) && (p == PAGE_READWRITE || p == PAGE_EXECUTE_READWRITE) &&
            hi - lo >= 16) {
            uint32_t *w = (uint32_t *)(uintptr_t)((lo + 3) & ~(uint64_t)3), *stop = (uint32_t *)(uintptr_t)(hi - 12);
            for (; w <= stop; w++) {
                uint32_t v = *w;
                if (!(bitmap[(v >> 5) & 0x7ff] & (1u << (v & 31)))) continue;
                for (int k = 0; k < n; k++)
                    if (on[k] && v == g_old[k][0] && w[1] == g_old[k][1] && w[2] == g_old[k][2]) {
                        if (write) w[0] = g_new[k][0], w[1] = g_new[k][1], w[2] = g_new[k][2];
                        counts[k]++;
                        break;
                    }
            }
        }
        a = hi > a ? hi : a + 0x1000;
    }
}

static void replace_copies(void) {
    static uint32_t bitmap[2048];   // the first words of the old colours, by their low 16 bits
    int on[MAX_PAIRS], n = g_npairs, kept = 0, left = 0;
    uint64_t counts[MAX_PAIRS] = {0}, written[MAX_PAIRS] = {0}, total = 0;
    const RtGuestLayout *L = mem_layout();
    uint64_t t0 = plat_now_ns();
    memset(bitmap, 0, sizeof bitmap);
    for (int k = 0; k < n; k++) on[k] = 1, bitmap[(g_old[k][0] >> 5) & 0x7ff] |= 1u << (g_old[k][0] & 31);
    // First count: a colour with too many copies is not only the player's (the same three floats mean something else
    // elsewhere), so it is left as it is rather than risk the game's other data.
    replace_in(L->heap.base, L->heap.size, bitmap, on, 0, counts);
    replace_in(L->alias.base, L->alias.size, bitmap, on, 0, counts);
    static uint64_t max_copies;
    if (!max_copies) {   // SR_INK_LIVE_MAX: a test knob
        const char *e = getenv("SR_INK_LIVE_MAX");
        max_copies = e && atoll(e) > 0 ? (uint64_t)atoll(e) : MAX_COPIES_OF_ONE;
    }
    for (int k = 0; k < n; k++) {   // each colour, its old and new floats and how often it is there
        float o[3], w[3];
        memcpy(o, g_old[k], 12), memcpy(w, g_new[k], 12);
        rt_log("ink: live:   %.4f %.4f %.4f -> %.4f %.4f %.4f: %llu copies", o[0], o[1], o[2], w[0], w[1], w[2],
               (unsigned long long)counts[k]);
    }
    memset(bitmap, 0, sizeof bitmap);
    for (int k = 0; k < n; k++) {
        on[k] = counts[k] <= max_copies;
        if (on[k]) kept++, bitmap[(g_old[k][0] >> 5) & 0x7ff] |= 1u << (g_old[k][0] & 31);
        else left++;
    }
    if (kept) {
        replace_in(L->heap.base, L->heap.size, bitmap, on, 1, written);
        replace_in(L->alias.base, L->alias.size, bitmap, on, 1, written);
    }
    for (int k = 0; k < n; k++) total += written[k];
    rt_log("ink: live: colours applied at once: %d colours of the game, %llu copies replaced, %d left alone (too many "
           "copies to be only the player's) (%.1f s)",
           kept, (unsigned long long)total, left, (double)(plat_now_ns() - t0) / 1e9);
    g_replace = 0;
}

void ink_live_start(void) {
    if (!g_table) {
        rt_log("ink: live: the game's colour table was not read at start; a change shows after a restart");
        return;
    }
    plat_thread_spawn(live_thread, NULL, "ink live", 0);
}
#else
void ink_live_start(void) {}
void ink_live_game_frame(Cpu *c) { (void)c; }
#endif
