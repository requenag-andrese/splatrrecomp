// sr-recomp: frame interpolation from the finished picture alone (SR_FRAME_INTERP, the Screen tab). Adapted from Hookshot's
// gpu/vk/vk_framegen.cpp (https://github.com/NotRexed/Hookshot, GPL-3.0); its shaders are in interp/.
//
// Frames are made between the game's from its last two, and needs nothing of the engine, unlike DLSS-G and FSR 3, whose
// picture this game ruined:
//   1. each game frame is copied, as the next may draw over it, and reduced to quarter-size luma;
//   2. block matching: a vector per 16x16 pixels within +-128 pixels (ours; see interp/fg_flow.comp), then a 3x3 median;
//   3. each made frame at time t blends the last frame moved along t and the new one moved back along 1 - t, and only
//      where those agree; elsewhere it is their plain blend, so a wrong vector costs softness, not a flash.
// The game's own speed does not change. A display thread shows the made frames and then the game's, evenly over the game's
// recent frame time, about one frame behind the game. They go through the normal present (present_image), so the colour
// step, the scaler and the menu overlay apply to each. A menu that is open and a hitch of over 100 ms are not interpolated.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "ui/menu.h"
#include "vk.h"

#include "interp/fg_flow_comp_spv.h"
#include "interp/fg_luma_comp_spv.h"
#include "interp/fg_median_comp_spv.h"
#include "interp/fg_synth_comp_spv.h"

int sl_fg_active(void);
int fsr_fg_on(void);

enum { MAX_MULT = 4, SETS = 3 };
#define IMG_FORMAT VK_FORMAT_R8G8B8A8_UNORM

typedef struct { VkImage image; VmaAllocation alloc; VkImageView view; } Img;
typedef struct { VkDescriptorSetLayout set; VkPipelineLayout layout; VkPipeline pipe; } Pipe;
typedef struct {
    Img frame[MAX_MULT];   // frame[0..made) the made ones, frame[made] the game's
    int made, busy, queued;
    int64_t at;
    VkCommandBuffer cb;
    VkFence fence;
} Set;

static struct {
    int init, failed, thread;
    uint32_t w, h;
    int mult;              // what the sets are made for
    Pipe luma, flow, median, synth;
    VkSampler nearest, linear;
    VkCommandPool pool;
    PFN_vkCmdPushDescriptorSetKHR push;
    Img frames[2], lumas[2], flows[2];
    Set sets[SETS];
    int cur, next_set, have_prev, tail;
    uint64_t last_at;
    PlatMutex mu;
    PlatCond cv;
} I = {.mu = PLAT_MUTEX_INIT, .cv = PLAT_COND_INIT};

static uint64_t now_ns(void) { return plat_now_ns(); }

static int setting(void) {
    const char *e = cfg_value("SR_FRAME_INTERP");
    return e && e[0] >= '2' && e[0] <= '4' ? e[0] - '0' : 0;
}

// What VSync lets out, measured by the display thread when its presents take longer than the refresh rate allows: the
// monitor's signal can be at 240 while the desktop composes a window at 60 (Windows' dynamic refresh rate), and the game
// would wait on each present. Millihertz, 0 none; it holds while the setting, the refresh rate, the present mode,
// fullscreen and the window's focus stay as they were when it was measured (a window in the background is composed even
// when it fills the screen).
static volatile int32_t g_out_mhz;
static volatile int32_t g_out_key;
static uint64_t g_set_wait_ns;   // the game's time waiting for a free set, all told (vk_interp_present)

static uint32_t out_key(void) {
    return (uint32_t)(gfx_window_refresh_hz() * 1000 + 0.5) ^ (uint32_t)gfx_window_present_mode()[0] << 24 ^
           (uint32_t)(cfg_bool("SR_FULLSCREEN") != 0) << 23 ^ (uint32_t)setting() << 20 ^
           gfx_window_focus_messages() * 2654435761u;
}

