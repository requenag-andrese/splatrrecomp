// nn::codec VP9 video decoding of the Switch 2 SDK (21.4.4), served on the host with libvpx. The title's movie library
// (subsdk0, movie::VideoDecoder) sends every VP9 packet to these SDK calls, which would talk to a decoder service the
// host does not have. Ported from nx2android PR #88's nxnative dm/host/hle_codec_video.c and hle_movie.c dm_yuv_* (MIT)
// to this SDK's signatures and to totk-pc-recomp's runtime:
//   - the client makes one image pool per picture and registers it with the decoder under an index; registering makes
//     the index acquirable (a queue NQ deep) with a sync handle already reached;
//   - Release gives the buffer to the decoder and makes it acquirable again at once, with a new sync handle reached
//     when a picture is in it; Acquire takes the oldest index (CV_EMPTY when none). The client waits on the handle
//     (nn::gs::TryWaitNativeSync or a sync point event), then reads the picture's properties and the picture
//     (ReadDecoderOutputImageNv12 / Rgba);
//   - Submit takes one packet with its time and an end-of-stream flag (CV_BUSY while NHOLD pictures wait for buffers:
//     the client retries on the submission-ready event, the SystemEventType at decoder type + 8 in this SDK).
// Differences from SDK 20.5.19 (nxnative): the decoder is initialised ByTransferMemory / ByRemoteMemory, the image pool
// takes no CodecMode, GetVp9DecoderSubmissionReadyEvent returns the event (type + 8; not overridden), and
// MakeDecodeImagePreference is the SDK's own (pure: a 0x40-byte struct with width - 1 and height - 1 at +8, read here).
// The opaque decoder and pool types hold the host object's pointer at +0; sync point events keep their state on the
// host, keyed by the event's address, so nothing past its SystemEventType at +0 is written.
#ifdef SR_HAVE_LIBVPX
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "vpx/vp8dx.h"
#include "vpx/vpx_decoder.h"

#include "../core/host.h"
#include "../core/platform/platform.h"

#define CV_R(desc) (111u | (uint32_t)(desc) << 9)   // nn::codec results
enum { CV_BAD = 2004, CV_BUSY = 3603, CV_EMPTY = 3604 };
enum { NBUF = 32, NQ = 8, NHOLD = 4, NDEC = 16, NSPT = 64 };
enum { EVENT_OFF = 8 };   // the submission-ready SystemEventType inside Vp9DecoderType
#define MAGIC 0x56436d44u   // 'DmCV'
#define SYNC 0x53436d44u    // 'DmCS'

typedef struct { uint32_t magic, dec, idx, gen; } cv_sync;   // nn::gs::NativeSyncHandle (16 bytes)
typedef struct { uint32_t magic; uint16_t w, h; uint32_t n; vpx_image_t **img; } cv_pool;
typedef struct {
    uint32_t magic, id;
    PlatMutex mu;
    vpx_codec_ctx_t vpx;
    int vpx_ok;
    uint64_t event, submits, pictures;
    struct { cv_pool *pool; uint32_t pidx, gen, filled_gen; uint8_t reg, filled, eos; uint16_t w, h; int64_t ts; } b[NBUF];
    uint32_t acq[NQ], nacq, want[NBUF], nwant;   // acquirable indices; released ones waiting for a picture
    vpx_image_t *held[NHOLD];
    int64_t held_ts[NHOLD];
    uint8_t held_eos[NHOLD];
    int nheld, eos_pending;
} cv_dec;

static cv_dec *g_dec[NDEC];
static PlatMutex g_mu = PLAT_MUTEX_INIT;   // lock order: g_mu, a decoder's mu, g_spt_mu

static void red_once(int *seen, const char *msg) {
    if (!__atomic_exchange_n(seen, 1, __ATOMIC_RELAXED)) rt_log("codec: %s", msg);
}
static cv_dec *dec_of(uint64_t t) {
    cv_dec *d = t ? *(cv_dec **)(uintptr_t)t : NULL;
    return d && d->magic == MAGIC ? d : NULL;
}
static cv_pool *pool_of(uint64_t t) {
    cv_pool *p = t ? *(cv_pool **)(uintptr_t)t : NULL;
    return p && p->magic == MAGIC ? p : NULL;
}
static void q_drop(uint32_t *q, uint32_t *n, uint32_t i) {
    uint32_t k = 0;
    for (uint32_t j = 0; j < *n; j++)
        if (q[j] != i) q[k++] = q[j];
    *n = k;
}

