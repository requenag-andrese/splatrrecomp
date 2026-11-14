// sr.exe --probe-vp9: SR's VP9 path (sr/ov_codec_video.c, the SDK's nn::codec VP9 decoder served with libvpx) on one
// of the game's own movies, UI/Movie/*.webm (the smallest but the black Default.webm, or the first whose path contains
// SR_PROBE_MOVIE). It drives
// the HOST_OVERRIDE entries the title's movie library calls, in its order, on a guest thread of its own:
//   image pool -> decoder -> register buffers -> start -> give every buffer to the decoder (acquire, release) ->
//   submit each VP9 frame (retrying while the decoder answers busy) -> take the filled buffers (acquire, the sync
//   handle reached, properties, RGBA and NV12 reads, release) -> finalize.
// Checks: one picture per frame, the track's size, presentation times in order, end of stream on the last picture
// only, and pictures that are not blank. Without the game data it prints why and exits 0 (skipped).
// The WebM demuxer is totk-pc-recomp's runtime/movie_probe.c demux (MIT), cut to what a VP9 track needs.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/cfg.h"
#include "../core/fs.h"
#include "../core/host.h"
#include "../core/platform/platform.h"

#ifndef SR_HAVE_LIBVPX
int probe_vp9(void) {
    printf("probe-vp9: skipped: built without libvpx (no VP9 decoding in this build)\n");
    return 0;
}
#else

#define U(x) ((uint64_t)(uintptr_t)(x))
#define CV_R(desc) (111u | (uint32_t)(desc) << 9)
enum { CV_BUSY = 3603, CV_EMPTY = 3604, NB = 8 };