double vk_interp_rate_hz(void) {
    double hz = gfx_window_refresh_hz();
    // with VSync and the window composed for sure (no focus, exclusive fullscreen given back: Alt+Tab, a recording
    // started), the presents go out at the desktop's rate, 60 under Windows' dynamic refresh rate; it applies at once,
    // where the measure below takes two stretches of 2 s of stutter
    const double desk = gfx_window_desktop_hz();
    if (desk > 0 && desk < hz && gfx_window_composed() && !strcmp(gfx_window_present_mode(), "fifo")) hz = desk;
    const int32_t out = plat_cas32(&g_out_mhz, 0, 0);
    if (out && (uint32_t)plat_cas32(&g_out_key, 0, 0) == out_key() && (hz <= 0 || out / 1000.0 < hz))
        hz = out / 1000.0;
    return hz;
}

// 0 off, else how many frames are shown for each of the game's: what the setting asks, and no more than the monitor can
// show of a game at 60 (with mailbox the rest would be dropped unevenly, with FIFO the game itself would slow down).
static int wanted(void) {
    int m = setting();
    if (m < 2 || cfg_get("SR_INTERP_SHOTS")) return m;
    const double hz = vk_interp_rate_hz();
    const int fits = hz > 0 ? (int)((hz + 3) / 60) : m;
    static int told;
    if (fits < m && told != m) {
        told = m;
        rt_log("vk_interp: %dx asked for, %.1f frames a second on the screen fit %dx%s", m, hz, fits, fits < 2 ? ": off" : "");
    }
    if (fits >= m) told = 0;
    if (m > fits) m = fits;
    return m >= 2 ? m : 0;
}

static int make_pipe(Pipe *p, const uint32_t *code, size_t bytes, const VkDescriptorType *types, uint32_t n, uint32_t push) {
    VkDevice d = vk_ctx()->device;
    VkDescriptorSetLayoutBinding b[4];
    for (uint32_t i = 0; i < n; i++)
        b[i] = (VkDescriptorSetLayoutBinding){i, types[i], 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                          .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                          .bindingCount = n, .pBindings = b};
    if (vkCreateDescriptorSetLayout(d, &li, NULL, &p->set) != VK_SUCCESS) return 0;
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, push};
    VkPipelineLayoutCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &p->set,
                                     .pushConstantRangeCount = push ? 1 : 0, .pPushConstantRanges = &pr};
    if (vkCreatePipelineLayout(d, &pi, NULL, &p->layout) != VK_SUCCESS) return 0;
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = bytes, .pCode = code};
    VkShaderModule m = VK_NULL_HANDLE;
    if (vkCreateShaderModule(d, &mi, NULL, &m) != VK_SUCCESS) return 0;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = p->layout};
    VkResult r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &ci, NULL, &p->pipe);
    vkDestroyShaderModule(d, m, NULL);
    return r == VK_SUCCESS;
}

static int init_once(void) {
    if (I.init) return !I.failed;
    I.init = I.failed = 1;
    VkCtx *c = vk_ctx();
    I.push = (PFN_vkCmdPushDescriptorSetKHR)vkGetDeviceProcAddr(c->device, "vkCmdPushDescriptorSetKHR");
    if (!I.push) {
        rt_log("vk_interp: no push descriptors: off");
        return 0;
    }
    const VkDescriptorType S = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, W = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    const VkDescriptorType si[2] = {S, W}, ssi[3] = {S, S, W}, sssi[4] = {S, S, S, W};
    if (!make_pipe(&I.luma, FG_LUMA_COMP_SPV, sizeof FG_LUMA_COMP_SPV, si, 2, 0) ||
        !make_pipe(&I.flow, FG_FLOW_COMP_SPV, sizeof FG_FLOW_COMP_SPV, ssi, 3, 0) ||
        !make_pipe(&I.median, FG_MEDIAN_COMP_SPV, sizeof FG_MEDIAN_COMP_SPV, si, 2, 0) ||
        !make_pipe(&I.synth, FG_SYNTH_COMP_SPV, sizeof FG_SYNTH_COMP_SPV, sssi, 4, 4)) {
        rt_log("vk_interp: pipelines not made: off");
        return 0;
    }
    VkSamplerCreateInfo sa = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .maxLod = 0.25f};
    vkCreateSampler(c->device, &sa, NULL, &I.nearest);
    sa.magFilter = sa.minFilter = VK_FILTER_LINEAR;
    vkCreateSampler(c->device, &sa, NULL, &I.linear);
    VkCommandPoolCreateInfo pci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = c->gfx_family};
    vkCreateCommandPool(c->device, &pci, NULL, &I.pool);
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < SETS; i++) {
        VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = I.pool,
                                          .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        vkAllocateCommandBuffers(c->device, &ai, &I.sets[i].cb);
        vkCreateFence(c->device, &fi, NULL, &I.sets[i].fence);
    }
    I.failed = 0;
    return 1;
}