// ---- guest system events
static uint64_t os_sym(const char *name) {
    uint64_t a = ldr_symbol(name);
    if (!a) rt_fatal("codec: the SDK function %s is not loaded", name);
    return a;
}
static void ev_clear(Cpu *c, uint64_t ev) {
    static uint64_t f;
    if (!f) f = os_sym("_ZN2nn2os16ClearSystemEventEPNS0_15SystemEventTypeE");
    rt_call(c, f, 1, ev);
}
static void ev_destroy(Cpu *c, uint64_t ev) {
    static uint64_t f;
    if (!f) f = os_sym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
    rt_call(c, f, 1, ev);
}

// ---- sync point events (nn::syncpt::sdk): the movie worker waits in a MultiWait for the handle of the picture it
// expects. The SystemEvent is at +0 of the event type; its awaited handle is kept here.
static struct { uint64_t e; int armed; cv_sync want; } g_spt[NSPT];
static PlatMutex g_spt_mu = PLAT_MUTEX_INIT;

static int spt_slot(uint64_t e, int make) {   // g_spt_mu held
    int freei = -1;
    for (int k = 0; k < NSPT; k++) {
        if (g_spt[k].e == e) return k;
        if (!g_spt[k].e && freei < 0) freei = k;
    }
    if (!make) return -1;
    if (freei < 0) rt_fatal("codec: more than %d sync point events", NSPT);
    g_spt[freei].e = e, g_spt[freei].armed = 0;
    return freei;
}
static void spt_disarm(uint64_t e) {
    plat_mutex_lock(&g_spt_mu);
    int k = spt_slot(e, 0);
    if (k >= 0) g_spt[k].armed = 0;
    plat_mutex_unlock(&g_spt_mu);
}
static void spt_fire(const cv_dec *d, uint32_t i) {   // buffer i of d is filled (decoder lock held)
    plat_mutex_lock(&g_spt_mu);
    for (int k = 0; k < NSPT; k++)
        if (g_spt[k].e && g_spt[k].armed && g_spt[k].want.dec == d->id && g_spt[k].want.idx == i &&
            d->b[i].filled_gen >= g_spt[k].want.gen) {
            g_spt[k].armed = 0;
            kern_signal_system_event(g_spt[k].e);
        }
    plat_mutex_unlock(&g_spt_mu);
}
static int reached(const cv_sync *s) {   // a handle the host did not make reads as reached
    if (!s || s->magic != SYNC || !s->gen) return 1;
    int r = 1;
    plat_mutex_lock(&g_mu);   // held until the decoder's lock is: Finalize cannot free it in between
    cv_dec *d = s->dec && s->dec <= NDEC ? g_dec[s->dec - 1] : NULL;
    if (d && s->idx < NBUF) {
        plat_mutex_lock(&d->mu);
        r = d->b[s->idx].filled && d->b[s->idx].filled_gen >= s->gen;
        plat_mutex_unlock(&d->mu);
    }
    plat_mutex_unlock(&g_mu);
    return r;
}
// bool nn::gs::TryWaitNativeSync(const NativeSyncHandle &)
static void ov_gs_trywait(Cpu *c) { ret_x(c, (uint64_t)reached(arg_p(c, 0))); }
// Result CreateSyncPointEvent(SyncPointEventType *)
static void ov_spt_create(Cpu *c) {
    uint64_t e = arg_x(c, 0);
    kern_bind_system_event(c, e, 0);
    plat_mutex_lock(&g_spt_mu);
    spt_slot(e, 1);
    plat_mutex_unlock(&g_spt_mu);
    ret_x(c, 0);
}
// void InitializeMultiWaitHolder(MultiWaitHolderType *, SyncPointEventType *): the holder waits on its SystemEvent
static void ov_spt_holder(Cpu *c) {
    static uint64_t f;
    if (!f) f = os_sym("_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_15SystemEventTypeE");
    rt_call(c, f, 2, arg_x(c, 0), arg_x(c, 1));
}
// Result RequestSyncPointEvent(SyncPointEventType *, const SyncPointHandle &): signalled when the decoder fills the
// awaited buffer, at once when it is already filled
static void ov_spt_request(Cpu *c) {
    uint64_t e = arg_x(c, 0);
    const cv_sync *s = arg_p(c, 1);
    ev_clear(c, e);
    plat_mutex_lock(&g_spt_mu);
    int k = spt_slot(e, 1);
    g_spt[k].want = s ? *s : (cv_sync){0};
    g_spt[k].armed = 1;
    plat_mutex_unlock(&g_spt_mu);
    if (reached(s)) {
        spt_disarm(e);
        kern_signal_system_event(e);
    }
    ret_x(c, 0);
}
static void ov_spt_cancel(Cpu *c) {
    spt_disarm(arg_x(c, 0));
    ev_clear(c, arg_x(c, 0));
}
static void ov_spt_destroy(Cpu *c) {
    uint64_t e = arg_x(c, 0);
    plat_mutex_lock(&g_spt_mu);
    int k = spt_slot(e, 0);
    if (k >= 0) g_spt[k].e = 0, g_spt[k].armed = 0;
    plat_mutex_unlock(&g_spt_mu);
    ev_destroy(c, e);
}

