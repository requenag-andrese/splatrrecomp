// Uniform blocks as of the submit call. NVN starts the GPU when nvnQueueSubmitCommands returns and the GPU reads the
// constants soon after; here the replay records for several milliseconds, and the GPU reads a pool's bytes when it
// runs the part that uses them (the VRAM mirror's sync copies run then, vk_memory.c "VRAM mirror"). Meanwhile the game,
// having handed the frame over, rewrites per-object uniform blocks on other threads with no fence: late draws read
// another object's matrix (phantom rocks, a flat far field; TotK 1.2.1's producer f_70008c6460, 14-24 ms after the
// submit). So at the start of a submission's recording every uniform range it binds is copied into an arena of its own,
// and the uniform bindings of the replay use the copy.
// - Only ranges of imported pools (memory the CPU writes) are copied, aligned out to 256 bytes (the largest
//   minUniformBufferOffsetAlignment) and at most 64 KiB from the bound address, overlapping ones merged.
// - A range the GPU wrote in a submission that has not completed is left out: its bytes reach guest memory only with
//   that submission's write-back, the mirror holds them until then.
// - A GPU write within the submission (UpdateUniformBuffer, a storage buffer a shader writes, a buffer copy or fill,
//   an image-to-buffer copy: vk_usnap_drop) drops the copies it touches from then on, so later draws read the result.
// - The arena goes back to a free list once the submission's fence is done (completion thread).
// The design follows MacNativeGames' SR_UBO_SNAPSHOT (games/totk/third_party/d2r_gfx/gfx/nvn/nvn_mtl.m usnap_take,
// usnap_drop; MIT, Copyright (c) 2026 jhaizhou); the code is ours.
// - sr-recomp, UpdateUniformBuffer within a render pass (SR_UB_INPASS, on by default): instead of a drop, vk_usnap_patch
//   writes a newer version of the 256-byte-aligned stretch it touches into spare room at the end of the arena, and the
//   lookups from then on (vk_usnap_find, so vk_usnap_bytes too) see the newest version of every byte. The replay leaves
//   the pass open and copies the bytes into guest memory's buffer at the next natural pass end (vk_exec_draw.c
//   op_update_ub, vk_exec_ub_flush): the game's per-draw updates had ended the pass behind a full barrier each time,
//   309 times a frame in one 880x880 pass, 6 of the GPU's 11 ms.
// The copy is taken where vk_exec_record starts: at the submit call when the replay runs on the submitting thread, later
// with SR_REPLAY_ASYNC. SR_UBO_SNAPSHOT=0 binds live memory as before; =ab alternates (SR_AB).
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "vk.h"
#include "vk_dlaa.h"
#include "platform/platform.h"

#define ALIGN 256u
#define MAX_RANGE (64u << 10)
#define ARENA_MIN (4u << 20)
#define CALL_DEPTH 16
#define HEADROOM (4u << 20)   // spare arena room per submission for vk_usnap_patch's versions

typedef struct Arena {
    struct Arena *next;
    VkBuffer buf;
    VmaAllocation alloc;
    uint8_t *map;
    uint8_t *cpu;    // with DLSS (vk_dlaa_available), the same bytes in host memory for the CPU's reads (vk_usnap_bytes):
                     // map is often device memory through the BAR, written fast but read back at about a microsecond a
                     // cache line. Without DLSS nothing reads them back, and the bytes go into map in one copy.
    uint64_t size;
} Arena;

typedef struct { uint64_t lo, hi; } Rng;
typedef struct { uint64_t lo, hi, off; int dropped, last; } Reg;   // last: its newest version, -1 none
typedef struct { uint64_t lo, hi, fence; int ub; } Wrote;   // ub: only UpdateUniformBuffers, whose bytes Known holds
typedef struct { uint64_t addr, fence; uint32_t n, at; } Known;   // an update's bytes at kbytes + at (SR_UB_INPASS)
typedef struct { uint64_t lo, hi, off; int prev; } Ver;   // a newer version of [lo, hi) at arena offset off
                                                          // (vk_usnap_patch); prev: the region's one before, -1