static int make_img(Img *i, uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage) {
    VkCtx *c = vk_ctx();
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f, .extent = {w, h, 1},
                            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = usage, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ci, &ai, &i->image, &i->alloc, NULL) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = i->image, .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                .format = f, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, &i->view) == VK_SUCCESS;
}

static void drop_img(Img *i) {
    VkCtx *c = vk_ctx();
    if (i->view) vkDestroyImageView(c->device, i->view, NULL);
    if (i->image) vmaDestroyImage(c->vma, i->image, i->alloc);
    memset(i, 0, sizeof *i);
}

static void barrier_all(VkCommandBuffer cb) {
    VkMemoryBarrier2 m = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                          .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &m};
    vkCmdPipelineBarrier2(cb, &d);
}

static void to_general(VkCommandBuffer cb, VkImage image) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                               .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                               .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                               .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                               .image = image, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo d = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &d);
}

// Waits until the display thread has shown everything, so a direct present cannot overtake it.
static void drain(void) {
    plat_mutex_lock(&I.mu);
    while (I.sets[0].busy || I.sets[1].busy || I.sets[2].busy) plat_cond_wait_ms(&I.cv, &I.mu, 50);
    plat_mutex_unlock(&I.mu);
}

// The images for this size and multiplier; new ones go to GENERAL at the head of cb. All sets are free here (drain).
static int ensure_size(VkCommandBuffer cb, uint32_t w, uint32_t h, int mult) {
    if (I.w == w && I.h == h && I.mult == mult) return 1;
    VkCtx *c = vk_ctx();
    vk_queue_lock();
    vkQueueWaitIdle(c->gfx_queue);
    vk_queue_unlock();
    for (int i = 0; i < 2; i++) drop_img(&I.frames[i]), drop_img(&I.lumas[i]), drop_img(&I.flows[i]);
    for (int s = 0; s < SETS; s++)
        for (int k = 0; k < MAX_MULT; k++) drop_img(&I.sets[s].frame[k]);
    const uint32_t qw = (w + 3) / 4, qh = (h + 3) / 4, fw = (qw + 3) / 4, fh = (qh + 3) / 4;
    const VkImageUsageFlags rw = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    const VkImageUsageFlags xf = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    int ok = 1;
    for (int i = 0; i < 2; i++) {
        ok = ok && make_img(&I.frames[i], w, h, IMG_FORMAT, rw | xf);
        ok = ok && make_img(&I.lumas[i], qw, qh, VK_FORMAT_R16_SFLOAT, rw);
        ok = ok && make_img(&I.flows[i], fw, fh, VK_FORMAT_R16G16B16A16_SFLOAT, rw);
    }
    for (int s = 0; s < SETS; s++)
        for (int k = 0; k < mult; k++) ok = ok && make_img(&I.sets[s].frame[k], w, h, IMG_FORMAT, rw | xf);
    if (!ok) {
        I.failed = 1;
        I.w = I.h = 0, I.mult = 0;
        rt_log("vk_interp: images for %ux%u not made: off", w, h);
        return 0;
    }
    for (int i = 0; i < 2; i++) to_general(cb, I.frames[i].image), to_general(cb, I.lumas[i].image), to_general(cb, I.flows[i].image);
    for (int s = 0; s < SETS; s++)
        for (int k = 0; k < mult; k++) to_general(cb, I.sets[s].frame[k].image);
    I.w = w, I.h = h, I.mult = mult, I.have_prev = 0;
    rt_log("vk_interp: %dx the game's frames at %ux%u (made from its last two, shown evenly)", mult, w, h);
    return 1;
}

// One pass over w x h in 8x8 groups. A binding with a sampler is sampled, without one a storage image.
typedef struct { VkImageView view; VkSampler sampler; } Bind;
static void pass(VkCommandBuffer cb, const Pipe *p, const Bind *b, uint32_t n, uint32_t w, uint32_t h, const void *push, uint32_t bytes) {
    VkDescriptorImageInfo ii[4];
    VkWriteDescriptorSet ws[4];
    for (uint32_t i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){b[i].sampler, b[i].view, VK_IMAGE_LAYOUT_GENERAL};
        ws[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = i, .descriptorCount = 1,
                                       .descriptorType = b[i].sampler ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .pImageInfo = &ii[i]};
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->pipe);
    I.push(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p->layout, 0, n, ws);
    if (bytes) vkCmdPushConstants(cb, p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, bytes, push);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    barrier_all(cb);
}

