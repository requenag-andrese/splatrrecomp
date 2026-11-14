// NVN2 (Switch 2) shader control sections, seen through the NVN1 layout totk-pc-recomp's back end reads.
//
// An NVN2 control section is short (0xd8 bytes in a BNSH) and has its own layout (nx2android PR #88 shader_split.py,
// MIT): magic 34127698 and version 0xc9 at +0, +0x38 the instructions' length (a 0x80-byte SPH first in the graphics
// stages), +0x40..+0x44 the c1 segment (+0x44 is also the code area's length), +0x56 the NVN stage, a compute shader's
// thread group at +0x58 and its shared memory at +0x64, its c[0] layout at +0xa9 and +0xb1..+0xbb. The back end
// (vk_exec_draw.c, vk_pipeline.c, gfx_shader.c) reads NVN1 fields of a 0x880-byte section: +0x700 c1 offset, +0x704
// code length, +0x714 stage, +0x718 early-Z, +0x748 thread group, +0x754 shared memory. A program's NVN2 sections are
// replaced by a host copy of 0x880 bytes holding the NVN2 bytes at +0 (so gfx_shader.c still sees an NVN2 section and
// its compute layout) and those NVN1 fields filled from them. Copies are kept for the life of the process, one per
// distinct (address, content).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../core/host.h"
#include "../core/platform/platform.h"

#define NVN1_SIZE 0x880
#define NVN2_READ 0xc0   // every NVN2 field used lies below +0xbc

int sr_nvn2_control(const uint8_t *c) {
    static const uint8_t magic[8] = {0x34, 0x12, 0x76, 0x98, 0xc9, 0, 0, 0};
    return c && !memcmp(c, magic, sizeof magic);
}

typedef struct Ent { struct Ent *next; const uint8_t *guest; uint8_t *copy; } Ent;
#define BUCKETS 1024
static Ent *tab[BUCKETS];
static PlatMutex lock = PLAT_MUTEX_INIT;
static uint64_t copies;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static void wr32(uint8_t *p, uint32_t v) { memcpy(p, &v, 4); }

static uint8_t *convert(const uint8_t *g) {
    uint8_t *o = calloc(1, NVN1_SIZE);
    if (!o) rt_fatal("nvn2: out of memory for a control section");
    memcpy(o, g, NVN2_READ);
    uint32_t instr = rd32(g + 0x38), c1a = rd32(g + 0x40), c1e = rd32(g + 0x44);
    uint32_t total = c1e > instr ? c1e : instr;
    // NVN1's c1 is code[+0x700 .. +0x704]; NVN2's is code[c1a .. c1e], with c1e the code area's end.
    uint32_t c1_off = c1e > c1a && c1e == total ? c1a : total;
    wr32(o + 0x700, c1_off);
    wr32(o + 0x704, total);
    wr32(o + 0x714, g[0x56]);
    if (g[0x56] == 5) {   // compute: thread group and shared memory (rounded up to 16 bytes, as shader_split cs_layout)
        memcpy(o + 0x748, g + 0x58, 12);
        wr32(o + 0x754, (rd32(g + 0x64) + 15) / 16 * 16);
    }
    if (c1e > c1a && c1e != total) {
        static int seen;
        if (!__atomic_exchange_n(&seen, 1, __ATOMIC_RELAXED))
            rt_log("nvn2: a control section's c1 segment (%#x..%#x) does not end the code (%#x): c1 not passed", c1a, c1e,
                   total);
    }
    return o;
}

// The control section the back end gets for a program stage: the guest's own when it is NVN1, else the host copy.
const void *sr_nvn2_control_for(const void *guest) {
    const uint8_t *g = guest;
    if (!sr_nvn2_control(g)) return guest;
    uint32_t b = (uint32_t)(((uintptr_t)g >> 4) * 0x9E3779B97F4A7C15ull >> 54) % BUCKETS;
    plat_mutex_lock(&lock);
    for (Ent *e = tab[b]; e; e = e->next)
        if (e->guest == g && !memcmp(e->copy, g, NVN2_READ)) {
            plat_mutex_unlock(&lock);
            return e->copy;
        }
    Ent *e = malloc(sizeof *e);
    if (!e) rt_fatal("nvn2: out of memory for a control section");
    *e = (Ent){tab[b], g, convert(g)};
    tab[b] = e;
    uint64_t n = ++copies;
    plat_mutex_unlock(&lock);
    if (n == 1) rt_log("nvn2: NVN2 control sections are read through NVN1-layout host copies");
    return e->copy;
}