// ---- pictures: I420 to NV12 or RGBA, a smaller picture padded with black, a larger one cropped
static void yuv_nv12(const vpx_image_t *im, uint8_t *dst, uint8_t *uv, int w, int h) {
    int cw = (int)im->d_w < w ? (int)im->d_w : w, ch = (int)im->d_h < h ? (int)im->d_h : h;
    if (cw < w || ch < h) memset(dst, 16, (size_t)w * h), memset(uv, 128, (size_t)w * h / 2);
    for (int y = 0; y < ch; y++) memcpy(dst + (size_t)y * w, im->planes[0] + (size_t)y * im->stride[0], (size_t)cw);
    int cw2 = (cw + 1) / 2, ch2 = (ch + 1) / 2;
    for (int y = 0; y < ch2 && y < h / 2; y++) {
        const uint8_t *u = im->planes[1] + (size_t)y * im->stride[1], *v = im->planes[2] + (size_t)y * im->stride[2];
        uint8_t *o = uv + (size_t)y * w;
        for (int x = 0; x < cw2 && 2 * x + 1 < w; x++) o[2 * x] = u[x], o[2 * x + 1] = v[x];
    }
}
// By the colour space and range the picture declares (when undeclared BT.709 from 720 lines up, else BT.601)
static void yuv_rgba(const vpx_image_t *im, uint8_t *dst, int w, int h) {
    int full = im->range == VPX_CR_FULL_RANGE, b709 = im->cs == VPX_CS_BT_709 || (im->cs == VPX_CS_UNKNOWN && h >= 720);
    double ky = full ? 1.0 : 255.0 / 219.0, kc = full ? 1.0 : 255.0 / 224.0;
    double rv = b709 ? 1.5748 : 1.402, gu = b709 ? 0.1873 : 0.3441, gv = b709 ? 0.4681 : 0.7141, bu = b709 ? 1.8556 : 1.772;
    int Y = (int)(ky * 65536), RV = (int)(rv * kc * 65536), GU = (int)(gu * kc * 65536), GV = (int)(gv * kc * 65536),
        BU = (int)(bu * kc * 65536), y0 = full ? 0 : 16;
    int cw = (int)im->d_w < w ? (int)im->d_w : w, ch = (int)im->d_h < h ? (int)im->d_h : h;
    if (cw < w || ch < h)
        for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
#define CL(v) ((v) < 0 ? 0 : (v) > 255 ? 255 : (v))
    for (int y = 0; y < ch; y++) {
        const uint8_t *py = im->planes[0] + (size_t)y * im->stride[0], *pu = im->planes[1] + (size_t)(y >> 1) * im->stride[1],
                      *pv = im->planes[2] + (size_t)(y >> 1) * im->stride[2];
        uint8_t *o = dst + (size_t)y * w * 4;
        for (int x = 0; x < cw; x++) {
            int l = (py[x] - y0) * Y, u = pu[x >> 1] - 128, v = pv[x >> 1] - 128;
            int r = (l + RV * v + 32768) >> 16, g = (l - GU * u - GV * v + 32768) >> 16, b = (l + BU * u + 32768) >> 16;
            o[4 * x] = (uint8_t)CL(r), o[4 * x + 1] = (uint8_t)CL(g), o[4 * x + 2] = (uint8_t)CL(b), o[4 * x + 3] = 255;
        }
    }
#undef CL
}