// ---- the display thread
static volatile int32_t g_shot_k = -1;     // the test hook: this present's frame in its set
static uint64_t g_shots_done;

// SR_INTERP_SHOTS=<lo>-<hi>[/<every>]:<dir>: the frames shown for game frames lo..hi of this thread's count (with every,
// two sets in a row each so many), each to a PNG named i<count>-<k>.png, k the place in its set (the highest is the
// game's). For looking at what it makes; it costs each a readback, so pacing does not hold while it runs.
const char *vk_interp_shot(void) {
    static char path[600];
    const int k = __atomic_load_n(&g_shot_k, __ATOMIC_RELAXED);
    if (k < 0) return NULL;
    const char *e = cfg_get("SR_INTERP_SHOTS");
    unsigned long long lo, hi, every = 0;
    int off = 0;
    if (!e) return NULL;
    if (sscanf(e, "%llu-%llu/%llu:%n", &lo, &hi, &every, &off) < 3 || !off) {
        every = 0, off = 0;
        if (sscanf(e, "%llu-%llu:%n", &lo, &hi, &off) < 2 || !off) return NULL;
    }
    if (g_shots_done < lo || g_shots_done > hi || (every && (g_shots_done - lo) % every > 1)) return NULL;
    plat_file_mkdir_p(e + off);
    snprintf(path, sizeof path, "%s/i%05llu-%d.png", e + off, (unsigned long long)g_shots_done, k);
    return path;
}