static struct {
    int mode;                   // rt_knob: 0 off, 1 on, 2 ab
    int on;                     // this submission has a snapshot
    uint64_t fence, gen;
    Arena *arena;
    Rng *col;                   // collected ranges
    int ncol, ccol;
    Reg *reg;                   // merged, sorted by lo, disjoint
    int nreg, creg;
    Wrote *wr;                  // GPU writes of submissions not known to be complete
    int nwr, cwr;
    Ver *ver;                   // this submission's versions, oldest first
    int nver, cver;
    Known *kn;                  // UpdateUniformBuffers of submissions not known to be complete, in replay order
    int nkn, ckn;
    uint8_t *kbytes;
    uint32_t nkb, ckb;
    uint64_t used;              // arena bytes in use: the regions, then the versions
    int inpass;                 // SR_UB_INPASS this submission: 1 on, -1 off
    int known;                  // SR_UB_KNOWN this submission
    uint64_t why[3];            // updates not kept: no region, region dropped, across its edge
    // since the last summary
    uint64_t subs, regions, bytes, copy_ns, hits, drops, inflight, refused, off_subs, patches, patch_bytes, unpatched;
} U;

// sr-recomp: uniform blocks still bound when a submission starts (bound by an earlier one): vk_exec_record notes them
// before vk_usnap_begin, which copies them with the ranges its own binds name. A pass that only draws with blocks bound
// before read live memory, and its UpdateUniformBuffers found no region to keep.
static Rng carry[6 * 18];
static int ncarry;

void vk_usnap_carry(uint64_t addr, uint64_t size) {
    if (!addr || !size || ncarry == (int)(sizeof carry / sizeof *carry)) return;
    const uint64_t n = size < MAX_RANGE ? size : MAX_RANGE;
    carry[ncarry++] = (Rng){addr & ~(uint64_t)(ALIGN - 1), addr + n};
}

static Arena *free_arenas;
static PlatMutex free_lock = PLAT_MUTEX_INIT;

static void *grow(void *p, int *cap, size_t elem) {
    int n = *cap ? *cap * 2 : 1024;
    void *q = realloc(p, (size_t)n * elem);
    if (!q) rt_fatal("vk_usnap: out of memory");
    *cap = n;
    return q;
}

static Arena *arena_get(uint64_t need) {
    plat_mutex_lock(&free_lock);
    Arena **pp = &free_arenas, *a = NULL;
    for (; *pp; pp = &(*pp)->next)
        if ((*pp)->size >= need) {
            a = *pp, *pp = a->next;
            break;
        }
    plat_mutex_unlock(&free_lock);
    if (a) return a;
    a = calloc(1, sizeof *a);
    if (!a) rt_fatal("vk_usnap: out of memory");
    a->size = need < ARENA_MIN ? ARENA_MIN : (need + (1u << 20) - 1) & ~(uint64_t)((1u << 20) - 1);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = a->size,
                             // transfer source: DLAA copies the camera block out (vk_dlaa.c mv_pass)
                             .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    // Device-local through the BAR when the device has it (the GPU reads constants on every draw), else host memory.
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                           VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo info;
    VkResult res = vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &a->buf, &a->alloc, &info);
    if (res != VK_SUCCESS) {
        rt_log("vk_usnap: arena of %llu bytes failed (%d): this submission binds live memory",
               (unsigned long long)a->size, (int)res);
        free(a);
        return NULL;
    }
    a->map = info.pMappedData;
    rt_log("vk_usnap: new arena of %.1f MiB", a->size / 1048576.0);
    return a;
}

static void arena_done(void *arg) {   // completion thread: the GPU has read it
    Arena *a = arg;
    plat_mutex_lock(&free_lock);
    a->next = free_arenas, free_arenas = a;
    plat_mutex_unlock(&free_lock);
}

