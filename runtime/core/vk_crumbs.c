// GPU breadcrumbs for a lost device (vk.h vk_crumb). VK_AMD_buffer_marker writes a 32-bit code into a buffer from
// the command stream: at TOP_OF_PIPE once the command processor reaches the marker, at BOTTOM_OF_PIPE once every
// command before it has finished. Each crumb writes both, into two words of one host-coherent buffer, so after a
// device loss word 0 names the newest point the GPU reached and word 1 the newest point it had finished: the hang lies
// between them. A code is the crumb kind in the top 4 bits and its number in the low 28. The markers cost two small
// writes per crumb, and crumbs mark submissions, upload batches, ASTC chunks and presents, not draws.
//
// Pass marks (vk.h vk_pass_mark_begin) use words 2 and 3 of the same buffer: the replay marks the start and the end of
// every render pass and dispatch, and a ring of records says what each mark was. The design follows Savage Wind
// Recomp's GPU checkpoints (refs/SavageWindRecomp runtime/nvn/vulkan/exec.c ck_mark and vk_checkpoints_report: a mark
// per pass and dispatch, numbers in a ring of 4096, text only at the device loss); the code is ours.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "nvn_api.h"
#include "platform/platform.h"
#include "vk.h"

static PFN_vkCmdWriteBufferMarkerAMD write_marker;
static VkBuffer buf;
static VmaAllocation alloc;
static volatile uint32_t *words;   // 0, 1: crumbs; 2, 3: pass marks

// Pass marks (below)
int vk_pass_marks_on;
static int pm_amd;                          // marks into words 2 and 3
static PFN_vkCmdSetCheckpointNV set_checkpoint;
static void pass_marks_init(VkCtx *c);
static void pt_release(VkCtx *c);   // pass timing (below)

static void buffer_init(VkCtx *c) {
    write_marker = (PFN_vkCmdWriteBufferMarkerAMD)vkGetDeviceProcAddr(c->device, "vkCmdWriteBufferMarkerAMD");
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 16,
                             .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                  .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT,
                                  .requiredFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT};
    VmaAllocationInfo info;
    if (!write_marker || vmaCreateBuffer(c->vma, &bi, &ai, &buf, &alloc, &info) != VK_SUCCESS) {
        rt_log("vk: GPU breadcrumbs unavailable");
        write_marker = NULL;
        return;
    }
    words = info.pMappedData;
    words[0] = words[1] = words[2] = words[3] = 0;
    rt_log("vk: GPU breadcrumbs on (VK_AMD_buffer_marker)");
}

void vk_crumbs_init(VkCtx *c) {
    if (c->has_buffer_marker) buffer_init(c);
    pass_marks_init(c);
}

void vk_crumbs_release(void) {
    VkCtx *c = vk_ctx();
    if (buf && c) vmaDestroyBuffer(c->vma, buf, alloc);
    buf = VK_NULL_HANDLE, alloc = NULL, words = NULL, write_marker = NULL;
    vk_pass_marks_on = pm_amd = 0, set_checkpoint = NULL;
    pt_release(c);
}

void vk_crumb(VkCommandBuffer cb, int kind, uint64_t n) {
    if (!write_marker || !cb) return;
    uint32_t code = (uint32_t)kind << 28 | (uint32_t)(n & 0x0fffffff);
    write_marker(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, buf, 0, code);
    write_marker(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, buf, 4, code);
}

static void describe(uint32_t code, char *o, size_t size) {
    static const char *const kinds[] = {"nothing", "the start of a part of submission", "the end of submission",
                                        "the start of upload batch", "the end of upload batch",
                                        "ASTC decode chunk", "the start of present",
                                        "the end of present"};
    unsigned k = code >> 28;
    if (!code) snprintf(o, size, "nothing yet");
    else if (k < sizeof kinds / sizeof *kinds) snprintf(o, size, "%s %u", kinds[k], code & 0x0fffffff);
    else snprintf(o, size, "unknown code %#x", code);
}

