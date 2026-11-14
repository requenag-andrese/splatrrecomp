// sr-recomp: the crash ring (see ring.h). Idea after SwitchRecomp v190's vk_trace.txt, a 32 MB ring in a memory-mapped
// file (MZ Labs, https://github.com/SwitchRECOMP-Dev/SwitchRECOMP); the code is ours.
//
// Layout: a 4 KiB header, then RING_RECS records of 128 bytes. A writer takes a sequence number with one atomic add
// and owns record (seq % RING_RECS); it clears the record's seq, fills it and stores seq + 1 last, so a record a kill
// cut in half has seq 0 and is left out. Each record also carries the run id, so records of an earlier run that were
// not overwritten are left out too. The decoder keeps the records of the header's run whose seq matches their slot,
// sorted by seq.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "ring.h"
#include "platform/platform.h"

#define RING_MAGIC "SRRCRING"
#define RING_VERSION 1u
#define RING_HEAD 4096u
#define RING_RECS (1u << 18)   // 262,144 records: 32 MiB
#define RING_TEXT 100u
#define RING_BYTES (RING_HEAD + (size_t)RING_RECS * sizeof(RingRec))

enum { RING_RUNNING = 1, RING_CLEAN = 2 };

typedef struct {
    char magic[8];
    uint32_t version, recs;
    volatile uint32_t state;
    uint32_t run, pid, pad;
    uint64_t t0_ns;            // plat_now_ns at open: record times are relative to it
    volatile uint64_t seq;     // records written so far
} RingHeader;

typedef struct {
    volatile uint64_t seq;     // the record's sequence number + 1; 0 while it is written
    uint32_t t_us;             // microseconds since open (wraps after 71 minutes; the order is seq's)
    uint32_t run;
    uint32_t tid;
    uint16_t len;
    uint16_t pad;
    char text[RING_TEXT];
    uint8_t pad2[4];
} RingRec;
_Static_assert(sizeof(RingRec) == 128, "ring record size");

int ring_level;
static RingHeader *H;
static RingRec *R;

static int rec_cmp(const void *a, const void *b) {
    uint64_t x = (*(RingRec *const *)a)->seq, y = (*(RingRec *const *)b)->seq;
    return x < y ? -1 : x > y;
}

// The previous run's records, oldest first, as text next to the ring. Returns how many.
static size_t decode(const char *txt) {
    RingRec **list = malloc(sizeof *list * RING_RECS);
    if (!list) return 0;
    size_t n = 0;
    for (uint32_t i = 0; i < RING_RECS; i++) {
        RingRec *r = &R[i];
        if (r->seq && r->run == H->run && (r->seq - 1) % RING_RECS == i && r->len <= RING_TEXT) list[n++] = r;
    }
    qsort(list, n, sizeof *list, rec_cmp);
    FILE *f = n ? fopen(txt, "wb") : NULL;
    if (f) {
        fprintf(f, "sr-ring: the last %zu records of process %u (of %llu written), oldest first: seq, ms since start, "
                "thread, what\n", n, H->pid, (unsigned long long)H->seq);
        for (size_t k = 0; k < n; k++)
            fprintf(f, "%9llu %10.3f %6u %.*s\n", (unsigned long long)(list[k]->seq - 1), list[k]->t_us / 1000.0,
                    list[k]->tid, (int)list[k]->len, list[k]->text);
        if (fclose(f)) n = 0;
    } else {
        n = 0;
    }
    free(list);
    return n;
}

void ring_open(void) {
    const char *v = cfg_get("SR_RING");
    int level = v && *v ? atoi(v) : 1;
    if (level <= 0) return;
    char dir[PLAT_PATH_MAX * 3], path[PLAT_PATH_MAX * 3 + 32], txt[PLAT_PATH_MAX * 3 + 32];
    if (!plat_exe_path(dir, sizeof dir)) return;
    char *cut = plat_last_sep(dir), *fs = strrchr(dir, '/');
    if (fs > cut) cut = fs;
    if (!cut || (size_t)(cut - dir) + sizeof "/logs" > sizeof dir) return;
    strcpy(cut, "/logs");
    plat_file_mkdir_p(dir);
    snprintf(path, sizeof path, "%s/sr-ring.bin", dir);
    snprintf(txt, sizeof txt, "%s/sr-ring-prev.txt", dir);
    uint64_t old = 0;
    void *m = NULL;
    // A restart (gfx_present.c) starts this process before the old one has exited and let go of the file.
    for (int i = 0; i < 30 && !(m = plat_file_map_rw(path, RING_BYTES, &old)); i++) plat_sleep_ms(100);
    if (!m) {
        rt_log("ring: %s is in use by another process (or cannot be mapped); no crash ring this run", path);
        return;
    }
    H = m, R = (RingRec *)((char *)m + RING_HEAD);
    if (old == RING_BYTES && !memcmp(H->magic, RING_MAGIC, 8) && H->version == RING_VERSION &&
        H->recs == RING_RECS && H->state == RING_RUNNING) {
        size_t n = decode(txt);
        rt_log("ring: the previous run ended without a clean exit; its last %zu steps are in %s", n, txt);
    }
    memset(H, 0, sizeof *H);
    memcpy(H->magic, RING_MAGIC, 8);
    H->version = RING_VERSION, H->recs = RING_RECS, H->pid = plat_process_id();
    H->t0_ns = plat_now_ns();
    H->run = (uint32_t)(H->t0_ns ^ H->t0_ns >> 32) * 2654435761u ^ H->pid;
    if (!H->run) H->run = 1;
    __atomic_store_n(&H->state, RING_RUNNING, __ATOMIC_RELEASE);
    __atomic_store_n(&ring_level, level, __ATOMIC_RELEASE);
    rt_log("ring: level %d, %s (%u records); after a crash the next start writes %s", level, path, RING_RECS, txt);
}

void ring_close(void) {
    if (H) __atomic_store_n(&H->state, RING_CLEAN, __ATOMIC_RELEASE);
}

void ring_text(const char *p, size_t n) {
    if (!H) return;
    while (n && (p[n - 1] == '\n' || p[n - 1] == '\r')) n--;
    if (n > RING_TEXT) n = RING_TEXT;
    uint64_t s = __atomic_fetch_add(&H->seq, 1, __ATOMIC_RELAXED);
    RingRec *r = &R[s % RING_RECS];
    __atomic_store_n(&r->seq, 0, __ATOMIC_RELAXED);
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
    r->t_us = (uint32_t)((plat_now_ns() - H->t0_ns) / 1000u);
    r->run = H->run;
    r->tid = plat_thread_id();
    r->len = (uint16_t)n;
    memcpy(r->text, p, n);
    __atomic_store_n(&r->seq, s + 1, __ATOMIC_RELEASE);
}

void ring_put(const char *fmt, ...) {
    if (!H) return;
    char buf[RING_TEXT + 1];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (n > 0) ring_text(buf, n < (int)sizeof buf ? (size_t)n : RING_TEXT);
}