static void display_main(void *arg) {
    (void)arg;
    plat_thread_priority(PLAT_PRIO_ABOVE_NORMAL);
    VkDevice device = vk_ctx()->device;
    int64_t interval = 16666667, last_at = 0, next_show = 0;
    uint64_t shown = 0, made = 0, late = 0, dropped = 0, since = 0, ready_ns = 0, ready_n = 0;
    // VSync's check (below): the stretch of 2 s being counted, and how many in a row held the game back
    struct { int64_t start; uint64_t shown, made, dropped, waited; int short_ones; } w = {0};
    // the GPU guard (below): the stretch of 2 s, the game's frames in it, and how many in a row ran below 57 a second
    struct { int64_t start; uint64_t sets; int slow; } gg = {0};
    for (;;) {
        plat_mutex_lock(&I.mu);
        while (!I.sets[I.tail].queued) plat_cond_wait(&I.cv, &I.mu);
        Set *s = &I.sets[I.tail];
        I.tail = (I.tail + 1) % SETS;
        plat_mutex_unlock(&I.mu);
        vkWaitForFences(device, 1, &s->fence, VK_TRUE, 5000000000ull);
        ready_ns += now_ns() - (uint64_t)s->at, ready_n++;
        const int64_t d = last_at ? s->at - last_at : 0;
        if (d > 0 && d < 100000000) interval = (interval * 7 + (d < 4000000 ? 4000000 : d > 50000000 ? 50000000 : d)) / 8;
        last_at = s->at;
        if (!since) since = (uint64_t)s->at;
        if ((uint64_t)s->at - since > 5000000000ull) {
            rt_log("vk_interp: %llu frames shown, %llu of them made, %llu late (over 2 ms), %llu dropped to catch up; the game's "
                   "frames %.2f ms apart, the made ones ready %.2f ms after each",
                   (unsigned long long)shown, (unsigned long long)made, (unsigned long long)late, (unsigned long long)dropped,
                   (double)interval / 1e6, (double)ready_ns / 1e6 / (double)(ready_n ? ready_n : 1));
            shown = made = late = dropped = ready_ns = ready_n = 0, since = (uint64_t)s->at;
        }
        // Every frame has its place on an even line, the recent frame time apart: never more than a game frame behind what
        // the game handed over, and a line that has fallen behind starts again from now.
        // Behind by more than a game frame (after a hitch, a made frame the GPU finished late, or a present that waited),
        // the made frames are dropped until the game's own catches up: a line that never drains would hold the game back.
        // (Presenting each set's frames at once with VSync and letting the swapchain space them, tried on 2026-11-10 for
        // 4x at 240 Hz, let out 60 a second; the timer's line gives 240.)
        const int count = s->made + 1;
        const int64_t step = interval / count;
        const double hz = gfx_window_refresh_hz();
        const int fifo = !strcmp(gfx_window_present_mode(), "fifo");
        uint64_t set_shown = 0, set_dropped = 0;
        for (int k = 0; k < count; k++) {
            int64_t due = next_show;
            const int64_t now = (int64_t)now_ns();
            if (k < s->made && now - s->at > interval + step) {
                dropped++, set_dropped++;
                continue;
            }
            set_shown++;
            if (due < now) due = now;
            if (due > s->at + interval + step) due = s->at + interval + step;
            if (due - now > 1500000) plat_wait_ns((uint64_t)(due - now - 1000000));
            while ((int64_t)now_ns() < due) plat_yield();
            const int64_t t = (int64_t)now_ns();
            plat_xchg32(&g_shot_k, k);
            vk_present_image(s->frame[k].image, VK_IMAGE_LAYOUT_GENERAL, IMG_FORMAT, (int)I.w, (int)I.h, NULL);
            plat_xchg32(&g_shot_k, -1);
            next_show = t + step;
            shown++;
            if (k < s->made) made++;
            if (t - due > 2000000) late++;
        }
        // VSync's check: with fewer turns than the monitor's signal has (a window the desktop composes at 60, which
        // Windows' dynamic refresh rate does while nobody touches the PC), the presents wait, the line falls behind, the
        // made frames are dropped and the game may wait for this thread to free a set (vk_interp_present). Two stretches
        // of 2 s in a row with over a fifth of the made frames dropped or the game waiting over a tenth of the time, and
        // wanted() fits the multiplier to what went out (off when that is the game's own 60).
        const int64_t end = (int64_t)now_ns();
        const uint64_t waited = __atomic_load_n(&g_set_wait_ns, __ATOMIC_RELAXED);
        if (!fifo || hz <= 0) {
            memset(&w, 0, sizeof w);
        } else if (!w.start) {
            w.start = end, w.waited = waited;
        } else {
            w.shown += set_shown, w.made += set_shown - 1, w.dropped += set_dropped;
            if (end - w.start >= 2000000000ll) {
                const double out = (double)w.shown * 1e9 / (double)(end - w.start);
                const uint64_t held = waited - w.waited;
                const int short_one = held * 10 > (uint64_t)(end - w.start) || w.dropped * 5 > w.made + w.dropped;
                w.short_ones = short_one ? w.short_ones + 1 : 0;
                if (w.short_ones >= 2) {
                    plat_xchg32(&g_out_key, (int32_t)out_key());
                    plat_xchg32(&g_out_mhz, (int32_t)(out * 1000));
                    rt_log("vk_interp: with VSync %.1f frames a second went out of %.1f asked for, %llu dropped and the game held "
                           "back %.0f ms in 2 s; the monitor's signal is at %.1f Hz (a window the desktop composes?)",
                           out, (double)count * 1e9 / (double)interval, (unsigned long long)w.dropped, (double)held / 1e6, hz);
                    w.short_ones = 0;
                }
                w.start = end, w.waited = waited, w.shown = w.made = w.dropped = 0;
            }
        }
        // The GPU guard: at 3x and 4x the made frames take several ms of GPU at 2560x1440, and in a heavy scene the game
        // and they together go past 16.7 ms, so the game itself falls below 60 and stutters. Two stretches of 2 s in a
        // row below 57 game frames a second, and wanted() takes one step off (never below 2x), until the setting, the
        // refresh rate, the present mode, fullscreen or the focus changes. A game slow on its own loses only the step.
        {
            const int64_t now = (int64_t)now_ns();
            if (!gg.start || count < 3) gg.start = now, gg.sets = 0, gg.slow = count < 3 ? 0 : gg.slow;
            else {
                gg.sets++;
                if (now - gg.start >= 2000000000ll) {
                    const double fps = (double)gg.sets * 1e9 / (double)(now - gg.start);
                    gg.slow = fps < 57.0 ? gg.slow + 1 : 0;
                    if (gg.slow >= 2) {
                        plat_xchg32(&g_out_key, (int32_t)out_key());
                        plat_xchg32(&g_out_mhz, (int32_t)((count - 1) * 60 * 1000 + 500));
                        rt_log("vk_interp: the game ran at %.1f frames a second with %dx (the GPU shared with the made "
                               "frames): %dx from now on", fps, count, count - 1);
                        gg.slow = 0;
                    }
                    gg.start = now, gg.sets = 0;
                }
            }
        }
        g_shots_done++;
        plat_mutex_lock(&I.mu);
        s->queued = 0;
        s->busy = 0;
        plat_cond_broadcast(&I.cv);
        plat_mutex_unlock(&I.mu);
    }
}