static void collect(const op_hdr *h, int depth) {
    for (;;) {
        if (h->size < sizeof *h || (h->size & 7) || h->op >= OP_COUNT_)
            rt_fatal("vk_usnap: corrupt command record at %p: op %u size %u", (const void *)h, h->op, h->size);
        const void *a = nvn_op_payload(h);
        if (h->op == OP_END) return;
        if (h->op == OP_JUMP) {
            h = (const op_hdr *)(uintptr_t)((const p_jump *)a)->target;
            continue;
        }
        if (h->op == OP_CALL && depth < CALL_DEPTH) collect(nvn_op_first(((const p_jump *)a)->target), depth + 1);
        if (h->op == OP_BIND_UBO) {
            const p_bind_buf *b = a;
            if (b->addr && b->size) {
                uint64_t n = b->size < MAX_RANGE ? b->size : MAX_RANGE;
                // the vertex stage's object and bone blocks: also the game's other copy on either side, which still
                // holds last frame's (vk_mvprev.c)
                uint64_t step = vk_mvprev_step(b->stage, b->index, b->size);
                // which neighbours lie in an imported pool, per block and vk_mem_gen: the same blocks are bound by
                // many draws, and two resolves for each binding were a quarter of the snapshot's time
                static struct { uint64_t addr, size, gen; int ok; } nc[1024];
                uint32_t ci = (uint32_t)((b->addr ^ n << 48) * 0x9E3779B97F4A7C15ull >> 54);
                uint64_t gen = vk_mem_gen();
                int known = step && nc[ci].addr == b->addr && nc[ci].size == n && nc[ci].gen == gen, ok = 0;
                for (int k = step ? -1 : 0; k <= (step ? 1 : 0); k++) {
                    uint64_t at = b->addr + (uint64_t)((int64_t)k * (int64_t)step);
                    if (k < 0 && b->addr < step) continue;
                    // a neighbour past its pool would make the whole merged region fail the pool check below, and
                    // the bindings in it read live memory again
                    VkMemSpan sp;
                    uint64_t lo = at & ~(uint64_t)(ALIGN - 1);
                    if (k && known) {
                        if (!(nc[ci].ok >> (k + 1) & 1)) continue;
                    } else if (k) {
                        if (!vk_mem_resolve(lo, at + n - lo, &sp) || sp.alloc || !sp.pool || sp.avail < at + n - lo ||
                            (sp.cls != VK_MCLS_HOST_CACHED && sp.cls != VK_MCLS_HOST_UNC))
                            continue;
                        ok |= 1 << (k + 1);
                    }
                    if (U.ncol == U.ccol) U.col = grow(U.col, &U.ccol, sizeof *U.col);
                    U.col[U.ncol++] = (Rng){at & ~(uint64_t)(ALIGN - 1), at + n};
                }
                if (step && !known) nc[ci].addr = b->addr, nc[ci].size = n, nc[ci].gen = gen, nc[ci].ok = ok;
            }
        }
        h = nvn_op_next(h);
    }
}

static int by_lo(const void *a, const void *b) {
    uint64_t x = ((const Rng *)a)->lo, y = ((const Rng *)b)->lo;
    return x < y ? -1 : x > y;
}
static int wr_by_lo(const void *a, const void *b) {
    uint64_t x = ((const Wrote *)a)->lo, y = ((const Wrote *)b)->lo;
    return x < y ? -1 : x > y;
}

// Sorts the collected ranges by lo. A town frame binds about 10,000 uniform ranges and qsort took half a millisecond
// of the copy's 0.96 ms on the game thread, so they go through an LSD radix sort on (lo - lowest) / ALIGN, three
// passes of 11 bits; qsort is left for a span that does not fit in 33 bits (2 TiB).
#define RADIX_BITS 11
static void sort_col(void) {
    static Rng *tmp;
    static int ctmp;
    int n = U.ncol;
    if (n < 2) return;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < n; i++) {
        if (U.col[i].lo < lo) lo = U.col[i].lo;
        if (U.col[i].lo > hi) hi = U.col[i].lo;
    }
    if ((hi - lo) / ALIGN >> (3 * RADIX_BITS)) {
        qsort(U.col, (size_t)n, sizeof *U.col, by_lo);
        return;
    }
    if (ctmp < n) {
        free(tmp);
        ctmp = U.ccol;
        if (!(tmp = malloc((size_t)ctmp * sizeof *tmp))) rt_fatal("vk_usnap: out of memory");
    }
    Rng *src = U.col, *dst = tmp;
    for (int pass = 0; pass < 3; pass++) {
        int shift = pass * RADIX_BITS, count[1 << RADIX_BITS] = {0};
        for (int i = 0; i < n; i++) count[(src[i].lo - lo) / ALIGN >> shift & ((1 << RADIX_BITS) - 1)]++;
        for (int k = 0, sum = 0; k < 1 << RADIX_BITS; k++) {
            int c = count[k];
            count[k] = sum, sum += c;
        }
        for (int i = 0; i < n; i++) dst[count[(src[i].lo - lo) / ALIGN >> shift & ((1 << RADIX_BITS) - 1)]++] = src[i];
        Rng *t = src;
        src = dst, dst = t;
    }
    if (src != U.col) memcpy(U.col, src, (size_t)n * sizeof *src);   // three passes end in tmp
}