void vk_crumbs_report(void) {
    if (!words) return;
    char reached[96], finished[96];
    describe(words[0], reached, sizeof reached);
    describe(words[1], finished, sizeof finished);
    rt_log("vk: GPU breadcrumbs: the GPU last reached %s; everything before %s had finished", reached, finished);
}

// ---- pass marks

// Cost per pass or dispatch: about twenty stores into the ring and two commands, one at the start and one at the end,
// each a marker write or a checkpoint. Per draw nothing, except that the branch of vk_exec_draw.c prepare_draw that
// records changed pipeline state also notes the shaders of a pass's first draw. A full barrier already precedes every
// pass and dispatch (vk_exec.c vk_exec_pass_begin, vk_exec_draw.c op_dispatch), so the end-of-pipe write adds no wait.
static VkPassMark ring[VK_PASS_MARKS];
static uint32_t seq;                        // marks made; only the replay thread writes it

// ---- pass timing (SR_PASS_TIMING=1, sr-recomp): the GPU time of every render pass and dispatch, from a timestamp at
// its start mark (all earlier commands done; a full barrier is already there) and one at its end mark (bottom of
// pipe), into a query pair per ring slot. The replay thread reads the marks whose submission has finished
// (vk_sync_fence_done), in batches, and adds each to a group: kind, size, attachment formats and the first draw's
// shaders (a dispatch: its compute shader). Every 5 s the log lists the groups that cost the most, in ms a frame.
// Copies, clears, blits, DLSS, the present and frame interpolation lie outside the marks: the line says how much of
// the time the marks covered. Diagnostics only: off, no pool and nothing recorded.
static int pt_on;
static VkQueryPool pt_pool;
static double pt_period;                    // ns per tick
static uint64_t pt_mask;
static PFN_vkCmdWriteTimestamp pt_write;    // the device's own entry points: vk_rec.h's macros are for the replay
static PFN_vkCmdResetQueryPool pt_reset;
static uint32_t pt_read;                    // replay thread: the newest mark read (or given up)
typedef struct {
    uint8_t kind, ncolor;
    uint32_t w, h, d;
    VkFormat c0, depth;
    const GfxShader *a, *b;                 // first draw's vs and fs; a dispatch: cs, NULL
    uint64_t ns, max_ns, n, draws, gap_ns, by_barrier;   // by_barrier: passes the game's own barrier ended
    uint32_t barrier_bits;              // those barriers' NVNbarrierBits, together
    const void *closer[2];              // the two calls that ended its passes most, and how often
    uint64_t closer_n[2];   // gap_ns: GPU time since the previous mark of its submission ended
} PtGroup;
enum { PT_GROUPS = 1024 };
static PtGroup pt_g[PT_GROUPS];
static int pt_ng;
static uint64_t pt_t0, pt_f0, pt_ns, pt_gap_ns, pt_marks, pt_lost, pt_over;
static uint64_t pt_prev_end, pt_prev_fence;   // replay thread: the previous mark read, for the gap before the next

static void pt_init(VkCtx *c) {
    if (!cfg_bool("SR_PASS_TIMING")) return;
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(c->phys, &nq, qp);
    const uint32_t bits = c->gfx_family < nq ? qp[c->gfx_family].timestampValidBits : 0;
    pt_write = (PFN_vkCmdWriteTimestamp)vkGetDeviceProcAddr(c->device, "vkCmdWriteTimestamp");
    pt_reset = (PFN_vkCmdResetQueryPool)vkGetDeviceProcAddr(c->device, "vkCmdResetQueryPool");
    VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                .queryCount = 2 * VK_PASS_MARKS};
    if (!bits || c->props.limits.timestampPeriod <= 0 || !pt_write || !pt_reset ||
        vkCreateQueryPool(c->device, &qi, NULL, &pt_pool) != VK_SUCCESS) {
        rt_log("vk: pass timing (SR_PASS_TIMING) unavailable: no timestamps on the graphics queue");
        return;
    }
    pt_period = c->props.limits.timestampPeriod;
    pt_mask = bits >= 64 ? ~0ull : (1ull << bits) - 1;
    pt_on = 1;
    rt_log("vk: pass timing on (SR_PASS_TIMING): every 5 s the render passes and dispatches that cost the GPU most");
}