static int failures, checks;
#define CHECK(cond, ...) \
    do { checks++; if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

// ---- WebM: the VP9 track's frames (after totk-pc-recomp's movie_probe.c)
typedef struct { uint32_t off, size; int64_t pts; } Frame;   // pts in microseconds
typedef struct { Frame *f; int n, cap, w, h; char codec[32]; } Track;

static int vint(const uint8_t *p, const uint8_t *end, int id, uint64_t *v, int *len, int *unknown) {
    if (p >= end) return 0;
    int n = 1;
    uint8_t mask = 0x80;
    while (n <= 8 && !(p[0] & mask)) mask >>= 1, n++;
    if (n > 8 || p + n > end) return 0;
    uint64_t x = id ? p[0] : (uint64_t)(p[0] & (mask - 1));
    for (int i = 1; i < n; i++) x = x << 8 | p[i];
    *v = x, *len = n;
    if (unknown) *unknown = !id && x == (1ull << (7 * n)) - 1;
    return 1;
}
static uint64_t be_uint(const uint8_t *p, uint64_t n) {
    uint64_t x = 0;
    for (uint64_t i = 0; i < n && i < 8; i++) x = x << 8 | p[i];
    return x;
}
static const char *demux(const uint8_t *d, size_t size, Track *t) {
    const uint8_t *end = d + size, *p = d;
    uint64_t scale = 1000000, cluster = 0, cur_track = 0, vtrack = 0;
    int have_cluster = 0;
    while (p < end) {
        uint64_t id, n;
        int il, sl, unknown;
        if (!vint(p, end, 1, &id, &il, NULL) || !vint(p + il, end, 0, &n, &sl, &unknown)) return "bad element header";
        const uint8_t *body = p + il + sl;
        switch (id) {
        case 0x18538067: case 0x1549A966: case 0x1654AE6B: case 0xAE: case 0xE0: case 0xA0: p = body; continue;
        case 0x1F43B675: p = body; have_cluster = 1; continue;
        }
        if (unknown || n > (uint64_t)(end - body)) return "element past the end of the file";
        switch (id) {
        case 0x2AD7B1: scale = be_uint(body, n); break;
        case 0xD7: cur_track = be_uint(body, n); break;
        case 0x86:
            if (n < sizeof t->codec && !t->codec[0]) memcpy(t->codec, body, n), t->codec[n] = 0, vtrack = cur_track;
            break;
        case 0xB0: t->w = (int)be_uint(body, n); break;
        case 0xBA: t->h = (int)be_uint(body, n); break;
        case 0xE7: cluster = be_uint(body, n); break;
        case 0xA3: case 0xA1: {
            uint64_t track;
            int tl;
            if (!have_cluster || !vint(body, body + n, 0, &track, &tl, NULL) || n < (uint64_t)tl + 3) return "bad block";
            if (track != vtrack) break;
            int16_t rel = (int16_t)(body[tl] << 8 | body[tl + 1]);
            if (body[tl + 2] & 0x06) return "laced video block";
            if (t->n == t->cap && !(t->f = realloc(t->f, (size_t)(t->cap = t->cap ? 2 * t->cap : 1024) * sizeof *t->f)))
                return "frame list allocation";
            t->f[t->n++] = (Frame){(uint32_t)(body + tl + 3 - d), (uint32_t)(n - tl - 3),
                                   ((int64_t)cluster + rel) * (int64_t)scale / 1000};
            break;
        }
        }
        p = body + n;
    }
    return NULL;
}

// ---- the overrides, called as the guest calls them
typedef struct { uint32_t magic, dec, idx, gen; } Sync;   // nn::gs::NativeSyncHandle
static GuestFn F_POOL, F_INIT, F_REG, F_START, F_SUBMIT, F_ACQ, F_REL, F_PROPS, F_RGBA, F_NV12, F_TRYWAIT, F_FIN,
    F_POOLFIN, F_CVINIT;
static Cpu *C;
// AAPCS64: the first eight arguments in x0..x7, the rest on the stack (arg_x reads them at sp)
static uint64_t call(GuestFn f, int n, ...) {
    va_list ap;
    va_start(ap, n);
    uint64_t sp = C->sp, args[16];
    for (int i = 0; i < n && i < 16; i++) args[i] = va_arg(ap, uint64_t);
    va_end(ap);
    for (int i = 0; i < n && i < 8; i++) C->x[i] = args[i];
    if (n > 8) {
        C->sp = (sp - 8ull * (uint64_t)(n - 8)) & ~15ull;
        for (int i = 8; i < n; i++) *(uint64_t *)(uintptr_t)(C->sp + 8ull * (uint64_t)(i - 8)) = args[i];
    }
    f(C);
    C->sp = sp;
    return C->x[0];
}
static GuestFn need(const char *name) {
    GuestFn f = hostfn_override(name);
    if (!f) rt_fatal("probe-vp9: no override for %s", name);
    return f;
}

static Track T;
static const uint8_t *DATA;
static uint64_t dec_t, pool_t;
static int pictures, eos_seen, eos_early, size_bad, order_bad, blank;
static int64_t last_ts = -1;
static uint8_t *rgba, *ny, *nuv;

// Takes every filled buffer: properties, the picture, back to the decoder. pend: acquired, not yet filled.
static Sync pend_sync[NB];
static uint32_t pend_idx[NB];
static int npend;
static void drain(void) {
    for (;;) {   // newly acquirable buffers join the pending ones
        uint32_t idx;
        Sync s;
        if ((uint32_t)call(F_ACQ, 3, U(&idx), U(&s), dec_t) != 0) break;
        if (npend < NB) pend_idx[npend] = idx, pend_sync[npend++] = s;
    }
    for (int k = 0; k < npend;) {
        if (!(call(F_TRYWAIT, 1, U(&pend_sync[k])) & 1)) { k++; continue; }
        uint32_t idx = pend_idx[k];
        uint16_t w = 0, h = 0, cw = 0, ch = 0;
        int64_t ts = 0;
        uint8_t eos = 0;
        if ((uint32_t)call(F_PROPS, 8, U(&w), U(&h), U(&cw), U(&ch), U(&ts), U(&eos), dec_t, (uint64_t)idx) == 0 &&
            w && h) {
            pictures++;
            size_bad += w != T.w || h != T.h;
            order_bad += ts <= last_ts;
            last_ts = ts;
            if (eos) eos_seen++;
            if (eos && pictures < T.n) eos_early++;
            if (pictures == T.n / 2 || pictures == 1) {   // the first and the middle picture: not blank
                call(F_RGBA, 9, pool_t, (uint64_t)idx, 0ull, 0ull, (uint64_t)w, (uint64_t)h, U(rgba), (uint64_t)w * 4,
                     (uint64_t)w * h * 4);
                int lo = 255, hi = 0, opaque = 1;
                for (int i = 0; i < w * h; i++) {
                    int l = (rgba[i * 4] + rgba[i * 4 + 1] + rgba[i * 4 + 2]) / 3;
                    lo = l < lo ? l : lo, hi = l > hi ? l : hi, opaque &= rgba[i * 4 + 3] == 255;
                }
                call(F_NV12, 12, pool_t, (uint64_t)idx, 0ull, 0ull, (uint64_t)w, (uint64_t)h, U(ny), (uint64_t)w,
                     (uint64_t)w * h, U(nuv), (uint64_t)w, (uint64_t)w * h / 2);
                int ylo = 255, yhi = 0;
                for (int i = 0; i < w * h; i++) ylo = ny[i] < ylo ? ny[i] : ylo, yhi = ny[i] > yhi ? ny[i] : yhi;
                if (hi - lo < 8 || !opaque || yhi - ylo < 8) blank++;
                printf("picture %d: %ux%u, RGBA luma %d..%d%s, NV12 Y %d..%d\n", pictures, w, h, lo, hi,
                       opaque ? ", opaque" : ", NOT opaque", ylo, yhi);
            }
        } else if (eos) {
            eos_seen++;   // the end of stream on an empty buffer
        }
        call(F_REL, 4, dec_t, (uint64_t)idx, 0ull, 0ull);   // back to the decoder (the handle goes in x2, x3)
        pend_idx[k] = pend_idx[npend - 1], pend_sync[k] = pend_sync[npend - 1], npend--;
    }
}

static void run(Cpu *c) {
    C = c;
    // movie::VideoDecoder::Initialize calls nv::InitCheck first: the SDK's aborts here, as its graphics set-up is the host's
    call(need("_ZN2nv9InitCheckEv"), 0);
    call(F_CVINIT, 0);
    uint8_t *pref = calloc(1, 0x40), *pool_mem = calloc(1, 0x1000);
    uint64_t *pool = calloc(1, 0x40), *dec = calloc(1, 0x200);
    uint32_t wh = (uint32_t)(T.w - 1) | (uint32_t)(T.h - 1) << 16;
    memcpy(pref + 8, &wh, 4);
    pool_t = U(pool), dec_t = U(dec);
    CHECK(call(F_POOL, 5, pool_t, U(pool_mem), 0x1000ull, U(pref), (uint64_t)NB) == 0, "InitializeDecoderImagePool");
    CHECK(call(F_INIT, 4, dec_t, 0x1000000ull, (uint64_t)T.w, (uint64_t)T.h) == 0, "InitializeVp9DecoderByRemoteMemory");
    for (uint32_t i = 0; i < NB; i++) CHECK(call(F_REG, 4, dec_t, (uint64_t)i, pool_t, (uint64_t)i) == 0, "Register %u", i);
    CHECK(call(F_START, 1, dec_t) == 0, "StartVp9Decoder");
    // every registered buffer is acquirable at once, empty and with its handle reached: give each to the decoder
    for (int k = 0; k < NB; k++) {
        uint32_t idx;
        Sync s;
        if ((uint32_t)call(F_ACQ, 3, U(&idx), U(&s), dec_t) != 0) break;
        call(F_REL, 4, dec_t, (uint64_t)idx, 0ull, 0ull);
    }
    int busy = 0, submitted = 0;
    for (int i = 0; i < T.n; i++) {
        uint64_t out = 0;
        int64_t ts_ns = T.f[i].pts * 1000;
        for (int tries = 0; tries < 64; tries++) {
            uint32_t r = (uint32_t)call(F_SUBMIT, 6, U(&out), dec_t, U(DATA + T.f[i].off), (uint64_t)T.f[i].size,
                                        (uint64_t)ts_ns, (uint64_t)(i == T.n - 1));
            if (r == 0) { submitted++; break; }
            CHECK(r == CV_R(CV_BUSY), "SubmitVp9DecoderInputBuffer frame %d: %#x", i, r);
            if (r != CV_R(CV_BUSY)) break;
            busy++;
            drain();
        }
        drain();
    }
    for (int k = 0; k < 8; k++) drain();
    CHECK(submitted == T.n, "%d of %d frames submitted", submitted, T.n);
    CHECK(pictures == T.n, "%d pictures from %d frames", pictures, T.n);
    CHECK(!size_bad, "%d pictures not %dx%d", size_bad, T.w, T.h);
    CHECK(!order_bad, "%d pictures out of presentation order", order_bad);
    CHECK(eos_seen == 1 && !eos_early, "end of stream seen %d times, %d before the last picture", eos_seen, eos_early);
    CHECK(!blank, "%d checked pictures blank or not opaque", blank);
    CHECK(call(F_FIN, 1, dec_t) == 0 && !*dec, "FinalizeVp9Decoder");
    call(F_POOLFIN, 1, pool_t);
    CHECK(!*pool, "FinalizeDecoderImagePool");
    printf("vp9: %d frames submitted (%d busy answers), %d pictures %dx%d, presentation times in order, end of stream on "
           "the last\n", submitted, busy, pictures, T.w, T.h);
}

int probe_vp9(void) {
    fs_init();
    const char *want = cfg_get("SR_PROBE_MOVIE");
    const char *name = NULL;
    uint64_t best = UINT64_MAX;
    int movies = 0;
    for (uint64_t i = 0; i < romfs_file_count(); i++) {
        uint64_t size;
        const char *p = romfs_entry(i, NULL, &size);
        size_t n = p ? strlen(p) : 0;
        if (!p || strncmp(p, "UI/Movie/", 9) || n < 5 || strcmp(p + n - 5, ".webm")) continue;
        movies++;
        // Default.webm is a black placeholder: the smallest of the others shows real pictures
        if (want ? strstr(p, want) && !name : size < best && !strstr(p, "/Default.")) name = p, best = size;
    }
    if (!name) {
        printf("probe-vp9: skipped: no UI/Movie/*.webm%s%s in the RomFS (%d movies; game data absent?)\n",
               want ? " matching " : "", want ? want : "", movies);
        return 0;
    }
    char path[512];
    snprintf(path, sizeof path, "probe:/%s", name);
    uint64_t h, got;
    int64_t size;
    if (romfs_mount("probe") || romfs_open(&h, path, FS_OPEN_READ) || romfs_size(&size, h) || size <= 0) {
        printf("probe-vp9: FAILED: cannot open %s\n", path);
        return 1;
    }
    uint8_t *data = malloc((size_t)size);
    int ok = data && romfs_read(&got, h, 0, data, (uint64_t)size) == 0 && got == (uint64_t)size;
    romfs_close(h);
    const char *err = ok ? demux(data, (size_t)size, &T) : "read";
    if (err || strcmp(T.codec, "V_VP9") || !T.n || T.w <= 0 || T.h <= 0) {
        printf("probe-vp9: FAILED: WebM %s: %s (codec %s, %d frames, %dx%d)\n", name, err ? err : "not a VP9 track",
               T.codec, T.n, T.w, T.h);
        return 1;
    }
    DATA = data;
    printf("probe-vp9: %d movies in UI/Movie; %s: %s %dx%d, %d frames, %.2f s\n", movies, name, T.codec, T.w, T.h, T.n,
           (T.f[T.n - 1].pts - T.f[0].pts) / 1e6);
    F_CVINIT = need("_ZN2nn5codec5video10InitializeEv");
    F_POOL = need("_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEj");
    F_POOLFIN = need("_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE");
    F_INIT = need("_ZN2nn5codec34InitializeVp9DecoderByRemoteMemoryEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE");
    F_FIN = need("_ZN2nn5codec18FinalizeVp9DecoderEPNS0_14Vp9DecoderTypeE");
    F_REG = need("_ZN2nn5codec24RegisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj");
    F_START = need("_ZN2nn5codec15StartVp9DecoderEPNS0_14Vp9DecoderTypeE");
    F_SUBMIT = need("_ZN2nn5codec27SubmitVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKvmNS_8TimeSpanEb");
    F_ACQ = need("_ZN2nn5codec29AcquireVp9DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp9DecoderTypeE");
    F_REL = need("_ZN2nn5codec29ReleaseVp9DecoderOutputBufferEPNS0_14Vp9DecoderTypeEjNS_2gs16NativeSyncHandleE");
    F_PROPS = need("_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj");
    F_RGBA = need("_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm");
    F_NV12 = need("_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm");
    F_TRYWAIT = need("_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE");
    rgba = malloc((size_t)T.w * T.h * 4), ny = malloc((size_t)T.w * T.h), nuv = malloc((size_t)T.w * T.h / 2 + 1);
    ldr_load(rt_config()->exefs);   // the decoder clears and destroys its event through the SDK's nn::os
    kern_run_on_guest_main(run);
    if (failures) {
        printf("probe-vp9: FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("probe-vp9: passed (%d checks)\n", checks);
    return 0;
}
#endif
