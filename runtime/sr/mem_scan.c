// sr-recomp diagnostics: where known 32-bit values live in the running game's memory (SR_MEMSCAN). For finding a
// structure of the game from the ids its parameter tables give: the places where several of them sit close together.
//
//   SR_MEMSCAN=<file>       text, one value per line: "<unsigned decimal> <tag>" (tag: a short word, free)
//   SR_MEMSCAN_AT_S=<n>     seconds after the start at which the scan runs (default 45); "a,b,c" scans at each
//   SR_MEMSCAN_OUT=<dir>    where the results go (default: next to the values file)
//   SR_MEMSCAN_GAP=<bytes>  hits at most this far apart are one group (default 256)
//   SR_MEMSCAN_PAD=<bytes>  bytes kept before and after each group in the .bin (default 64)
//   SR_MEMSCAN_STEP=1       every byte offset, not only the 4-byte aligned ones: packed data (a serialized save)
//
// Read-only, on a thread of its own, while the game runs: the committed, readable pages of the guest's heap, alias and
// module regions (VirtualQuery, so a page the game mapped but never touched is neither read nor committed by this),
// every 4-byte aligned word looked up in the set. Each scan writes memscan-<n>s.txt (a line per group: its address, its
// size, the hits and the commonest distance between them, then the hits) and memscan-<n>s.bin (the bytes of the groups
// of 2 to 4096 hits with 64 bytes around them, each after a 16-byte header: address and length), for a script to read
// the structures from. Nothing of this is on without SR_MEMSCAN.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/cfg.h"
#include "../core/host.h"
#include "../core/platform/platform.h"

#ifdef _WIN32
#include <windows.h>

typedef struct { uint32_t value; char tag[12]; } Want;
typedef struct { uint64_t at; uint32_t want; } Hit;

static Want *wants;
static uint32_t nwants, *slots, nslots;   // slots: open addressing, index + 1 into wants
static Hit *hits;
static size_t nhits, hits_cap;
static char out_dir[1024];
static unsigned scan_step = 4;
static uint64_t scan_pad = 64;

static uint32_t mix(uint32_t v) { return (v * 2654435761u) >> 7; }

static int load_wants(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[128];
    uint32_t cap = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long v;
        char tag[12] = "";
        if (sscanf(line, "%llu %11s", &v, tag) < 1 || v > 0xFFFFFFFFull) continue;
        if (nwants == cap) wants = realloc(wants, (cap = cap ? cap * 2 : 1024) * sizeof *wants);
        if (!wants) return 0;
        wants[nwants].value = (uint32_t)v;
        memcpy(wants[nwants++].tag, tag, sizeof tag);
    }
    fclose(f);
    for (nslots = 1024; nslots < nwants * 4; nslots *= 2) {}
    slots = calloc(nslots, sizeof *slots);
    if (!slots) return 0;
    for (uint32_t i = 0; i < nwants; i++) {
        uint32_t s = mix(wants[i].value) & (nslots - 1);
        while (slots[s] && wants[slots[s] - 1].value != wants[i].value) s = (s + 1) & (nslots - 1);
        if (!slots[s]) slots[s] = i + 1;   // a value listed twice keeps its first tag
    }
    return nwants != 0;
}

static void scan_range(uint64_t base, uint64_t size) {
    uint64_t a = base, end = base + size;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mi;
        if (!VirtualQuery((void *)(uintptr_t)a, &mi, sizeof mi)) break;
        uint64_t lo = (uint64_t)(uintptr_t)mi.BaseAddress, hi = lo + mi.RegionSize;
        if (hi > end) hi = end;
        if (lo < a) lo = a;
        DWORD p = mi.Protect & 0xFF;
        int readable = mi.State == MEM_COMMIT && !(mi.Protect & PAGE_GUARD) &&
                       (p == PAGE_READONLY || p == PAGE_READWRITE || p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE ||
                        p == PAGE_WRITECOPY || p == PAGE_EXECUTE_WRITECOPY);
        if (readable) {
            const uint8_t *w = (const uint8_t *)(uintptr_t)lo;
            size_t n = (size_t)(hi - lo) >= 4 ? (size_t)(hi - lo) - 3 : 0;
            for (size_t i = 0; i < n; i += scan_step) {
                uint32_t v;
                memcpy(&v, w + i, 4);
                if (v < 0x10000) continue;   // small numbers are everywhere; no id of the tables is one
                uint32_t s = mix(v) & (nslots - 1);
                while (slots[s] && wants[slots[s] - 1].value != v) s = (s + 1) & (nslots - 1);
                if (!slots[s]) continue;
                if (nhits == hits_cap) {
                    Hit *more = realloc(hits, (hits_cap = hits_cap ? hits_cap * 2 : 1 << 16) * sizeof *hits);
                    if (!more) return;
                    hits = more;
                }
                hits[nhits++] = (Hit){lo + (uint64_t)i, slots[s] - 1};
            }
        }
        a = hi > a ? hi : a + 0x1000;
    }
}