static void pt_release(VkCtx *c) {
    if (pt_pool && c) vkDestroyQueryPool(c->device, pt_pool, NULL);
    pt_pool = VK_NULL_HANDLE, pt_on = 0;
}

static PtGroup *pt_group(const VkPassMark *m) {
    const GfxShader *a = m->first[0], *b = m->kind == VK_MARK_PASS ? m->first[1] : NULL;
    const VkFormat c0 = m->ncolor ? m->color[0] : VK_FORMAT_UNDEFINED;
    const uint32_t w = m->kind == VK_MARK_PASS ? m->w : 0, h = m->kind == VK_MARK_PASS ? m->h : 0;   // dispatch sizes vary
    for (int i = 0; i < pt_ng; i++) {
        PtGroup *g = &pt_g[i];
        if (g->kind == m->kind && g->w == w && g->h == h && g->c0 == c0 && g->depth == m->depth && g->a == a && g->b == b)
            return g;
    }
    if (pt_ng == PT_GROUPS) return NULL;
    PtGroup *g = &pt_g[pt_ng++];
    *g = (PtGroup){.kind = m->kind, .ncolor = m->ncolor, .w = w, .h = h, .d = m->kind == VK_MARK_PASS ? m->d : 0,
                   .c0 = c0, .depth = m->depth, .a = a, .b = b};
    return g;
}

static int pt_cmp(const void *x, const void *y) {
    const uint64_t a = ((const PtGroup *)x)->ns + ((const PtGroup *)x)->gap_ns, b = ((const PtGroup *)y)->ns + ((const PtGroup *)y)->gap_ns;
    return a < b ? 1 : a > b ? -1 : 0;
}

static unsigned long long shader_id(const GfxShader *s);

// An address in sr.exe as an offset from where it loaded (addr2line wants the image base added back).
#ifdef _WIN32
extern char __ImageBase;
static unsigned long long pt_rva(const void *p) { return p ? (unsigned long long)((uintptr_t)p - (uintptr_t)&__ImageBase) : 0; }
#else
static unsigned long long pt_rva(const void *p) { return (unsigned long long)(uintptr_t)p; }
#endif