// ---- image pools: the host keeps each decoded picture
static void ov_cv_init(Cpu *c) { ret_x(c, 0); }   // nn::codec::video::Initialize / Finalize: the service is the host's
// Result CalculateDecoderImagePoolBuffer(size_t *size, size_t *align, const VideoDecoderImagePreference &, uint32_t n)
static void ov_cv_poolsize(Cpu *c) {
    uint64_t *size = arg_p(c, 0), *align = arg_p(c, 1);
    if (size) *size = 0x1000;
    if (align) *align = 0x1000;
    ret_x(c, 0);
}
// Result InitializeDecoderImagePool(VideoDecoderImagePoolType *, void *mem, size_t, const VideoDecoderImagePreference &,
// uint32_t n)
static void ov_cv_pool(Cpu *c) {
    uint64_t t = arg_x(c, 0);
    const uint8_t *pref = arg_p(c, 3);
    uint32_t n = (uint32_t)arg_x(c, 4), wh;
    memcpy(&wh, pref + 8, 4);
    cv_pool *o = calloc(1, sizeof *o);
    if (!o) rt_fatal("codec: out of memory");
    o->magic = MAGIC, o->w = (uint16_t)((wh & 0xffff) + 1), o->h = (uint16_t)((wh >> 16) + 1), o->n = n ? n : 1;
    o->img = calloc(o->n, sizeof *o->img);
    if (!o->img) rt_fatal("codec: out of memory");
    *(cv_pool **)(uintptr_t)t = o;
    ret_x(c, 0);
}
static void ov_cv_poolfin(Cpu *c) {
    uint64_t t = arg_x(c, 0);
    cv_pool *p = pool_of(t);
    if (!p) return;
    plat_mutex_lock(&g_mu);   // a buffer still registered with this pool keeps no pool to write into
    for (int k = 0; k < NDEC; k++)
        if (g_dec[k]) {
            plat_mutex_lock(&g_dec[k]->mu);
            for (int i = 0; i < NBUF; i++)
                if (g_dec[k]->b[i].pool == p) g_dec[k]->b[i].pool = NULL;
            plat_mutex_unlock(&g_dec[k]->mu);
        }
    plat_mutex_unlock(&g_mu);
    for (uint32_t i = 0; i < p->n; i++)
        if (p->img[i]) vpx_img_free(p->img[i]);
    free(p->img);
    p->magic = 0;
    free(p);
    *(cv_pool **)(uintptr_t)t = NULL;
}
static void ov_cv_native(Cpu *c) {
    static int seen;
    red_once(&seen, "a native image handle was asked for: the GPU route of decoded pictures is not served");
    ret_x(c, 0);
}
static const vpx_image_t *picture(uint64_t t, uint32_t i) {
    const cv_pool *p = pool_of(t);
    return p && i < p->n ? p->img[i] : NULL;
}
static int whole(uint64_t x, uint64_t y) {
    static int seen;
    if (!x && !y) return 1;
    red_once(&seen, "a picture region away from the origin was asked for: only whole pictures are served");
    return 0;
}
// Result ReadDecoderOutputImageRgba(const VideoDecoderImagePoolType *, uint32_t, u16 x, u16 y, u16 w, u16 h, void *dst,
// size_t stride, size_t size)
static void ov_cv_rgba(Cpu *c) {
    const vpx_image_t *im = picture(arg_x(c, 0), (uint32_t)arg_x(c, 1));
    uint16_t w = (uint16_t)arg_x(c, 4), h = (uint16_t)arg_x(c, 5);
    uint8_t *dst = arg_p(c, 6);
    uint64_t stride = arg_x(c, 7), size = arg_x(c, 8);
    if (!im || !whole(arg_x(c, 2) & 0xffff, arg_x(c, 3) & 0xffff) || stride != (uint64_t)w * 4 || size < (uint64_t)w * h * 4)
        memset(dst, 0, size);
    else
        yuv_rgba(im, dst, w, h);
    ret_x(c, 0);
}
// Result ReadDecoderOutputImageNv12(pool, uint32_t, u16 x, u16 y, u16 w, u16 h, void *y, size_t ystride, size_t ysize,
// void *uv, size_t uvstride, size_t uvsize)
static void ov_cv_nv12(Cpu *c) {
    const vpx_image_t *im = picture(arg_x(c, 0), (uint32_t)arg_x(c, 1));
    uint16_t w = (uint16_t)arg_x(c, 4), h = (uint16_t)arg_x(c, 5);
    uint8_t *ydst = arg_p(c, 6), *uvdst = arg_p(c, 9);
    uint64_t ystride = arg_x(c, 7), ysize = arg_x(c, 8), uvstride = arg_x(c, 10), uvsize = arg_x(c, 11);
    uint64_t ny = (uint64_t)w * h;
    if (!im || !whole(arg_x(c, 2) & 0xffff, arg_x(c, 3) & 0xffff) || ystride != w || uvstride != w || ysize < ny ||
        uvsize < ny / 2) {
        memset(ydst, 16, ysize);
        memset(uvdst, 128, uvsize);
    } else {
        yuv_nv12(im, ydst, uvdst, w, h);
    }
    ret_x(c, 0);
}
// ReadDecoderOutputImageRgbaHdr: no HDR video here; the picture reads as black
static void ov_cv_rgba_hdr(Cpu *c) {
    static int seen;
    red_once(&seen, "an HDR picture was asked for: not served (black)");
    memset(arg_p(c, 6), 0, arg_x(c, 8));
    ret_x(c, 0);
}

