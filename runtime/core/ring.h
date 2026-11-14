// sr-recomp: the crash ring (ring.c). A ring of short records in logs/sr-ring.bin, a file mapped into memory, so what
// the runtime last did stays on disk when the process is killed or crashes (the pages belong to the file, not to the
// process). The next start turns an unfinished ring into logs/sr-ring-prev.txt, oldest record first.
//
// SR_RING (ring in sr-config.ini): 0 off; 1 (default) log lines, presents, queue submits, render passes, pipeline
// builds and DLSS calls; 2 also every draw, dispatch and recorder call (a few hundred thousand records a second: a
// second or two of history, and a measurable cost; for chasing a crash).
#pragma once
#include <stddef.h>

extern int ring_level;   // 0 until ring_open; read before formatting a record, so a ring that is off costs a load

void ring_open(void);                       // game runs, after log_open; turns an unfinished previous ring into text
void ring_close(void);                      // a clean exit: the next start leaves the ring as it is
void ring_put(const char *fmt, ...) __attribute__((format(printf, 1, 2)));   // one record, cut at 100 bytes
void ring_text(const char *p, size_t n);    // one record from a byte range (a log line), cut the same way

#define RING1(...) (ring_level >= 1 ? ring_put(__VA_ARGS__) : (void)0)
#define RING2(...) (ring_level >= 2 ? ring_put(__VA_ARGS__) : (void)0)

// FNV-1a over n bytes: a short name for a pipeline key or shader code in a record.
static inline unsigned long long ring_hash(const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    unsigned long long h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    return h;
}