static void pt_report(uint64_t now) {
    const uint64_t frames = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    const double f = frames > pt_f0 ? (double)(frames - pt_f0) : 1.0;
    qsort(pt_g, (size_t)pt_ng, sizeof *pt_g, pt_cmp);
    rt_log("vk: pass timing: %llu marks over %.0f frames in %.1f s, %.2f ms of GPU a frame inside them and %.2f ms between "
           "them within a submission (barriers, copies, clears; %llu unread, %llu over the group table); the most costly, "
           "inside + before:",
           (unsigned long long)pt_marks, f, (double)(now - pt_t0) / 1e9, (double)pt_ns / 1e6 / f, (double)pt_gap_ns / 1e6 / f,
           (unsigned long long)pt_lost, (unsigned long long)pt_over);
    for (int i = 0; i < pt_ng && i < 14; i++) {
        const PtGroup *g = &pt_g[i];
        if (g->kind == VK_MARK_DISPATCH)
            rt_log("vk:   %5.2f + %5.2f ms a frame, %5.1f a frame, longest %5.2f ms: dispatch cs %016llx",
                   (double)g->ns / 1e6 / f, (double)g->gap_ns / 1e6 / f, (double)g->n / f, (double)g->max_ns / 1e6,
                   shader_id(g->a));
        else
            rt_log("vk:   %5.2f + %5.2f ms a frame, %5.1f a frame, longest %5.2f ms: pass %ux%u x%u, %u colour (first "
                   "format %d), depth format %d, %.0f draws a frame, %.0f%% ended by the game's barrier (bits %#x), ended at "
                   "rva %#llx (%.0f%%) %#llx (%.0f%%); first vs %016llx fs %016llx",
                   (double)g->ns / 1e6 / f, (double)g->gap_ns / 1e6 / f, (double)g->n / f,
                   (double)g->max_ns / 1e6, g->w, g->h, g->d, g->ncolor, (int)g->c0, (int)g->depth, (double)g->draws / f,
                   g->n ? 100.0 * (double)g->by_barrier / (double)g->n : 0.0, g->barrier_bits, pt_rva(g->closer[0]),
                   g->n ? 100.0 * (double)g->closer_n[0] / (double)g->n : 0.0, pt_rva(g->closer[1]),
                   g->n ? 100.0 * (double)g->closer_n[1] / (double)g->n : 0.0,
                   shader_id(g->a), shader_id(g->b));
    }
    pt_ng = 0, pt_ns = pt_gap_ns = pt_marks = pt_lost = pt_over = 0, pt_t0 = now, pt_f0 = frames;
}