// ---- decoders
static void deliver(cv_dec *d, Cpu *c) {   // held pictures and a pending end of stream into released buffers (lock held)
    while (d->nwant && (d->nheld || d->eos_pending)) {
        uint32_t i = d->want[0];
        q_drop(d->want, &d->nwant, i);
        typeof(d->b[0]) *b = &d->b[i];
        if (d->nheld) {
            vpx_image_t *im = d->held[0];
            b->ts = d->held_ts[0], b->eos = d->held_eos[0];
            d->nheld--;
            memmove(d->held, d->held + 1, sizeof *d->held * (size_t)d->nheld);
            memmove(d->held_ts, d->held_ts + 1, sizeof *d->held_ts * (size_t)d->nheld);
            memmove(d->held_eos, d->held_eos + 1, (size_t)d->nheld);
            b->w = (uint16_t)im->d_w, b->h = (uint16_t)im->d_h;
            if (b->pool && b->pidx < b->pool->n) {
                if (b->pool->img[b->pidx]) vpx_img_free(b->pool->img[b->pidx]);
                b->pool->img[b->pidx] = im;
            } else {
                vpx_img_free(im);
            }
            if (!d->pictures++) rt_log("codec: VP9 decoder %u: first picture %ux%u", d->id, b->w, b->h);
        } else {
            b->w = b->h = 0, b->ts = 0, b->eos = 1, d->eos_pending = 0;
        }
        b->filled = 1, b->filled_gen = b->gen;
        spt_fire(d, i);
    }
    if (d->nheld < NHOLD) kern_signal_system_event(d->event);
    else ev_clear(c, d->event);
}
static void reopen(cv_dec *d) {   // a new stream: decoding starts again at the next key frame
    if (d->vpx_ok) vpx_codec_destroy(&d->vpx);
    vpx_codec_dec_cfg_t cfg = {.threads = 4};
    d->vpx_ok = vpx_codec_dec_init(&d->vpx, vpx_codec_vp9_dx(), &cfg, 0) == VPX_CODEC_OK;
    if (!d->vpx_ok) rt_log("codec: libvpx would not start a VP9 decoder");
}
static uint32_t dec_init(Cpu *c, uint64_t t, unsigned w, unsigned h) {
    cv_dec *d = calloc(1, sizeof *d);
    if (!d) rt_fatal("codec: out of memory");
    d->magic = MAGIC;
    plat_mutex_init(&d->mu);
    reopen(d);
    plat_mutex_lock(&g_mu);
    for (int i = 0; i < NDEC && !d->id; i++)
        if (!g_dec[i]) g_dec[i] = d, d->id = (uint32_t)i + 1;
    plat_mutex_unlock(&g_mu);
    if (!d->id) rt_fatal("codec: more than %d video decoders", NDEC);
    d->event = t + EVENT_OFF;
    kern_bind_system_event(c, d->event, 0);
    kern_signal_system_event(d->event);
    *(cv_dec **)(uintptr_t)t = d;
    rt_log("codec: VP9 decoder %u: %ux%u, decoded with libvpx on the host", d->id, w, h);
    return 0;
}
// Result InitializeVp9DecoderByTransferMemory(Vp9DecoderType *, void *, size_t, size_t, u16 w, u16 h, uint32_t, ...)
static void ov_vp9_init_tm(Cpu *c) { ret_x(c, dec_init(c, arg_x(c, 0), (unsigned)(arg_x(c, 4) & 0xffff), (unsigned)(arg_x(c, 5) & 0xffff))); }
// Result InitializeVp9DecoderByRemoteMemory(Vp9DecoderType *, size_t, u16 w, u16 h, uint32_t, ...)
static void ov_vp9_init_rm(Cpu *c) { ret_x(c, dec_init(c, arg_x(c, 0), (unsigned)(arg_x(c, 2) & 0xffff), (unsigned)(arg_x(c, 3) & 0xffff))); }
static void ov_vp9_fin(Cpu *c) {
    uint64_t t = arg_x(c, 0);
    cv_dec *d = dec_of(t);
    if (!d) { ret_x(c, 0); return; }
    plat_mutex_lock(&g_mu);
    g_dec[d->id - 1] = NULL;
    plat_mutex_lock(&d->mu);
    plat_mutex_unlock(&d->mu);
    plat_mutex_unlock(&g_mu);
    if (d->vpx_ok) vpx_codec_destroy(&d->vpx);
    for (int i = 0; i < d->nheld; i++) vpx_img_free(d->held[i]);
    ev_destroy(c, d->event);   // the event goes with the decoder, as in the SDK
    d->magic = 0;
    free(d);
    *(cv_dec **)(uintptr_t)t = NULL;
    ret_x(c, 0);
}
// Result RegisterVp9DecoderBuffer(Vp9DecoderType *, uint32_t index, VideoDecoderImagePoolType *, uint32_t pool index)
static void ov_vp9_reg(Cpu *c) {
    cv_dec *d = dec_of(arg_x(c, 0));
    uint32_t i = (uint32_t)arg_x(c, 1);
    if (!d || i >= NBUF) { ret_x(c, CV_R(CV_BAD)); return; }
    plat_mutex_lock(&d->mu);
    d->b[i].pool = pool_of(arg_x(c, 2)), d->b[i].pidx = (uint32_t)arg_x(c, 3), d->b[i].reg = 1, d->b[i].filled = 0;
    d->b[i].gen = d->b[i].filled_gen = 0;
    if (d->nacq < NQ) d->acq[d->nacq++] = i;
    else rt_log("codec: acquirable queue full at register %u", i);
    plat_mutex_unlock(&d->mu);
    ret_x(c, 0);
}
static void ov_vp9_start(Cpu *c) { ret_x(c, 0); }   // the host decoder decodes on submission
// Stop / Flush: undelivered pictures and a pending end of stream are dropped
static void ov_vp9_reset(Cpu *c) {
    cv_dec *d = dec_of(arg_x(c, 0));
    if (d) {
        plat_mutex_lock(&d->mu);
        for (int i = 0; i < d->nheld; i++) vpx_img_free(d->held[i]);
        d->nheld = d->eos_pending = 0;
        reopen(d);
        kern_signal_system_event(d->event);
        plat_mutex_unlock(&d->mu);
    }
    ret_x(c, 0);
}
// Result SubmitVp9DecoderInputBuffer(uint64_t *out, Vp9DecoderType *, const void *data, size_t size, TimeSpan, bool eos)
static void ov_vp9_submit(Cpu *c) {
    uint64_t *out = arg_p(c, 0);
    cv_dec *d = dec_of(arg_x(c, 1));
    const void *data = arg_p(c, 2);
    uint64_t size = arg_x(c, 3);
    int64_t ts = (int64_t)arg_x(c, 4);
    int eos = (int)(arg_x(c, 5) & 0xff);
    if (!d) { ret_x(c, CV_R(CV_BAD)); return; }
    plat_mutex_lock(&d->mu);
    if (d->nheld >= NHOLD) {
        ev_clear(c, d->event);
        plat_mutex_unlock(&d->mu);
        ret_x(c, CV_R(CV_BUSY));
        return;
    }
    int got = 0;
    if (size && d->vpx_ok) {
        if (vpx_codec_decode(&d->vpx, data, (unsigned)size, NULL, 0) != VPX_CODEC_OK) {
            static int n;
            if (n++ < 4) rt_log("codec: libvpx: %s", vpx_codec_error(&d->vpx));
        }
        vpx_codec_iter_t it = NULL;
        const vpx_image_t *im;
        while ((im = vpx_codec_get_frame(&d->vpx, &it))) {   // an owned copy: libvpx reuses its buffers
            if (d->nheld >= NHOLD) {
                static int n;
                if (n++ < 4) rt_log("codec: a packet gave more pictures than the host holds: one dropped");
                continue;
            }
            vpx_image_t *cp = vpx_img_alloc(NULL, VPX_IMG_FMT_I420, im->d_w, im->d_h, 1);
            if (!cp) continue;
            cp->cs = im->cs, cp->range = im->range;
            for (int p = 0; p < 3; p++)
                for (unsigned y = 0, ph = p ? (im->d_h + 1) / 2 : im->d_h; y < ph; y++)
                    memcpy(cp->planes[p] + (size_t)y * cp->stride[p], im->planes[p] + (size_t)y * im->stride[p],
                           p ? (im->d_w + 1) / 2 : im->d_w);
            d->held[d->nheld] = cp, d->held_ts[d->nheld] = ts, d->held_eos[d->nheld++] = 0;
            got++;
        }
    }
    if (eos) {
        if (got) d->held_eos[d->nheld - 1] = 1;
        else d->eos_pending = 1;
    }
    if (out) *out = ++d->submits;
    deliver(d, c);
    plat_mutex_unlock(&d->mu);
    ret_x(c, 0);
}
// Result AcquireVp9DecoderOutputBuffer(uint32_t *index, nn::gs::NativeSyncHandle *, Vp9DecoderType *)
static void ov_vp9_acq(Cpu *c) {
    uint32_t *idx = arg_p(c, 0);
    cv_sync *sync = arg_p(c, 1);
    cv_dec *d = dec_of(arg_x(c, 2));
    if (!d) { ret_x(c, CV_R(CV_BAD)); return; }
    plat_mutex_lock(&d->mu);
    uint32_t r = d->nacq ? 0 : CV_R(CV_EMPTY);
    if (!r) {
        uint32_t i = d->acq[0];
        q_drop(d->acq, &d->nacq, i);
        *idx = i;
        *sync = (cv_sync){SYNC, d->id, i, d->b[i].gen};
    }
    plat_mutex_unlock(&d->mu);
    ret_x(c, r);
}
// void ReleaseVp9DecoderOutputBuffer(Vp9DecoderType *, uint32_t index, nn::gs::NativeSyncHandle)
static void ov_vp9_rel(Cpu *c) {
    cv_dec *d = dec_of(arg_x(c, 0));
    uint32_t i = (uint32_t)arg_x(c, 1);
    if (!d || i >= NBUF || !d->b[i].reg) {
        rt_log("codec: release of buffer %u, which is not registered", i);
        return;
    }
    plat_mutex_lock(&d->mu);
    d->b[i].gen++, d->b[i].filled = 0;
    if (d->nwant < NBUF) d->want[d->nwant++] = i;
    if (d->nacq < NQ) d->acq[d->nacq++] = i;
    else rt_log("codec: acquirable queue full at release %u", i);
    deliver(d, c);
    plat_mutex_unlock(&d->mu);
}
// Result GetVp9DecoderOutputBufferProperties(u16 *w, u16 *h, u16 *cw, u16 *ch, TimeSpan *, bool *eos, Vp9DecoderType *,
// uint32_t index)
static void ov_vp9_props(Cpu *c) {
    uint16_t *w = arg_p(c, 0), *h = arg_p(c, 1), *cw = arg_p(c, 2), *ch = arg_p(c, 3);
    int64_t *ts = arg_p(c, 4);
    uint8_t *eos = arg_p(c, 5);
    cv_dec *d = dec_of(arg_x(c, 6));
    uint32_t i = (uint32_t)arg_x(c, 7);
    if (!d || i >= NBUF) { ret_x(c, CV_R(CV_BAD)); return; }
    plat_mutex_lock(&d->mu);
    int ok = d->b[i].filled;
    if (ok) *w = *cw = d->b[i].w, *h = *ch = d->b[i].h, *ts = d->b[i].ts, *eos = d->b[i].eos;
    plat_mutex_unlock(&d->mu);
    ret_x(c, ok ? 0 : CV_R(CV_EMPTY));
}