// ---- the present path
int vk_interp_present(VkImage src, VkImageLayout layout, VkFormat fmt, int w, int h, const int crop[4]) {
    const int mult = wanted();
    int on = mult >= 2 && src && layout == VK_IMAGE_LAYOUT_GENERAL && !menu_is_open() && !sl_fg_active() && !fsr_fg_on() && !I.failed;
    if (on && fmt != IMG_FORMAT) {
        static int told;
        if (!told++) rt_log("vk_interp: the game's window texture has format %d, not RGBA8: off", (int)fmt);
        on = 0;
    }
    if (!on) {
        if (I.init) {
            drain();
            I.have_prev = 0;
        }
        return 0;
    }
    int x0 = 0, y0 = 0, cw = w, ch = h;
    if (crop && crop[2] > 0 && crop[3] > 0) x0 = crop[0], y0 = crop[1], cw = crop[2], ch = crop[3];
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (cw > w - x0) cw = w - x0;
    if (ch > h - y0) ch = h - y0;
    if (cw <= 0 || ch <= 0 || !init_once()) return 0;
    if (I.w != (uint32_t)cw || I.h != (uint32_t)ch || I.mult != mult) drain();
    const uint64_t now = now_ns();
    // A hitch: nothing to blend. The test hook's PNGs make every frame look like one.
    const int gap = I.last_at && now - I.last_at > 100000000 && !cfg_get("SR_INTERP_SHOTS");
    I.last_at = now;
    plat_mutex_lock(&I.mu);
    Set *s = &I.sets[I.next_set];
    if (s->busy) {   // the display thread is behind: the game waits (VSync's check in display_main counts it)
        const uint64_t t0 = now_ns();
        while (s->busy) plat_cond_wait_ms(&I.cv, &I.mu, 50);
        __atomic_fetch_add(&g_set_wait_ns, now_ns() - t0, __ATOMIC_RELAXED);
    }
    s->busy = 1;
    I.next_set = (I.next_set + 1) % SETS;
    plat_mutex_unlock(&I.mu);
    VkCtx *c = vk_ctx();
    vkWaitForFences(c->device, 1, &s->fence, VK_TRUE, 5000000000ull);
    vkResetCommandBuffer(s->cb, 0);
    VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    vkBeginCommandBuffer(s->cb, &bi);
    if (!ensure_size(s->cb, (uint32_t)cw, (uint32_t)ch, mult)) {
        vkEndCommandBuffer(s->cb);
        plat_mutex_lock(&I.mu);
        s->busy = 0;
        plat_mutex_unlock(&I.mu);
        return 0;
    }
    const VkCommandBuffer cb = s->cb;
    barrier_all(cb);   // the game's writes to the window texture
    const int cur = I.cur, prev = cur ^ 1, blend = I.have_prev && !gap;
    const uint32_t uw = I.w, uh = I.h, qw = (uw + 3) / 4, qh = (uh + 3) / 4, fw = (qw + 3) / 4, fh = (qh + 3) / 4;
    VkImageCopy crop_copy = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {x0, y0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {uw, uh, 1}};
    vkCmdCopyImage(cb, src, VK_IMAGE_LAYOUT_GENERAL, I.frames[cur].image, VK_IMAGE_LAYOUT_GENERAL, 1, &crop_copy);
    barrier_all(cb);
    {
        const Bind b[2] = {{I.frames[cur].view, I.nearest}, {I.lumas[cur].view, VK_NULL_HANDLE}};
        pass(cb, &I.luma, b, 2, qw, qh, NULL, 0);
    }
    s->made = 0;
    if (blend) {
        const Bind fb[3] = {{I.lumas[prev].view, I.nearest}, {I.lumas[cur].view, I.nearest}, {I.flows[0].view, VK_NULL_HANDLE}};
        pass(cb, &I.flow, fb, 3, fw, fh, NULL, 0);
        const Bind mb[2] = {{I.flows[0].view, I.nearest}, {I.flows[1].view, VK_NULL_HANDLE}};
        pass(cb, &I.median, mb, 2, fw, fh, NULL, 0);
        for (int k = 1; k < mult; k++) {
            const float t = (float)k / (float)mult;
            const Bind sb[4] = {{I.frames[prev].view, I.linear}, {I.frames[cur].view, I.linear}, {I.flows[1].view, I.nearest},
                                {s->frame[k - 1].view, VK_NULL_HANDLE}};
            pass(cb, &I.synth, sb, 4, uw, uh, &t, 4);
        }
        s->made = mult - 1;
    }
    VkImageCopy whole = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {uw, uh, 1}};
    vkCmdCopyImage(cb, I.frames[cur].image, VK_IMAGE_LAYOUT_GENERAL, s->frame[s->made].image, VK_IMAGE_LAYOUT_GENERAL, 1, &whole);
    barrier_all(cb);
    vkEndCommandBuffer(cb);
    I.have_prev = 1;
    I.cur = prev;
    s->at = (int64_t)now;
    vkResetFences(c->device, 1, &s->fence);
    VkCommandBufferSubmitInfo cbi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi};
    vk_queue_lock();
    VkResult r = vkQueueSubmit2(c->gfx_queue, 1, &si, s->fence);
    vk_queue_unlock();
    if (r != VK_SUCCESS) {
        rt_log("vk_interp: submit failed (%d): off", (int)r);
        I.failed = 1;
        plat_mutex_lock(&I.mu);
        s->busy = 0;
        plat_mutex_unlock(&I.mu);
        return 0;
    }
    if (!I.thread) {
        I.thread = 1;
        if (!plat_thread_spawn(display_main, NULL, "interp display", 0)) rt_fatal("vk_interp: cannot start its display thread");
    }
    plat_mutex_lock(&I.mu);
    s->queued = 1;
    plat_cond_broadcast(&I.cv);
    plat_mutex_unlock(&I.mu);
    return 1;
}