// Reads the finished marks, in runs of consecutive slots, up to the first whose submission is still running.
static void pt_read_marks(void) {
    const uint64_t done = vk_sync_fence_done();
    VkDevice dev = vk_ctx()->device;
    static uint64_t v[2 * 2 * 256];         // value and availability per query
    while (pt_read != seq) {
        // the run: consecutive marks still in the ring, their submission finished, within one stretch of slots
        uint32_t first = pt_read + 1, n = 0;
        if (!first) first = 1;
        while (n < 256 && first + n <= seq) {
            const uint32_t id = first + n;
            const VkPassMark *m = &ring[id & (VK_PASS_MARKS - 1)];
            if (m->id != id || m->fence > done) break;
            if (n && (id & (VK_PASS_MARKS - 1)) == 0) break;   // the ring wraps: a new run
            n++;
        }
        if (!n) {
            const VkPassMark *m = &ring[first & (VK_PASS_MARKS - 1)];
            if (m->id == first) return;     // not finished yet: later
            pt_lost++, pt_read = first;     // a later mark took its slot
            continue;
        }
        const uint32_t slot = first & (VK_PASS_MARKS - 1);
        const VkResult r = vkGetQueryPoolResults(dev, pt_pool, 2 * slot, 2 * n, sizeof(uint64_t) * 4 * n, v,
                                                 2 * sizeof(uint64_t), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        for (uint32_t i = 0; i < n; i++) {
            const uint64_t *s = &v[4 * i], *e = &v[4 * i + 2];
            if ((r != VK_SUCCESS && r != VK_NOT_READY) || !s[1] || !e[1]) { pt_lost++, pt_prev_fence = 0; continue; }
            const uint64_t a = s[0] & pt_mask, b = e[0] & pt_mask;
            if (b < a) { pt_lost++, pt_prev_fence = 0; continue; }
            const uint64_t ns = (uint64_t)((double)(b - a) * pt_period);
            const VkPassMark *m = &ring[(first + i) & (VK_PASS_MARKS - 1)];
            // the GPU time since the previous mark of the same submission ended: the barrier before this one, and
            // whatever copies or clears were recorded between them
            const uint64_t gap = pt_prev_fence == m->fence && a >= pt_prev_end ? (uint64_t)((double)(a - pt_prev_end) * pt_period) : 0;
            pt_prev_end = b, pt_prev_fence = m->fence;
            PtGroup *g = pt_group(m);
            pt_ns += ns, pt_gap_ns += gap, pt_marks++;
            if (!g) { pt_over++; continue; }
            g->ns += ns, g->gap_ns += gap, g->n++;
            if (m->closed_by) g->by_barrier++, g->barrier_bits |= m->closed_by & 0x7fffffffu;
            if (m->kind == VK_MARK_PASS && m->closer) {
                if (g->closer[0] == m->closer) g->closer_n[0]++;
                else if (g->closer[1] == m->closer) g->closer_n[1]++;
                else if (!g->closer[0]) g->closer[0] = m->closer, g->closer_n[0] = 1;
                else if (!g->closer[1] || g->closer_n[1] < 2) g->closer[1] = m->closer, g->closer_n[1] = 1;
                if (g->closer_n[1] > g->closer_n[0]) {
                    const void *t = g->closer[0]; g->closer[0] = g->closer[1], g->closer[1] = t;
                    const uint64_t u = g->closer_n[0]; g->closer_n[0] = g->closer_n[1], g->closer_n[1] = u;
                }
            }
            if (ns > g->max_ns) g->max_ns = ns;
            if (m->kind == VK_MARK_PASS && m->draws != UINT32_MAX) g->draws += m->draws;
        }
        pt_read = first + n - 1;
    }
}

// Replay thread, at each new mark: reads once 256 marks have gathered, and reports every 5 s.
static void pt_harvest(void) {
    if (seq - pt_read < 256) return;
    pt_read_marks();
    const uint64_t now = plat_now_ns();
    if (!pt_t0) pt_t0 = now, pt_f0 = __atomic_load_n(&gfx_counters.frames, __ATOMIC_RELAXED);
    else if (now - pt_t0 >= 5000000000ull) pt_report(now);
}

// SR_PASS_CRUMBS: unset, buffer marks where VK_AMD_buffer_marker exists; 1, also NVIDIA checkpoints; 0, neither.
int vk_pass_marks_want_nv(void) {
    const char *e = cfg_get("SR_PASS_CRUMBS");
    return e && *e && strcmp(e, "0") != 0;
}

static void pass_marks_init(VkCtx *c) {
    const char *e = cfg_get("SR_PASS_CRUMBS");
    int off = e && !strcmp(e, "0");
    set_checkpoint = c->has_nv_checkpoints
                         ? (PFN_vkCmdSetCheckpointNV)vkGetDeviceProcAddr(c->device, "vkCmdSetCheckpointNV") : NULL;
    pm_amd = !off && write_marker && words;
    pt_init(c);
    vk_pass_marks_on = pm_amd || set_checkpoint || pt_on;
    if (pt_on && !pm_amd && !set_checkpoint) return;   // marks for timing only
    if (pm_amd || set_checkpoint)
        rt_log("vk: pass marks on (%s%s%s; SR_PASS_CRUMBS=0 turns them off%s)", pm_amd ? "VK_AMD_buffer_marker" : "",
               pm_amd && set_checkpoint ? " and " : "", set_checkpoint ? "VK_NV_device_diagnostic_checkpoints" : "",
               set_checkpoint ? "" : ", =1 adds NVIDIA checkpoints where the driver has them");
    else
        rt_log("vk: pass marks off (%s)", off ? "SR_PASS_CRUMBS=0"
                                              : "no VK_AMD_buffer_marker; SR_PASS_CRUMBS=1 asks for NVIDIA checkpoints");
}

void vk_pass_mark_cmd(VkCommandBuffer cb, uint32_t id, int end) {
    if (pm_amd)
        write_marker(cb, end ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, buf,
                     end ? 12 : 8, id);
    if (set_checkpoint) set_checkpoint(cb, (const void *)(uintptr_t)((uint64_t)id << 1 | (end != 0)));
    if (pt_on) {   // outside a render pass at both marks (vk_exec.c vk_exec_pass_begin / _end, op_dispatch)
        const uint32_t q = 2 * (id & (VK_PASS_MARKS - 1));
        if (!end) {
            pt_reset(cb, pt_pool, q, 2);
            pt_write(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, pt_pool, q);
        } else
            pt_write(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pt_pool, q + 1);
    }
}

VkPassMark *vk_pass_mark_begin(VkCommandBuffer cb, int kind, uint64_t fence) {
    if (!vk_pass_marks_on || !cb) return NULL;
    if (pt_on) pt_harvest();
    uint32_t id = ++seq;
    if (!id) id = ++seq;   // 0 means no mark
    VkPassMark *m = &ring[id & (VK_PASS_MARKS - 1)];
    m->id = id, m->kind = (uint8_t)kind, m->fence = fence, m->draws = UINT32_MAX, m->closed_by = 0, m->closer = NULL;
    rec_pass_mark(cb, id, 0);
    return m;
}

void vk_pass_mark_end(VkCommandBuffer cb, const VkPassMark *m) {
    if (m && cb) rec_pass_mark(cb, m->id, 1);
}

int vk_pass_marks_read(uint32_t *started, uint32_t *finished) {
    if (!pm_amd) return 0;
    *started = words[2], *finished = words[3];
    return 1;
}

const VkPassMark *vk_pass_mark_record(uint32_t id) {
    const VkPassMark *m = &ring[id & (VK_PASS_MARKS - 1)];
    return id && m->id == id ? m : NULL;
}

// The first 8 bytes of the shader's input id (gfx_shader.c), the start of its stored input's name <cache>/in/<id>.in.
static unsigned long long shader_id(const GfxShader *s) {
    if (!s) return 0;
    const uint8_t *iid = gfx_shader_input_id(s);
    unsigned long long v = 0;
    for (int i = 0; i < 8; i++) v = v << 8 | iid[i];
    return v;
}

static void describe_mark(uint32_t id, char *o, size_t size) {
    const VkPassMark *m = vk_pass_mark_record(id);
    if (!id) {
        snprintf(o, size, "nothing yet");
        return;
    }
    if (!m) {
        snprintf(o, size, "no record (a later mark took its slot)");
        return;
    }
    if (m->kind == VK_MARK_DISPATCH) {
        snprintf(o, size, "dispatch %u of submission %llu: %ux%ux%u groups%s, cs %016llx", m->step,
                 (unsigned long long)m->fence, m->w, m->h, m->d, m->w ? "" : " (indirect)", shader_id(m->first[0]));
        return;
    }
    size_t n = (size_t)snprintf(o, size, "pass %u of submission %llu: %ux%u, %u layer%s, colour formats", m->step,
                                (unsigned long long)m->fence, m->w, m->h, m->d, m->d == 1 ? "" : "s");
    for (int i = 0; i < m->ncolor && i < 8 && n < size; i++)
        n += (size_t)snprintf(o + n, size - n, "%s%d", i ? "," : " ", (int)m->color[i]);
    if (!m->ncolor && n < size) n += (size_t)snprintf(o + n, size - n, " none");
    if (n < size) n += (size_t)snprintf(o + n, size - n, ", depth format %d", (int)m->depth);
    if (n >= size) return;
    if (m->draws == UINT32_MAX)
        snprintf(o + n, size - n, ", still recording; first draw vs %016llx fs %016llx", shader_id(m->first[0]),
                 shader_id(m->first[1]));
    else if (!m->draws)
        snprintf(o + n, size - n, ", no draws");
    else
        snprintf(o + n, size - n, ", %u draws; first vs %016llx fs %016llx, last vs %016llx fs %016llx", m->draws,
                 shader_id(m->first[0]), shader_id(m->first[1]), shader_id(m->last[0]), shader_id(m->last[1]));
}

void vk_pass_marks_format(const char *source, uint32_t started, uint32_t finished, char *o, size_t size) {
    char a[640], b[640];
    describe_mark(started, a, sizeof a);
    describe_mark(finished, b, sizeof b);
    snprintf(o, size,
             "vk: pass marks (%s, %u made): the GPU last started mark %u and last finished mark %u\n"
             "vk:   last started %u: %s\n"
             "vk:   last finished %u: %s",
             source, (unsigned)seq, (unsigned)started, (unsigned)finished, (unsigned)started, a, (unsigned)finished, b);
}

static void log_lines(char *t) {
    for (char *line = t, *nl; line && *line; line = nl ? nl + 1 : NULL) {
        nl = strchr(line, '\n');
        if (nl) *nl = 0;
        rt_log("%s", line);
    }
}

// vkGetQueueCheckpointData2NV, else vkGetQueueCheckpointDataNV, on the graphics queue (the replay's): one entry per
// stage with the last checkpoint that stage executed. A checkpoint is the mark's id times 2, plus 1 at its end, so the
// top-of-pipe entry names the last start or end the GPU reached, and the bottom-of-pipe entry the last one before which
// everything had finished: at a start, that is the end of the mark before it.
static void nv_report(void) {
    VkCtx *c = vk_ctx();
    if (!c || !set_checkpoint) return;
    PFN_vkGetQueueCheckpointData2NV get2 =
        (PFN_vkGetQueueCheckpointData2NV)vkGetDeviceProcAddr(c->device, "vkGetQueueCheckpointData2NV");
    PFN_vkGetQueueCheckpointDataNV get1 =
        (PFN_vkGetQueueCheckpointDataNV)vkGetDeviceProcAddr(c->device, "vkGetQueueCheckpointDataNV");
    enum { MAXN = 32 };
    uint64_t stage[MAXN], marker[MAXN];
    uint32_t n = 0;
    if (get2) {
        VkCheckpointData2NV d[MAXN];
        get2(c->gfx_queue, &n, NULL);
        if (n > MAXN) n = MAXN;
        for (uint32_t i = 0; i < n; i++) d[i] = (VkCheckpointData2NV){.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_2_NV};
        get2(c->gfx_queue, &n, d);
        for (uint32_t i = 0; i < n; i++) stage[i] = d[i].stage, marker[i] = (uint64_t)(uintptr_t)d[i].pCheckpointMarker;
    } else if (get1) {
        VkCheckpointDataNV d[MAXN];
        get1(c->gfx_queue, &n, NULL);
        if (n > MAXN) n = MAXN;
        for (uint32_t i = 0; i < n; i++) d[i] = (VkCheckpointDataNV){.sType = VK_STRUCTURE_TYPE_CHECKPOINT_DATA_NV};
        get1(c->gfx_queue, &n, d);
        for (uint32_t i = 0; i < n; i++) stage[i] = d[i].stage, marker[i] = (uint64_t)(uintptr_t)d[i].pCheckpointMarker;
    } else {
        rt_log("vk: pass marks: no vkGetQueueCheckpointDataNV entry point");
        return;
    }
    uint32_t started = 0, finished = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t id = (uint32_t)(marker[i] >> 1);
        int end = (int)(marker[i] & 1);
        rt_log("vk: checkpoint of stage %#llx: the %s of mark %u", (unsigned long long)stage[i], end ? "end" : "start", id);
        if (stage[i] == VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT) started = id;
        if (stage[i] == VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT) finished = end ? id : id ? id - 1 : 0;
    }
    if (!n) rt_log("vk: pass marks: the driver reported no checkpoints");
    char t[2048];
    vk_pass_marks_format("VK_NV_device_diagnostic_checkpoints", started, finished, t, sizeof t);
    log_lines(t);
}

void vk_pass_marks_report(void) {
    if (!vk_pass_marks_on) return;
    uint32_t started, finished;
    char t[2048];
    if (vk_pass_marks_read(&started, &finished)) {
        vk_pass_marks_format("VK_AMD_buffer_marker", started, finished, t, sizeof t);
        log_lines(t);
    }
    nv_report();
}