HOST_OVERRIDE("_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE", ov_gs_trywait);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk20CreateSyncPointEventEPNS1_18SyncPointEventTypeE", ov_spt_create);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk25InitializeMultiWaitHolderEPNS_2os19MultiWaitHolderTypeEPNS1_18SyncPointEventTypeE",
              ov_spt_holder);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk21RequestSyncPointEventEPNS1_18SyncPointEventTypeERKNS1_15SyncPointHandleE",
              ov_spt_request);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk20CancelSyncPointEventEPNS1_18SyncPointEventTypeE", ov_spt_cancel);
HOST_OVERRIDE("_ZN2nn6syncpt3sdk21DestroySyncPointEventEPNS1_18SyncPointEventTypeE", ov_spt_destroy);
HOST_OVERRIDE("_ZN2nn5codec5video10InitializeEv", ov_cv_init);
HOST_OVERRIDE("_ZN2nn5codec5video8FinalizeEv", ov_cv_init);
HOST_OVERRIDE("_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj", ov_cv_poolsize);
HOST_OVERRIDE("_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEj",
              ov_cv_pool);
HOST_OVERRIDE("_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE", ov_cv_poolfin);
HOST_OVERRIDE("_ZN2nn5codec36GetDecoderImagePoolNativeImageHandleEPKNS0_25VideoDecoderImagePoolTypeEj", ov_cv_native);
HOST_OVERRIDE("_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", ov_cv_rgba);
HOST_OVERRIDE("_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm", ov_cv_nv12);
HOST_OVERRIDE("_ZN2nn5codec29ReadDecoderOutputImageRgbaHdrEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", ov_cv_rgba_hdr);
HOST_OVERRIDE("_ZN2nn5codec36InitializeVp9DecoderByTransferMemoryEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
              ov_vp9_init_tm);