// Keeps the GPU writes of submissions that are not done, merged into disjoint ranges sorted by lo (the terrain rewrites
// one block before every draw: hundreds of equal entries), each with the newest fence among its parts.
static void wr_prune(void) {
    uint64_t done = vk_sync_fence_done();
    int n = 0;
    for (int i = 0; i < U.nwr; i++)
        if (U.wr[i].fence > done) U.wr[n++] = U.wr[i];
    qsort(U.wr, (size_t)n, sizeof *U.wr, wr_by_lo);
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (m && U.wr[i].lo <= U.wr[m - 1].hi) {
            if (U.wr[i].hi > U.wr[m - 1].hi) U.wr[m - 1].hi = U.wr[i].hi;
            if (U.wr[i].fence > U.wr[m - 1].fence) U.wr[m - 1].fence = U.wr[i].fence;
            U.wr[m - 1].ub &= U.wr[i].ub;
        } else {
            U.wr[m++] = U.wr[i];
        }
    }
    U.nwr = m;
    // the known updates of finished submissions: guest memory has their bytes now
    int kk = 0;
    uint32_t kb = 0;
    for (int i = 0; i < U.nkn; i++)
        if (U.kn[i].fence > done) {
            memmove(U.kbytes + kb, U.kbytes + U.kn[i].at, U.kn[i].n);
            U.kn[kk] = U.kn[i], U.kn[kk++].at = kb, kb += U.kn[i].n;
        }
    U.nkn = kk, U.nkb = kb;
}
// 0: no GPU write in flight touches [lo, hi); 1: only UpdateUniformBuffers whose bytes are known; 2: others too.
static int wr_hits(uint64_t lo, uint64_t hi) {   // wr_prune ran: disjoint, sorted
    int a = 0, b = U.nwr;
    while (a < b) {
        int k = (a + b) / 2;
        if (U.wr[k].hi <= lo) a = k + 1;
        else b = k;
    }
    int hit = 0;
    for (; a < U.nwr && U.wr[a].lo < hi; a++) hit = U.wr[a].ub && hit != 2 ? 1 : 2;
    return hit;
}

static int first_above(uint64_t a);

static void known_add(uint64_t addr, uint64_t n, const void *data) {
    if (U.nkn == U.ckn) U.kn = grow(U.kn, &U.ckn, sizeof *U.kn);
    while (U.nkb + n > U.ckb) {
        U.ckb = U.ckb ? U.ckb * 2 : 1u << 16;
        if (!(U.kbytes = realloc(U.kbytes, U.ckb))) rt_fatal("vk_usnap: out of memory");
    }
    memcpy(U.kbytes + U.nkb, data, n);
    U.kn[U.nkn++] = (Known){addr, U.fence, (uint32_t)n, U.nkb};
    U.nkb += (uint32_t)n;
}

static void summary(void) {
    if (U.subs + U.off_subs < 3600) return;
    double s = U.subs ? (double)U.subs : 1.0;
    rt_log("vk_usnap: %llu submissions with a snapshot, %llu without; per submission %.0f ranges, %.1f KiB copied in "
           "%.3f ms, %.0f uniform bindings from the copy, %.1f dropped by GPU writes, %.1f left out (GPU writes in "
           "flight), %.1f refused (not an imported pool); %.1f updates kept in the pass (%.1f KiB of versions), %.1f not (%.1f no region, %.1f dropped, %.1f across an edge)",
           (unsigned long long)U.subs, (unsigned long long)U.off_subs, U.regions / s, U.bytes / s / 1024.0,
           U.copy_ns / s / 1e6, U.hits / s, U.drops / s, U.inflight / s, U.refused / s, U.patches / s,
           U.patch_bytes / s / 1024.0, U.unpatched / s, U.why[0] / s, U.why[1] / s, U.why[2] / s);
    U.subs = U.off_subs = U.regions = U.bytes = U.copy_ns = U.hits = U.drops = U.inflight = U.refused = 0;
    U.patches = U.patch_bytes = U.unpatched = U.why[0] = U.why[1] = U.why[2] = 0;
}