// Before the device goes away (gfx_window_release_vk): presents go straight through from here on. The display thread stays
// parked. If it does not finish what it holds in two seconds, what it uses is left for the process's end to free.
void vk_interp_release(void) {
    if (!I.init) return;
    I.failed = 1;
    plat_mutex_lock(&I.mu);
    for (int i = 0; i < 40 && (I.sets[0].busy || I.sets[1].busy || I.sets[2].busy); i++) plat_cond_wait_ms(&I.cv, &I.mu, 50);
    const int stuck = I.sets[0].busy || I.sets[1].busy || I.sets[2].busy;
    plat_mutex_unlock(&I.mu);
    if (stuck) return;
    VkCtx *c = vk_ctx();
    vk_queue_lock();
    vkQueueWaitIdle(c->gfx_queue);
    vk_queue_unlock();
    for (int i = 0; i < 2; i++) drop_img(&I.frames[i]), drop_img(&I.lumas[i]), drop_img(&I.flows[i]);
    for (int s = 0; s < SETS; s++) {
        for (int k = 0; k < MAX_MULT; k++) drop_img(&I.sets[s].frame[k]);
        if (I.sets[s].fence) vkDestroyFence(c->device, I.sets[s].fence, NULL);
    }
    if (I.pool) vkDestroyCommandPool(c->device, I.pool, NULL);
    if (I.nearest) vkDestroySampler(c->device, I.nearest, NULL);
    if (I.linear) vkDestroySampler(c->device, I.linear, NULL);
    const Pipe *const ps[4] = {&I.luma, &I.flow, &I.median, &I.synth};
    for (int i = 0; i < 4; i++) {
        if (ps[i]->pipe) vkDestroyPipeline(c->device, ps[i]->pipe, NULL);
        if (ps[i]->layout) vkDestroyPipelineLayout(c->device, ps[i]->layout, NULL);
        if (ps[i]->set) vkDestroyDescriptorSetLayout(c->device, ps[i]->set, NULL);
    }
}