HOST_OVERRIDE("_ZN2nn5codec34InitializeVp9DecoderByRemoteMemoryEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
              ov_vp9_init_rm);
HOST_OVERRIDE("_ZN2nn5codec18FinalizeVp9DecoderEPNS0_14Vp9DecoderTypeE", ov_vp9_fin);
HOST_OVERRIDE("_ZN2nn5codec24RegisterVp9DecoderBufferEPNS0_14Vp9DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", ov_vp9_reg);
HOST_OVERRIDE("_ZN2nn5codec15StartVp9DecoderEPNS0_14Vp9DecoderTypeE", ov_vp9_start);
HOST_OVERRIDE("_ZN2nn5codec14StopVp9DecoderEPNS0_14Vp9DecoderTypeE", ov_vp9_reset);
HOST_OVERRIDE("_ZN2nn5codec15FlushVp9DecoderEPNS0_14Vp9DecoderTypeE", ov_vp9_reset);
HOST_OVERRIDE("_ZN2nn5codec27SubmitVp9DecoderInputBufferEPmPNS0_14Vp9DecoderTypeEPKvmNS_8TimeSpanEb", ov_vp9_submit);
HOST_OVERRIDE("_ZN2nn5codec29AcquireVp9DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Vp9DecoderTypeE", ov_vp9_acq);
HOST_OVERRIDE("_ZN2nn5codec29ReleaseVp9DecoderOutputBufferEPNS0_14Vp9DecoderTypeEjNS_2gs16NativeSyncHandleE", ov_vp9_rel);
HOST_OVERRIDE("_ZN2nn5codec35GetVp9DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Vp9DecoderTypeEj",
              ov_vp9_props);
#endif