void vk_usnap_begin(const uint64_t *handles, int n, uint64_t fence) {
    if (!U.mode) U.mode = rt_knob("UBO_SNAPSHOT", "1") + 1;   // stored +1: 0 is "not read yet"
    U.on = 0, U.fence = fence, U.gen++, U.nver = 0;
    const int carried = ncarry;
    ncarry = 0;
    (void)carried;
    // SR_UB_INPASS (versions), SR_UB_KNOWN (in-flight update bytes laid over guest memory): 0, 1 or ab, decided
    // per submission; on by default
    static int m_inpass = -1, m_known = -1;
    if (m_inpass < 0) {
        m_inpass = rt_knob("UB_INPASS", "1"), m_known = rt_knob("UB_KNOWN", "1");
        rt_log("vk_usnap: UpdateUniformBuffer within a pass: versions %s, known in-flight bytes %s (SR_UB_INPASS, SR_UB_KNOWN)",
               m_inpass == 2 ? "ab" : m_inpass ? "on" : "off", m_known == 2 ? "ab" : m_known ? "on" : "off");
    }
    U.inpass = RT_KNOB_ON(m_inpass) ? 1 : -1;
    U.known = RT_KNOB_ON(m_known) && U.inpass > 0;
    summary();
    if (!RT_KNOB_ON(U.mode - 1)) {
        U.off_subs++;
        return;
    }
    uint64_t t0 = kern_now_ns();
    U.ncol = 0;
    for (int i = 0; i < n; i++)
        if (handles[i]) collect(nvn_op_first(handles[i]), 0);
    for (int i = 0; i < carried; i++) {
        if (U.ncol == U.ccol) U.col = grow(U.col, &U.ccol, sizeof *U.col);
        U.col[U.ncol++] = carry[i];
    }
    sort_col();
    wr_prune();
    // merge, keeping only what lies wholly in one imported pool and no GPU write in flight touches
    U.nreg = 0;
    uint64_t total = 0;
    for (int i = 0; i < U.ncol;) {
        uint64_t lo = U.col[i].lo, hi = U.col[i].hi;
        for (i++; i < U.ncol && U.col[i].lo <= hi; i++)
            if (U.col[i].hi > hi) hi = U.col[i].hi;
        VkMemSpan sp;
        if (!vk_mem_resolve(lo, hi - lo, &sp) || sp.alloc || !sp.pool || sp.avail < hi - lo ||
            (sp.cls != VK_MCLS_HOST_CACHED && sp.cls != VK_MCLS_HOST_UNC)) {
            U.refused++;
            continue;
        }
        const int hit = wr_hits(lo, hi);
        if (hit == 2 || (hit == 1 && !U.known)) {
            U.inflight++;
            continue;
        }
        if (U.nreg == U.creg) U.reg = grow(U.reg, &U.creg, sizeof *U.reg);
        U.reg[U.nreg++] = (Reg){lo, hi, total, 0, -1};
        total += (hi - lo + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);
    }
    if (!U.nreg) return;
    if (!(U.arena = arena_get(total + (U.inpass > 0 ? HEADROOM : 0)))) return;
    U.used = total;
    uint8_t *dst = U.arena->map;
    if (vk_dlaa_available() || U.inpass > 0) {   // vk_usnap_patch reads the current bytes back too
        if (!U.arena->cpu && !(U.arena->cpu = malloc(U.arena->size))) rt_fatal("vk_usnap: out of memory");
        dst = U.arena->cpu;
    }
    for (int i = 0; i < U.nreg; i++)
        memcpy(dst + U.reg[i].off, (const void *)(uintptr_t)U.reg[i].lo, U.reg[i].hi - U.reg[i].lo);
    // sr-recomp: UpdateUniformBuffers of submissions still running, which guest memory has not got yet, in order
    for (int k = 0; k < (U.known ? U.nkn : 0); k++) {
        const Known *e = &U.kn[k];
        for (int i = first_above(e->addr); i < U.nreg && U.reg[i].lo < e->addr + e->n; i++) {
            const uint64_t a = e->addr > U.reg[i].lo ? e->addr : U.reg[i].lo,
                           b = e->addr + e->n < U.reg[i].hi ? e->addr + e->n : U.reg[i].hi;
            if (a < b) memcpy(dst + U.reg[i].off + (a - U.reg[i].lo), U.kbytes + e->at + (a - e->addr), b - a);
        }
    }
    if (dst != U.arena->map) memcpy(U.arena->map, dst, total);   // one sequential write into the device copy
    vmaFlushAllocation(vk_ctx()->vma, U.arena->alloc, 0, total);   // a no-op on coherent memory
    U.on = 1;
    U.subs++, U.regions += (uint64_t)U.nreg, U.bytes += total, U.copy_ns += kern_now_ns() - t0;
}