static void report(unsigned seconds, uint64_t gap) {
    char path[1200];
    snprintf(path, sizeof path, "%s/memscan-%us.txt", out_dir, seconds);
    FILE *t = fopen(path, "w");
    snprintf(path, sizeof path, "%s/memscan-%us.bin", out_dir, seconds);
    FILE *b = fopen(path, "wb");
    if (!t || !b) {
        rt_log("memscan: cannot write to %s", out_dir);
        if (t) fclose(t);
        if (b) fclose(b);
        return;
    }
    size_t groups = 0;
    for (size_t i = 0; i < nhits;) {
        size_t j = i + 1;
        while (j < nhits && hits[j].at - hits[j - 1].at <= gap) j++;
        uint64_t best = 0;
        size_t best_n = 0;
        for (size_t k = i + 1; k < j; k++) {   // the commonest distance between neighbours
            uint64_t d = hits[k].at - hits[k - 1].at;
            size_t n = 0;
            for (size_t m = i + 1; m < j; m++) n += hits[m].at - hits[m - 1].at == d;
            if (n > best_n) best_n = n, best = d;
            if (j - i > 512) break;   // a big group: the first distance is telling enough
        }
        fprintf(t, "group at %012llx size %llu hits %zu step %llu\n", (unsigned long long)hits[i].at,
                (unsigned long long)(hits[j - 1].at - hits[i].at + 4), j - i, (unsigned long long)best);
        for (size_t k = i; k < j && k < i + 64; k++)
            fprintf(t, "  +%-6llu %10u %s\n", (unsigned long long)(hits[k].at - hits[i].at), wants[hits[k].want].value,
                    wants[hits[k].want].tag);
        if (j - i >= 2 && j - i <= 4096) {
            uint64_t from = hits[i].at - scan_pad, len = hits[j - 1].at + 4 + scan_pad - from;
            MEMORY_BASIC_INFORMATION mi;   // the bytes before may belong to another, unreadable region
            if (VirtualQuery((void *)(uintptr_t)from, &mi, sizeof mi) && mi.State == MEM_COMMIT &&
                (uint64_t)(uintptr_t)mi.BaseAddress + mi.RegionSize >= from + len && !(mi.Protect & (PAGE_GUARD | PAGE_NOACCESS))) {
                uint64_t head[2] = {from, len};
                fwrite(head, 8, 2, b);
                fwrite((void *)(uintptr_t)from, 1, (size_t)len, b);
            }
        }
        groups++;
        i = j;
    }
    fclose(t);
    fclose(b);
    rt_log("memscan: %us: %zu hits in %zu groups (%s/memscan-%us.txt)", seconds, nhits, groups, out_dir, seconds);
}

static void scan_thread(void *arg) {
    (void)arg;
    const char *at = cfg_get("SR_MEMSCAN_AT_S");
    const char *g = cfg_get("SR_MEMSCAN_GAP"), *pad = cfg_get("SR_MEMSCAN_PAD"), *st = cfg_get("SR_MEMSCAN_STEP");
    if (pad && *pad) scan_pad = strtoull(pad, NULL, 10);
    if (st && st[0] == '1') scan_step = 1;
    uint64_t gap = g && *g ? strtoull(g, NULL, 10) : 256, start = plat_now_ns();
    char list[128];
    snprintf(list, sizeof list, "%s", at && *at ? at : "45");
    for (char *s = strtok(list, ","); s; s = strtok(NULL, ",")) {
        unsigned seconds = (unsigned)strtoul(s, NULL, 10);
        while (plat_now_ns() - start < (uint64_t)seconds * 1000000000ull) Sleep(100);
        const RtGuestLayout *L = mem_layout();
        nhits = 0;
        uint64_t t0 = plat_now_ns();
        scan_range(L->aslr.base, L->aslr.size);
        scan_range(L->heap.base, L->heap.size);
        scan_range(L->alias.base, L->alias.size);
        rt_log("memscan: %us: scanned in %.1f s", seconds, (double)(plat_now_ns() - t0) / 1e9);
        report(seconds, gap);
    }
}

void mem_scan_start(void) {
    const char *file = cfg_get("SR_MEMSCAN");
    if (!file || !*file) return;
    if (!load_wants(file)) {
        rt_log("memscan: no values in %s", file);
        return;
    }
    const char *out = cfg_get("SR_MEMSCAN_OUT");
    if (out && *out) {
        snprintf(out_dir, sizeof out_dir, "%s", out);
    } else {
        snprintf(out_dir, sizeof out_dir, "%s", file);
        char *cut = strrchr(out_dir, '/'), *cut2 = strrchr(out_dir, '\\');
        if (cut2 > cut) cut = cut2;
        if (cut) *cut = 0;
        else snprintf(out_dir, sizeof out_dir, ".");
    }
    rt_log("memscan: %u values from %s", nwants, file);
    plat_thread_spawn(scan_thread, NULL, "memscan", 0);
}
#else
void mem_scan_start(void) {}
#endif