void vk_usnap_end(uint64_t fence) {
    if (U.arena) vk_completion_add(fence, arena_done, U.arena), U.arena = NULL;
    U.on = 0, U.gen++;
}

// First region whose end is above a.
static int first_above(uint64_t a) {
    int lo = 0, hi = U.nreg;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (U.reg[m].hi <= a) lo = m + 1;
        else hi = m;
    }
    return lo;
}

// the host copy where there is one (DLSS, SR_UB_INPASS), else the device copy
static uint8_t *host_copy(void) { return U.arena->cpu ? U.arena->cpu : U.arena->map; }

// A new version of [lo, hi) (within region r, lo 256-aligned) at the end of the arena: the region's bytes with every
// earlier version laid over them in order. Its index, -1 when the arena has no room left.
// The region's versions, newest first, back to the first that covers [lo, hi) whole (it holds every older byte there):
// into list[], their count. A replay of 1,100 draws a frame looked up every uniform binding against every version of
// the submission, and its thread took 26 ms a frame.
enum { CHAIN_MAX = 64 };
static int ver_chain(const Reg *r, uint64_t lo, uint64_t hi, int list[CHAIN_MAX]) {
    int n = 0;
    for (int k = r->last; k >= 0 && n < CHAIN_MAX; k = U.ver[k].prev) {
        const Ver *v = &U.ver[k];
        if (v->hi <= lo || v->lo >= hi) continue;
        list[n++] = k;
        if (v->lo <= lo && v->hi >= hi) return n;
    }
    return n < CHAIN_MAX ? n : -1;   // too many partial ones: the caller gives up
}

static int ver_make(Reg *r, uint64_t lo, uint64_t hi) {
    const uint64_t n = hi - lo, room = (n + ALIGN - 1) & ~(uint64_t)(ALIGN - 1);
    if (!U.arena->cpu || U.used + room > U.arena->size) return -1;
    int list[CHAIN_MAX];
    const int nl = ver_chain(r, lo, hi, list);
    if (nl < 0) return -1;
    const uint64_t off = U.used;
    uint8_t *h = U.arena->cpu;
    memcpy(h + off, h + r->off + (lo - r->lo), n);
    for (int i = nl; i-- > 0;) {   // oldest first
        const Ver *v = &U.ver[list[i]];
        const uint64_t a = v->lo > lo ? v->lo : lo, b = v->hi < hi ? v->hi : hi;
        if (a < b) memcpy(h + off + (a - lo), h + v->off + (a - v->lo), b - a);
    }
    U.used += room;
    if (U.nver == U.cver) U.ver = grow(U.ver, &U.cver, sizeof *U.ver);
    U.ver[U.nver] = (Ver){lo, hi, off, r->last};
    r->last = U.nver;
    return U.nver++;
}

static void ver_publish(const Ver *v) {   // the device copy of a version the host copy now holds
    memcpy(U.arena->map + v->off, U.arena->cpu + v->off, v->hi - v->lo);
}

int vk_usnap_find(uint64_t addr, uint64_t n, uint64_t align, VkDescriptorBufferInfo *out) {
    if (!U.on || !n) return 0;
    int i = first_above(addr);
    if (i >= U.nreg) return 0;
    Reg *r = &U.reg[i];
    if (r->dropped || addr < r->lo || addr + n > r->hi) return 0;
    uint64_t off = r->off + (addr - r->lo);
    // the region's newest version that touches the range: it holds every byte when it covers it, else a version of the
    // whole range is made from the region and its versions
    for (int k = r->last; k >= 0; k = U.ver[k].prev) {
        const Ver *v = &U.ver[k];
        if (v->hi <= addr || v->lo >= addr + n) continue;
        if (addr < v->lo || addr + n > v->hi) {
            const int m = ver_make(r, addr & ~(uint64_t)(ALIGN - 1), addr + n);
            if (m < 0) return 0;   // no room: live memory (the replay flushes before such a read: vk_usnap_patched)
            ver_publish(&U.ver[m]);
            v = &U.ver[m];
        }
        off = v->off + (addr - v->lo);
        break;
    }
    if (off % align) return 0;
    *out = (VkDescriptorBufferInfo){U.arena->buf, off, n};
    U.hits++;
    return 1;
}

void vk_usnap_drop(uint64_t addr, uint64_t n) {
    if (!U.mode || !n) return;
    if (U.nwr == U.cwr) U.wr = grow(U.wr, &U.cwr, sizeof *U.wr);
    U.wr[U.nwr++] = (Wrote){addr, addr + n, U.fence, 0};
    if (!U.on) return;
    for (int i = first_above(addr); i < U.nreg && U.reg[i].lo < addr + n; i++)
        if (!U.reg[i].dropped) U.reg[i].dropped = 1, U.drops++, U.gen++;
}

uint64_t vk_usnap_gen(void) { return U.gen; }

int vk_usnap_patch(uint64_t addr, uint64_t n, const void *data) {
    if (U.inpass <= 0 || !U.on || !n || !U.arena || !U.arena->cpu) return 0;
    int i = first_above(addr);
    Reg *r = i < U.nreg ? &U.reg[i] : NULL;
    if (!r || r->dropped || addr < r->lo || addr + n > r->hi) {
        U.unpatched++;
        U.why[!r || addr + n <= r->lo ? 0 : r->dropped ? 1 : 2]++;
        return 0;
    }
    const uint64_t lo = addr & ~(uint64_t)(ALIGN - 1), hi0 = (addr + n + ALIGN - 1) & ~(uint64_t)(ALIGN - 1),
                   hi = hi0 < r->hi ? hi0 : r->hi;
    // a quarter of the room stays for the versions vk_usnap_find makes of blocks an update covers in part: a lookup
    // that found no room would read guest memory, which the waiting copies have not reached yet
    if (U.used + (hi - lo) + HEADROOM / 4 > U.arena->size) {
        U.unpatched++;
        return 0;
    }
    const int k = ver_make(r, lo, hi);
    if (k < 0) {
        U.unpatched++;
        return 0;
    }
    const Ver *v = &U.ver[k];
    memcpy(U.arena->cpu + v->off + (addr - lo), data, n);
    ver_publish(v);
    known_add(addr, n, data);
    U.gen++, U.patches++, U.patch_bytes += hi - lo;
    return 1;
}

int vk_usnap_patched(uint64_t addr, uint64_t n) {
    for (int k = 0; k < U.nver; k++)
        if (U.ver[k].lo < addr + n && addr < U.ver[k].hi) return 1;
    return 0;
}

void vk_usnap_wrote(uint64_t addr, uint64_t n) {
    if (!U.mode || !n) return;
    if (U.nwr == U.cwr) U.wr = grow(U.wr, &U.cwr, sizeof *U.wr);
    U.wr[U.nwr++] = (Wrote){addr, addr + n, U.fence, 1};
}

void vk_usnap_drop_ub(uint64_t addr, uint64_t n, const void *data) {
    if (U.inpass <= 0 || !U.mode || !n) {
        vk_usnap_drop(addr, n);
        return;
    }
    vk_usnap_drop(addr, n);
    U.wr[U.nwr - 1].ub = 1;   // the entry vk_usnap_drop just added
    known_add(addr, n, data);
}

const uint8_t *vk_usnap_map(void) { return U.arena ? host_copy() : NULL; }

const uint8_t *vk_usnap_bytes(uint64_t addr, uint64_t n) {
    VkDescriptorBufferInfo bi;
    return vk_usnap_find(addr, n, 1, &bi) ? host_copy() + bi.offset : NULL;
}

// The arenas back on the free list (vk_exec_draw_shutdown: the device is idle, every completion ran).
void vk_usnap_shutdown(void) {
    plat_mutex_lock(&free_lock);
    for (Arena *a = free_arenas, *next; a; a = next) next = a->next, vmaDestroyBuffer(vk_ctx()->vma, a->buf, a->alloc), free(a->cpu), free(a);
    free_arenas = NULL;
    plat_mutex_unlock(&free_lock);
}
