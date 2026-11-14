// sr-recomp: NVIDIA DLSS in place of Splatoon Raiders' own neural upscaler (sr-config.ini dlaa = 1, SR_DLAA; read at start,
// since NGX's Vulkan extensions are asked for at device creation). F8 switches it off and on while the game runs, to
// compare with the game's upscaler.
//
// The NGX half (loading the driver's core, the parameter object's vtable, the extensions, feature creation and
// evaluation) and the camera reprojection are adapted from totk-pc-recomp runtime/vk_dlaa.c and shaders/dlaa_mv.comp
// @ 0f65b4ee07e934e99b0c937d059f6bb6ae72e1c3 (MIT, Copyright (c) 2026 Valiantsin Dzerakh); dlaa_mv_spv.h is that
// shader's SPIR-V, unchanged. No NVIDIA SDK code or header is compiled in; NGX loads nvngx_dlss.dll from the folder of
// sr.exe.
//
// Where it runs (SR_DUMP of a gameplay frame): the scene is drawn forward into an R11G11B10F
// target with a D32F depth at the render size (1707x960 at a 2560x1440 output, smaller under dynamic resolution), then
// bloom, tonemap and SMAA at that size; then the game's neural upscaler, one compute dispatch (vk_exec_draw.c
// nn_upscaler), scales the picture to 2560x1440, and the HUD is drawn over that. DLSS takes the dispatch's place: it
// reads the dispatch's input picture, the scene depth and the motion vectors below, and its result is blitted into the
// dispatch's output image, so the HUD draws over DLSS's picture and is never jittered.
//
// Camera: every scene draw binds the same uniform block as vertex c3 (0x3040 bytes). vec4 3-6 are this frame's
// view-projection (rows of a row-major 4x4, OpenGL clip depth -w..w), vec4 24-27 last frame's, which the game keeps
// itself. The motion of each pixel is its depth taken back through this frame's view-projection and forward through last
// frame's (dlaa_mv.comp); objects that move on their own get only the camera's motion.
//
// Jitter: the viewports of the scene passes (the R11G11B10F target with the scene depth) move by a Halton(2,3)
// sub-pixel offset; the post passes and the HUD are not moved. A jittered frame DLSS did not resolve would shake on
// screen, so a missed frame stops the jitter for a second.
#ifdef _WIN32
#include <windows.h>
#endif
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#include "../core/cfg.h"
#include "../core/host.h"
#include "../core/ring.h"
#include "../core/platform/platform.h"
#include "../core/vk.h"
#include "../core/vk_dlaa.h"
#include "../core/vk_exec.h"
#include "../core/vk_rec.h"
#include "dlaa_mv_spv.h"
#include "vk_dlssg.h"

// ---------------------------------------------------------------- NGX (after totk vk_dlaa.c)
typedef int NgxResult;   // NVSDK_NGX_Result: 1 success, 0xBAD00000 | n failures
#define NGX_OK(r) (((unsigned)(r) & 0xFFF00000u) != 0xBAD00000u)
#define NGX_VERSION_API 0x15
#define NGX_FEATURE_SUPERSAMPLING 1
typedef struct NgxParam NgxParam;
typedef struct NgxHandle NgxHandle;
typedef struct { void *slot[17]; } NgxParamVtbl;
struct NgxParam { const NgxParamVtbl *vt; };
typedef struct {   // NVSDK_NGX_ImageViewInfo_VK
    VkImageView view;
    VkImage image;
    VkImageSubresourceRange range;
    VkFormat format;
    unsigned width, height;
} NgxImageView;
typedef struct {   // NVSDK_NGX_Resource_VK
    union { NgxImageView img; struct { VkBuffer buffer; unsigned bytes; } buf; } r;
    int type;      // 0 image view, 1 buffer
    _Bool rw;
} NgxResourceVK;

typedef NgxResult (*PFN_Init)(unsigned long long app, const wchar_t *path, VkInstance, VkPhysicalDevice, VkDevice,
                              PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, int version, const void *info);
typedef NgxResult (*PFN_ReqExt)(unsigned *ni, const char ***ie, unsigned *nd, const char ***de);
typedef NgxResult (*PFN_Params)(NgxParam **);
typedef NgxResult (*PFN_DestroyParams)(NgxParam *);
typedef NgxResult (*PFN_Create)(VkCommandBuffer, int feature, NgxParam *, NgxHandle **);
typedef NgxResult (*PFN_Evaluate)(VkCommandBuffer, const NgxHandle *, const NgxParam *, void *callback);
typedef NgxResult (*PFN_Release)(NgxHandle *);

static struct {
    void *core;
    PFN_Init init;
    PFN_ReqExt req_ext;
    PFN_Params caps, alloc_params;
    PFN_DestroyParams destroy_params;
    PFN_Create create;
    PFN_Evaluate evaluate;
    PFN_Release release;
    int tried, loaded, available;
} N;

typedef void (*SetI)(NgxParam *, const char *, int);
typedef void (*SetU)(NgxParam *, const char *, unsigned);
typedef void (*SetF)(NgxParam *, const char *, float);
typedef void (*SetP)(NgxParam *, const char *, void *);
typedef NgxResult (*GetI)(const NgxParam *, const char *, int *);
typedef NgxResult (*GetU)(const NgxParam *, const char *, unsigned *);
typedef NgxResult (*GetF)(const NgxParam *, const char *, float *);
typedef NgxResult (*GetP)(const NgxParam *, const char *, void **);
// The Windows core (MSVC) lays each group of overloads out in reverse declaration order (totk vk_dlaa.c).
enum { S_P = 0, S_D12, S_D11, S_I, S_U32, S_D, S_F, S_U64, G_P, G_D12, G_D11, G_I, G_U32, G_D, G_F, G_U64 };
static void p_seti(NgxParam *p, const char *n, int v) { ((SetI)p->vt->slot[S_I])(p, n, v); }
static void p_setu(NgxParam *p, const char *n, unsigned v) { ((SetU)p->vt->slot[S_U32])(p, n, v); }
static void p_setf(NgxParam *p, const char *n, float v) { ((SetF)p->vt->slot[S_F])(p, n, v); }
static void p_setp(NgxParam *p, const char *n, void *v) { ((SetP)p->vt->slot[S_P])(p, n, v); }
static int p_geti(const NgxParam *p, const char *n, int *v) { return NGX_OK(((GetI)p->vt->slot[G_I])(p, n, v)); }
static int p_getu(const NgxParam *p, const char *n, unsigned *v) { return NGX_OK(((GetU)p->vt->slot[G_U32])(p, n, v)); }
static int p_getf(const NgxParam *p, const char *n, float *v) { return NGX_OK(((GetF)p->vt->slot[G_F])(p, n, v)); }
static int p_getp(const NgxParam *p, const char *n, void **v) { return NGX_OK(((GetP)p->vt->slot[G_P])(p, n, v)); }

int vk_dlaa_wanted(void) {
    static int wanted = -1;
    if (wanted < 0) wanted = cfg_bool("SR_DLAA");
    return wanted;
}

static int core_load(void) {
    if (N.tried) return N.loaded;
    N.tried = 1;
#ifdef _WIN32
    char dir[MAX_PATH] = {0}, path[MAX_PATH + 32];
    DWORD n = sizeof dir;
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "SOFTWARE\\NVIDIA Corporation\\Global\\NGXCore", "FullPath", RRF_RT_REG_SZ,
                     NULL, dir, &n) != ERROR_SUCCESS) {
        rt_log("dlss: the driver has no NGX core (registry NGXCore missing): DLSS off");
        return 0;
    }
    snprintf(path, sizeof path, "%s\\_nvngx.dll", dir);
    N.core = LoadLibraryA(path);
    if (!N.core) {
        rt_log("dlss: cannot load %s (error %lu): DLSS off", path, GetLastError());
        return 0;
    }
#define FN(f, name) N.f = (void *)GetProcAddress((HMODULE)N.core, name)
    FN(init, "NVSDK_NGX_VULKAN_Init_Ext2");
    FN(req_ext, "NVSDK_NGX_VULKAN_RequiredExtensions");
    FN(caps, "NVSDK_NGX_VULKAN_GetCapabilityParameters");
    FN(alloc_params, "NVSDK_NGX_VULKAN_AllocateParameters");
    FN(destroy_params, "NVSDK_NGX_VULKAN_DestroyParameters");
    FN(create, "NVSDK_NGX_VULKAN_CreateFeature");
    FN(evaluate, "NVSDK_NGX_VULKAN_EvaluateFeature");
    FN(release, "NVSDK_NGX_VULKAN_ReleaseFeature");
#undef FN
    if (!N.init || !N.req_ext || !N.caps || !N.alloc_params || !N.create || !N.evaluate || !N.release) {
        rt_log("dlss: the NGX core %s lacks Vulkan entry points: DLSS off", path);
        return 0;
    }
    N.loaded = 1;
    rt_log("dlss: NGX core %s", path);
    return 1;
#else
    rt_log("dlss: Windows only in this port: DLSS off");
    return 0;
#endif
}

static int listed(const char *const *list, uint32_t n, const char *s) {
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(list[i], s)) return 1;
    return 0;
}

void vk_dlaa_instance_exts(const char **ext, uint32_t *n, uint32_t max) {
    unsigned ni = 0, nd = 0;
    const char **ie = NULL, **de = NULL;
    if (!vk_dlaa_wanted() || !core_load() || !NGX_OK(N.req_ext(&ni, &ie, &nd, &de))) return;
    for (unsigned i = 0; i < ni; i++)
        if (!listed(ext, *n, ie[i]) && *n < max) ext[(*n)++] = ie[i];
}

void vk_dlaa_device_exts(const VkExtensionProperties *have, uint32_t nhave, const char **ext, uint32_t *n,
                         uint32_t max) {
    unsigned ni = 0, nd = 0;
    const char **ie = NULL, **de = NULL;
    if (!vk_dlaa_wanted() || !core_load() || !NGX_OK(N.req_ext(&ni, &ie, &nd, &de))) return;
    for (unsigned i = 0; i < nd; i++) {
        int ok = 0;
        for (uint32_t k = 0; k < nhave && !ok; k++) ok = !strcmp(have[k].extensionName, de[i]);
        if (!ok) rt_log("dlss: the device lacks %s, which NGX asks for", de[i]);
        else if (!listed(ext, *n, de[i]) && *n < max) ext[(*n)++] = de[i];
    }
}

static int vtable_ok(void) {
    NgxParam *p = NULL;
    if (!NGX_OK(N.alloc_params(&p)) || !p) return 0;
    int i = 0;
    unsigned u = 0;
    float f = 0;
    void *v = NULL;
    p_seti(p, "SR.Test.I", -12345);
    p_setu(p, "SR.Test.U", 4000000000u);
    p_setf(p, "SR.Test.F", 1.25f);
    p_setp(p, "SR.Test.P", (void *)p);
    int ok = p_geti(p, "SR.Test.I", &i) && i == -12345 && p_getu(p, "SR.Test.U", &u) && u == 4000000000u &&
             p_getf(p, "SR.Test.F", &f) && f == 1.25f && p_getp(p, "SR.Test.P", &v) && v == (void *)p;
    if (N.destroy_params) N.destroy_params(p);
    if (!ok) rt_log("dlss: NGX parameter layout check failed: DLSS off");
    return ok;
}

void vk_dlaa_init(void) {
#ifdef _WIN32
    if (!vk_dlaa_wanted() || !core_load()) return;
    wchar_t wdir[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, wdir, MAX_PATH);
    while (len && wdir[len - 1] != L'\\') len--;
    wdir[len] = 0;
    // 231313132: the application id NVIDIA's DLSS samples use for builds without an NVIDIA-issued id
    const VkCtx *c = vk_ctx();
    NgxResult r = N.init(231313132ull, wdir, c->instance, c->phys, c->device, vkGetInstanceProcAddr,
                         vkGetDeviceProcAddr, NGX_VERSION_API, NULL);
    if (!NGX_OK(r)) {
        rt_log("dlss: NGX init failed (%08x): DLSS off", (unsigned)r);
        return;
    }
    if (!vtable_ok()) return;
    NgxParam *caps = NULL;
    int avail = 0, driver = 0;
    if (NGX_OK(N.caps(&caps)) && caps) {
        p_geti(caps, "SuperSampling.Available", &avail);
        p_geti(caps, "SuperSampling.NeedsUpdatedDriver", &driver);
    }
    N.available = avail != 0;
    rt_log("dlss: NGX up; DLSS %s%s", avail ? "available (F8 switches it off and on)"
                                             : "NOT available (is nvngx_dlss.dll next to sr.exe?)",
           driver ? "; the driver needs an update" : "");
#endif
}

int vk_dlaa_available(void) { return vk_dlaa_wanted() && N.available; }

// ---------------------------------------------------------------- state
#define CAM_BLOCK 0x3040u   // the scene's camera block (vertex c3)
#define CUR_VP 48u          // vec4 3-6: this frame's view-projection
#define PREV_VP 384u        // vec4 24-27: last frame's
#define PROJ 112u           // vec4 7-10: the projection (frame generation's constants, sr/vk_dlssg.cpp)
#define VIEW_INV 176u       // vec4 11-13: camera to world (3x4)
#define NEAR_FAR 224u       // vec4 14: near, far
#define ASPECT 248u         // vec4 15 .z: the aspect ratio
#define CAM_BYTES 256u      // a dlaa_mv.comp camera block: vec4 rows 0-15, rows 3-6 read
#define MVM_SLOTS 16u       // frames the GPU may still read the reprojection blocks of
#define PCAM_SLOTS 8u       // the same for last frame's camera block (per-object motion)
#define PCAM_STRIDE 0x3100u // CAM_BLOCK rounded up to 256
#define NSIZES 4

struct Size {   // a DLSS feature and its images per maximum render size and output size
    uint32_t mw, mh, ow, oh;
    VkImage out, mv, mask;
    VmaAllocation out_mem, mv_mem, mask_mem;
    VkImageView out_view, mv_view, mask_view;
    NgxHandle *feat;
    uint64_t used;
};

static struct {
    struct Size sizes[NSIZES], *last;
    NgxParam *params;
    int failed, reset;
    volatile int user_off;            // F8, the menu's DLSS row
    int preset_used;                  // the model the features were made with, + 1; 0 none yet
    uint64_t evals, missed, fallbacks, mv_frames, no_cam;
    // this frame's scene (vk_dlaa_draw): its depth and camera, captured once per scene pass
    VkExecTarget depth;
    double cur_vp[16], prev_vp[16];
    SlCamera fg_cam;                  // the rest of the block, for frame generation
    uint32_t cam_pass;                // ex->pass_serial of the pass the camera was taken in
    int cam_fresh;                    // taken since the last evaluation
    // jitter
    VkImage jdepth;                   // the scene depth DLSS last ran with: passes drawing with it are jittered
    float jx, jy, ux, uy;
    uint32_t jframe;
    int jit_ok, jit_used, hold, evaluated;
    // motion-vector pass
    VkDescriptorSetLayout mv_dsl;
    VkPipelineLayout mv_layout;
    VkPipeline mv_pipe;
    VkSampler mv_samp;
    VkBuffer mvm;
    VmaAllocation mvm_mem;
    uint8_t *mvm_map;
    uint32_t mvm_slot;
    // per-object motion (vk_exec_draw.c vk_mvobj_execute): last frame's camera block for the draws drawn again: this
    // frame's c3 block with vec4 0-10 (view, view-projection, projection) replaced by 21-31, the game's own copy of
    // last frame's, in a ring of PCAM_SLOTS
    VkBuffer pcam;
    VmaAllocation pcam_mem;
    uint8_t *pcam_map;
    uint32_t pcam_slot;
    int pcam_ok;                      // pcam_desc holds this frame's
    VkDescriptorBufferInfo pcam_desc;
    uint64_t cam_addr;                // this frame's scene camera block (c3), the object pass keeps draws with it
    int mv_ok;                        // -1 not built, 0 failed, 1 ready
    uint64_t last_eval_ns;
    uint32_t last_w, last_h;          // the render size of the last evaluation (the overlay shows it)
} D = {.mv_ok = -1};

// Nothing reads DLSS's picture through a texture swap in this port (totk's scaling draw did).
const VkTexImage *vk_dlaa_swap_img;
VkImageView vk_dlaa_swap_view;

static int dl_active(void) { return N.available && !D.failed && !D.user_off; }

// Test switches (environment only): SR_DLSS_JITTER=0, SR_DLSS_MV=0 (zero motion), SR_DLSS_MVDEBUG=1.
static int env_knob(const char *name, int def) {
    const char *v = getenv(name);
    return v && *v ? strcmp(v, "0") != 0 : def;
}

// Off: the game's upscaler runs again; on: DLSS takes its place. F8 and the menu's DLSS row (ui/menu.cpp).
void vk_dlss_set_off(int off) {
    if (!N.available) {
        rt_log("dlss: not available (dlaa = 1 in sr-config.ini, an RTX GPU and nvngx_dlss.dll next to sr.exe)");
        return;
    }
    if (!D.user_off == !off) return;
    D.user_off = off != 0;
    sl_fg_pause(D.user_off);   // frame generation takes its inputs from this pass (sr/vk_dlssg.cpp)
    D.reset = 1, D.jit_ok = 0;
    rt_log("dlss: %s", D.user_off ? "off, the game's upscaler runs" : "on");
}

void vk_dlss_toggle(void) { vk_dlss_set_off(!D.user_off); }
int vk_dlss_is_off(void) { return D.user_off; }

// The performance overlay's DLSS line (ui/menu.cpp): NULL when DLSS is not set up in this run.
const char *vk_dlss_overlay_line(void) {
    static char line[64];
    if (!N.available) return NULL;
    if (D.failed) return "DLSS failed";
    if (D.user_off) return "DLSS off";
    if (!D.evals) return "DLSS on";
    char model = D.preset_used > 1 ? (char)('A' + D.preset_used - 2) : 'K';   // automatic: K at the game's 1.5x
    snprintf(line, sizeof line, "DLSS %ux%u, model %c", D.last_w, D.last_h, model);
    return line;
}

static void dl_fail(const char *what, NgxResult r) {
    rt_log("dlss: %s failed (%08x): DLSS off for this run, the game's upscaler runs", what, (unsigned)r);
    D.failed = 1;
}

// view_f: the view's format when it differs from the image's (the output: an sRGB image written through a UNORM view).
static int image2dv(uint32_t w, uint32_t h, VkFormat f, VkFormat view_f, VkImageUsageFlags usage, VkImage *img,
                    VmaAllocation *mem, VkImageView *view) {
    const VkCtx *c = vk_ctx();
    VkImageCreateInfo ii = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = f,
                            .flags = view_f != f ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0,
                            .extent = {w, h, 1}, .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                            .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    if (vmaCreateImage(c->vma, &ii, &ai, img, mem, NULL) != VK_SUCCESS) return 0;
    VkImageViewCreateInfo vi = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = *img,
                                .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = view_f,
                                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    return vkCreateImageView(c->device, &vi, NULL, view) == VK_SUCCESS;
}

static int image2d(uint32_t w, uint32_t h, VkFormat f, VkImageUsageFlags usage, VkImage *img, VmaAllocation *mem,
                   VkImageView *view) {
    return image2dv(w, h, f, f, usage, img, mem, view);
}

static void free_size(struct Size *e) {
    const VkCtx *c = vk_ctx();
    vkDeviceWaitIdle(c->device);   // the evicted images may still be in flight
    if (e->out_view) vkDestroyImageView(c->device, e->out_view, NULL);
    if (e->mv_view) vkDestroyImageView(c->device, e->mv_view, NULL);
    if (e->mask_view) vkDestroyImageView(c->device, e->mask_view, NULL);
    if (e->out) vmaDestroyImage(c->vma, e->out, e->out_mem);
    if (e->mv) vmaDestroyImage(c->vma, e->mv, e->mv_mem);
    if (e->mask) vmaDestroyImage(c->vma, e->mask, e->mask_mem);
    if (e->feat) N.release(e->feat);
    memset(e, 0, sizeof *e);
}

// The entry for a maximum render size (the size of the input picture's image; dynamic resolution renders a smaller
// rectangle of it) and an output size: DLSS's output, the motion vectors (RG16F) and a stand-in R8 image for the
// shader's mask bindings, which it does not use here. All in GENERAL like every image of the back end.
// The output: the game's upscaler takes a gamma-coded picture (SMAA works on it) and writes linear light, which the
// last pass stores into an sRGB target; DLSS keeps the coding of its input. So DLSS writes through a UNORM view into an
// R8G8B8A8_SRGB image, and the blit into the game's target reads that image as sRGB: the decode to linear the game's
// upscaler did (measured on adjacent frames, SR_DLSS_ALT: the game's output is the sRGB decode of DLSS's).
static struct Size *size_for(VkCommandBuffer cb, uint32_t mw, uint32_t mh, uint32_t ow, uint32_t oh) {
    struct Size *e = NULL, *lru = &D.sizes[0];
    for (int i = 0; i < NSIZES; i++) {
        if (D.sizes[i].mw == mw && D.sizes[i].mh == mh && D.sizes[i].ow == ow && D.sizes[i].oh == oh)
            return &D.sizes[i];
        if (!D.sizes[i].out && !e) e = &D.sizes[i];
        if (D.sizes[i].used < lru->used) lru = &D.sizes[i];
    }
    if (!e) {
        if (D.last == lru) D.last = NULL;
        free_size(e = lru);
    }
    VkImageUsageFlags u = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!image2dv(ow, oh, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, u, &e->out, &e->out_mem, &e->out_view) ||
        !image2d(mw, mh, VK_FORMAT_R16G16_SFLOAT, u | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &e->mv, &e->mv_mem, &e->mv_view) ||
        !image2d(mw, mh, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, &e->mask,
                 &e->mask_mem, &e->mask_view)) {
        free_size(e);
        return NULL;
    }
    VkImageMemoryBarrier2 ib[3];
    VkImage im[3] = {e->out, e->mv, e->mask};
    for (int i = 0; i < 3; i++)
        ib[i] = (VkImageMemoryBarrier2){.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
                                        .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                                        .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                                        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = im[i],
                                        .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 3,
                           .pImageMemoryBarriers = ib};
    vkCmdPipelineBarrier2(cb, &di);
    e->mw = mw, e->mh = mh, e->ow = ow, e->oh = oh;
    return e;
}

static void full_barrier(VkCommandBuffer cb) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2,
                           .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
                           .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                           .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(cb, &di);
}

static NgxResourceVK res_image(VkImageView v, VkImage img, VkFormat f, uint32_t w, uint32_t h, VkImageAspectFlags a,
                               int rw) {
    NgxResourceVK r;
    memset(&r, 0, sizeof r);
    r.r.img = (NgxImageView){v, img, {a, 0, 1, 0, 1}, f, w, h};
    r.type = 0;
    r.rw = rw;
    return r;
}

// ---------------------------------------------------------------- scene, camera and jitter
void vk_dlaa_pass(VkExec *ex) { (void)ex; }

static int scene_pass(const VkExec *ex) {
    const VkExecTarget *z = &ex->depth;
    return z->image && z->format == VK_FORMAT_D32_SFLOAT && z->width >= DLAA_SCENE_MIN_W && !z->is3d &&
           ex->color[0].image && ex->color[0].format == VK_FORMAT_B10G11R11_UFLOAT_PACK32;
}

// Last frame's camera block for the per-object motion pass (vk_dlaa_prev_camera): this frame's with its vec4 0-10 taken
// from 21-31, where the game keeps last frame's view, view-projection and projection.
static void pcam_capture(uint64_t addr) {
    D.pcam_ok = 0;
    D.cam_addr = addr;
    const uint8_t *blk = vk_usnap_bytes(addr, CAM_BLOCK);
    if (!blk) return;
    if (!D.pcam) {
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = PCAM_SLOTS * PCAM_STRIDE,
                                 .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
        VmaAllocationCreateInfo ai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                      .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                               VMA_ALLOCATION_CREATE_MAPPED_BIT};
        VmaAllocationInfo info;
        if (vmaCreateBuffer(vk_ctx()->vma, &bi, &ai, &D.pcam, &D.pcam_mem, &info) != VK_SUCCESS) {
            D.pcam = VK_NULL_HANDLE;
            return;
        }
        D.pcam_map = info.pMappedData;
    }
    uint32_t slot = D.pcam_slot++ % PCAM_SLOTS;
    uint8_t *dst = D.pcam_map + (size_t)slot * PCAM_STRIDE;
    memcpy(dst, blk, CAM_BLOCK);
    memcpy(dst, blk + 21 * 16, 11 * 16);
    vmaFlushAllocation(vk_ctx()->vma, D.pcam_mem, (VkDeviceSize)slot * PCAM_STRIDE, CAM_BLOCK);
    D.pcam_desc = (VkDescriptorBufferInfo){D.pcam, (VkDeviceSize)slot * PCAM_STRIDE, CAM_BLOCK};
    D.pcam_ok = 1;
}

int vk_dlaa_prev_camera(VkDescriptorBufferInfo *out, float jitter[2]) {
    if (!dl_active() || !D.pcam_ok) return 0;
    *out = D.pcam_desc;
    jitter[0] = D.jit_ok ? D.jx : 0.0f, jitter[1] = D.jit_ok ? D.jy : 0.0f;
    return 1;
}

// Each draw: in a scene pass, the camera block of its vertex stage (bank c3) as of the submit (vk_usnap), once per
// pass, and the scene depth.
void vk_dlaa_draw(VkExec *ex) {
    if (!dl_active() || ex->pass_serial == D.cam_pass || !scene_pass(ex)) return;
    const p_bind_buf *b = &ex->ubo[0][0];   // the replay keeps user banks from c3 on
    if (b->size != CAM_BLOCK || !b->addr) return;
    const float *c = (const float *)vk_usnap_bytes(b->addr + CUR_VP, 64);
    const float *p = (const float *)vk_usnap_bytes(b->addr + PREV_VP, 64);
    if (!c || !p) return;
    for (int i = 0; i < 16; i++) D.cur_vp[i] = c[i], D.prev_vp[i] = p[i];
    pcam_capture(b->addr);
    if (sl_fg_active() || fsr_fg_on()) {   // the rest of the camera block, for frame generation
        const float *pj = (const float *)vk_usnap_bytes(b->addr + PROJ, 64);
        const float *vi = (const float *)vk_usnap_bytes(b->addr + VIEW_INV, 48);
        const float *nf = (const float *)vk_usnap_bytes(b->addr + NEAR_FAR, 8);
        const float *as = (const float *)vk_usnap_bytes(b->addr + ASPECT, 4);
        if (pj && vi && nf && as) {
            for (int i = 0; i < 16; i++) D.fg_cam.proj[i] = pj[i];
            for (int i = 0; i < 12; i++) D.fg_cam.view_inv[i] = vi[i];
            D.fg_cam.near_z = nf[0], D.fg_cam.far_z = nf[1], D.fg_cam.aspect = as[0];
        }
    }
    D.depth = ex->depth;
    D.cam_pass = ex->pass_serial, D.cam_fresh = 1;
}

// A depth-only pass onto the scene depth (no colour target): the ink splash's prepass, three draws whose colour pass,
// a few passes later, tests LEQUAL against it. Left without the jitter its depth lay up to half a pixel away from the
// colour draws: on slanted drops and on the splash the colour came out behind its own prepass, which had also hidden
// the scene: black patches the shape of the drops, changing with every jitter phase. SR_DLSS_JITTER_Z=0: as before.
static int scene_depth_pass(const VkExec *ex) {
    static int on = -1;
    if (on < 0) on = env_knob("SR_DLSS_JITTER_Z", 1);
    const VkExecTarget *z = &ex->depth;
    return on && z->image && z->format == VK_FORMAT_D32_SFLOAT && z->width >= DLAA_SCENE_MIN_W && !z->is3d &&
           !ex->color[0].image;
}

void vk_dlaa_viewport(VkExec *ex, VkViewport *v) {
    if (!dl_active() || !D.jit_ok || ex->depth.image != D.jdepth || !(scene_pass(ex) || scene_depth_pass(ex))) return;
    v->x += D.jx, v->y += D.jy;
    D.ux = D.jx, D.uy = D.jy, D.jit_used = 1;
}

static float halton(uint32_t i, uint32_t b) {
    float f = 1, r = 0;
    for (; i; i /= b) f /= (float)b, r += f * (float)(i % b);
    return r;
}

// NVIDIA's DLSS guide: 8 x (output / render)^2 phases, at least 16 (totk vk_dlaa.c next_jitter).
static void next_jitter(uint32_t w, uint32_t ow) {
    double r = w ? (double)ow / w : 1.0;
    uint32_t phases = (uint32_t)ceil(8.0 * r * r - 1e-6);
    if (phases < 16) phases = 16;
    if (phases > 72) phases = 72;
    uint32_t i = D.jframe++ % phases + 1;
    D.jx = halton(i, 2) - 0.5f, D.jy = halton(i, 3) - 0.5f;
    D.jit_ok = !D.hold && env_knob("SR_DLSS_JITTER", 1);
}

// Each present: a frame drawn with jitter that DLSS did not resolve stops the jitter for a second.
void vk_dlaa_frame(void) {
    if (!dl_active()) {
        D.jit_ok = 0;
        return;
    }
    if (!D.evaluated && D.jit_used) D.missed++, D.hold = 60, D.jit_ok = 0;
    else if (D.hold && !--D.hold) D.reset = 1;
    D.evaluated = 0, D.jit_used = 0;
}

int vk_dlaa_skip_draw(VkExec *ex, const GfxShader *fs) { (void)ex, (void)fs; return 0; }

// ---------------------------------------------------------------- motion vectors
static int mv_init(void) {
    if (D.mv_ok >= 0) return D.mv_ok;
    D.mv_ok = 0;
    const VkCtx *c = vk_ctx();
    VkDescriptorSetLayoutBinding b[8] = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL},
        {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
                                           .bindingCount = 8, .pBindings = b};
    VkPushConstantRange pr = {VK_SHADER_STAGE_COMPUTE_BIT, 0, 24};
    VkSamplerCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_NEAREST,
                              .minFilter = VK_FILTER_NEAREST, .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
                              .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
                              .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof dlaa_mv_spv,
                                   .pCode = dlaa_mv_spv};
    VkShaderModule m = VK_NULL_HANDLE;
    VkBufferCreateInfo mbi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = MVM_SLOTS * 2 * CAM_BYTES,
                              .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VmaAllocationCreateInfo mai = {.usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
                                   .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                                            VMA_ALLOCATION_CREATE_MAPPED_BIT};
    VmaAllocationInfo minfo;
    VkPipelineLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                     .pSetLayouts = &D.mv_dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr};
    if (vkCreateDescriptorSetLayout(c->device, &dci, NULL, &D.mv_dsl) != VK_SUCCESS ||
        vmaCreateBuffer(c->vma, &mbi, &mai, &D.mvm, &D.mvm_mem, &minfo) != VK_SUCCESS ||
        vkCreatePipelineLayout(c->device, &li, NULL, &D.mv_layout) != VK_SUCCESS ||
        vkCreateSampler(c->device, &si, NULL, &D.mv_samp) != VK_SUCCESS ||
        vkCreateShaderModule(c->device, &mi, NULL, &m) != VK_SUCCESS) {
        rt_log("dlss: the motion-vector pass could not be set up: DLSS runs with zero motion");
        return 0;
    }
    D.mvm_map = minfo.pMappedData;
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                      .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"},
                                      .layout = D.mv_layout};
    VkResult r = vkCreateComputePipelines(c->device, VK_NULL_HANDLE, 1, &ci, NULL, &D.mv_pipe);
    vkDestroyShaderModule(c->device, m, NULL);
    if (r != VK_SUCCESS) {
        rt_log("dlss: the motion-vector pipeline could not be built: DLSS runs with zero motion");
        return 0;
    }
    return D.mv_ok = 1;
}

// last * inverse(this) in double precision (totk vk_dlaa.c inv4: a float inverse in the shader gave sub-pixel noise on
// distant surfaces that DLSS followed into a shimmer).
static int inv4(const double m[16], double o[16]) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 8; k++) a[r][k] = k < 4 ? m[r * 4 + k] : (k - 4 == r);
    for (int col = 0; col < 4; col++) {
        int piv = col;
        for (int r = col + 1; r < 4; r++)
            if (fabs(a[r][col]) > fabs(a[piv][col])) piv = r;
        if (fabs(a[piv][col]) < 1e-300) return 0;
        for (int k = 0; k < 8; k++) { double t = a[col][k]; a[col][k] = a[piv][k]; a[piv][k] = t; }
        double d = a[col][col];
        for (int k = 0; k < 8; k++) a[col][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != col) {
                double f = a[r][col];
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[col][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) o[r * 4 + k] = a[r][k + 4];
    return 1;
}

// The camera motion of every pixel of the w x h render rectangle into e->mv; 0 when there is no camera for this frame.
static int mv_pass(VkCommandBuffer cb, struct Size *e, VkImageView zv, uint32_t w, uint32_t h, float jx, float jy) {
    double ci[16], m[16];
    if (!mv_init() || !D.cam_fresh || !inv4(D.cur_vp, ci)) return 0;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) {
            double v = 0;
            for (int j = 0; j < 4; j++) v += D.prev_vp[r * 4 + j] * ci[j * 4 + k];
            m[r * 4 + k] = v;
        }
    // dlaa_mv.comp reads rows 3-6 of each block: "this frame" gets the identity, "last frame" the reprojection
    uint32_t ms = D.mvm_slot++ % MVM_SLOTS;
    float *blk = (float *)(D.mvm_map + (size_t)ms * 2 * CAM_BYTES);
    memset(blk, 0, 2 * CAM_BYTES);
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++)
            blk[12 + r * 4 + k] = r == k ? 1.0f : 0.0f, blk[64 + 12 + r * 4 + k] = (float)m[r * 4 + k];
    vmaFlushAllocation(vk_ctx()->vma, D.mvm_mem, (VkDeviceSize)ms * 2 * CAM_BYTES, 2 * CAM_BYTES);
    VkDescriptorBufferInfo cur = {D.mvm, (VkDeviceSize)ms * 2 * CAM_BYTES, CAM_BYTES};
    VkDescriptorBufferInfo prev = {D.mvm, (VkDeviceSize)ms * 2 * CAM_BYTES + CAM_BYTES, CAM_BYTES};
    VkDescriptorImageInfo zi = {D.mv_samp, zv, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo oi = {VK_NULL_HANDLE, e->mv_view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo ki = {D.mv_samp, e->mask_view, VK_IMAGE_LAYOUT_GENERAL};    // unused (bias 0)
    VkDescriptorImageInfo si = {VK_NULL_HANDLE, e->mask_view, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet wr[8];
    for (int i = 0; i < 8; i++)
        wr[i] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = (uint32_t)i,
                                       .descriptorCount = 1};
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[0].pImageInfo = &zi;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wr[1].pImageInfo = &oi;
    wr[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, wr[2].pBufferInfo = &cur;
    wr[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, wr[3].pBufferInfo = &prev;
    wr[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[4].pImageInfo = &ki;
    for (int i = 5; i < 8; i++) wr[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, wr[i].pImageInfo = &si;
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, D.mv_pipe);
    rec_push(vk_pipeline_push_fn(), cb, VK_PIPELINE_BIND_POINT_COMPUTE, D.mv_layout, 0, 8, wr);
    float pc[6] = {(float)w, (float)h, jx, jy, 0.0f, 0.0f};
    vkCmdPushConstants(cb, D.mv_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
    full_barrier(cb);
    D.mv_frames++;
    return 1;
}

// ---------------------------------------------------------------- DLSS in place of the game's upscaler
static int preset_value(void) {
    const char *v = cfg_get("SR_DLSS_PRESET");
    int c = v && v[0] && !v[1] ? v[0] | 0x20 : 0;
    return c >= 'a' && c <= 'm' ? c - 'a' + 1 : 0;
}

static int perf_quality(uint32_t w, uint32_t ow) {
    if (w >= ow) return 5;   // DLAA
    double k = (double)w / ow;
    return k >= 0.66 ? 2 : k >= 0.57 ? 1 : k >= 0.49 ? 0 : 3;
}

int vk_dlss_upscale(VkExec *ex, const VkTexImage *src, const VkTexImage *dst, const uint32_t r[8]) {
    if (!dl_active()) return 0;
    static uint64_t calls;   // SR_DLSS_ALT=1 (test): every other upscale is the game's, to compare the two on one scene
    if (env_knob("SR_DLSS_ALT", 0) && calls++ % 2) return 0;
    uint32_t w = r[2] - r[0] + 1, h = r[3] - r[1] + 1, ow = r[6] - r[4] + 1, oh = r[7] - r[5] + 1;
    const VkExecTarget *z = &D.depth;
    // No scene this frame, a picture DLSS cannot take, or a small one (a 171x96 -> 256x144 upscale at the start, not
    // the scene's picture: dynamic resolution keeps the scene's above half the depth target's width): the game's runs.
    if (r[0] || r[1] || !D.cam_fresh || !z->img || z->width < w || z->height < h || 2 * w < z->width ||
        src->width < w || src->height < h) {
        D.fallbacks++;
        static int logged;
        if (!logged++)
            rt_log("dlss: an upscale DLSS could not take (input %u,%u %ux%u, camera %d, depth %ux%u): the game's runs",
                   r[0], r[1], w, h, D.cam_fresh, z->width, z->height);
        return 0;
    }
    VkCommandBuffer cb = ex->cb;
    vk_exec_barrier(ex);   // ends the pass; the picture and the depth are written
    // One feature for every dynamic resolution level: created at the output image's size for both the render size
    // (the most a frame may render; DLSS takes the frame's rectangle) and the output, which is then scaled into the
    // rectangle the game uses this frame. A feature per size pair churned under dynamic resolution (eleven sizes in
    // a minute with frame generation on): each new one waited for the device and started a new history.
    uint32_t fw = dst->width, fh = dst->height;
    struct Size *e = size_for(cb, fw, fh, fw, fh);
    if (!e) return dl_fail("work image creation", 0), 0;
    if (e != D.last) D.reset = 1;
    if (!D.params && !NGX_OK(N.alloc_params(&D.params))) return dl_fail("parameter allocation", 0), 0;
    NgxParam *p = D.params;
    VkImageView zv = vk_exec_target_view(z->img, z->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_DEPTH_BIT, z->level,
                                         z->base_layer, 1, 0);
    VkImageView cv = vk_exec_target_view(src, src->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0);
    if (!zv || !cv) return dl_fail("input views", 0), 0;
    float jx = D.jit_used ? D.ux : 0.0f, jy = D.jit_used ? D.uy : 0.0f;
    int cam_mv = env_knob("SR_DLSS_MV", 1) && mv_pass(cb, e, zv, w, h, jx, jy);
    // the motion of whatever moves on its own (characters, their shadows' casters, props) over the camera's: the scene
    // draws listed during the replay drawn again with last frame's blocks (vk_exec_draw.c, SR_DLAA_OBJMV=0: off)
    if (cam_mv && vk_mvobj_execute(ex, D.cam_addr, e->mv_view, zv, z->format, w, h)) full_barrier(cb);
    if (!cam_mv) {
        VkClearColorValue zero = {{0, 0, 0, 0}};
        VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, e->mv, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
        full_barrier(cb);
        D.reset = 1, D.no_cam++;
    }
    rec_drain();   // NGX records into the command buffer itself: the recorder must have made every call so far
    // a new model (the menu, SR_DLSS_PRESET): the features made with the old one go and are made again below
    if (D.preset_used && D.preset_used != preset_value() + 1) {
        vkDeviceWaitIdle(vk_ctx()->device);   // they may still be in flight
        for (int i = 0; i < NSIZES; i++)
            if (D.sizes[i].feat) N.release(D.sizes[i].feat), D.sizes[i].feat = NULL;
        D.reset = 1;
        rt_log("dlss: model %s from now on", preset_value() ? cfg_value("SR_DLSS_PRESET") : "automatic");
    }
    D.preset_used = preset_value() + 1;
    if (!e->feat) {
        int preset = preset_value(), pk = preset ? preset : 11, pm = preset ? preset : 13, pl = preset ? preset : 12;
        p_setu(p, "CreationNodeMask", 1);
        p_setu(p, "VisibilityNodeMask", 1);
        p_setu(p, "Width", e->mw);
        p_setu(p, "Height", e->mh);
        p_setu(p, "OutWidth", fw);
        p_setu(p, "OutHeight", fh);
        p_seti(p, "PerfQualityValue", perf_quality(w, ow));   // this frame's ratio (1.5x, Quality, at full size)
        p_seti(p, "DLSS.Feature.Create.Flags", 1 << 1);   // MVLowRes: vectors at the render size; the picture is LDR
        p_seti(p, "DLSS.Enable.Output.Subrects", 0);
        p_setu(p, "DLSS.Hint.Render.Preset.DLAA", (unsigned)pk);
        p_setu(p, "DLSS.Hint.Render.Preset.UltraQuality", (unsigned)pk);
        p_setu(p, "DLSS.Hint.Render.Preset.Quality", (unsigned)pk);
        p_setu(p, "DLSS.Hint.Render.Preset.Balanced", (unsigned)pk);
        p_setu(p, "DLSS.Hint.Render.Preset.Performance", (unsigned)pm);
        p_setu(p, "DLSS.Hint.Render.Preset.UltraPerformance", (unsigned)pl);
        NgxResult rr = N.create(cb, NGX_FEATURE_SUPERSAMPLING, p, &e->feat);
        if (!NGX_OK(rr) || !e->feat) return dl_fail("DLSS feature creation", rr), 0;
        D.reset = 1;
        int q = perf_quality(w, ow);
        rt_log("dlss: feature created for up to %ux%u -> %ux%u (this frame %ux%u -> %ux%u), quality value %d, preset %c, "
               "picture format %d, depth format %d", e->mw, e->mh, fw, fh, w, h, ow, oh, q,
               'A' + (q == 0 ? pm : q == 3 ? pl : pk) - 1, (int)src->format, (int)z->format);
    }
    NgxResourceVK rc = res_image(cv, src->image, src->format, src->width, src->height, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    NgxResourceVK ro = res_image(e->out_view, e->out, VK_FORMAT_R8G8B8A8_UNORM, fw, fh, VK_IMAGE_ASPECT_COLOR_BIT, 1);
    NgxResourceVK rz = res_image(zv, z->image, z->format, z->width, z->height, VK_IMAGE_ASPECT_DEPTH_BIT, 0);
    NgxResourceVK rm = res_image(e->mv_view, e->mv, VK_FORMAT_R16G16_SFLOAT, e->mw, e->mh, VK_IMAGE_ASPECT_COLOR_BIT, 0);
    p_setp(p, "Color", &rc);
    p_setp(p, "Output", &ro);
    p_setp(p, "Depth", &rz);
    p_setp(p, "MotionVectors", &rm);
    p_setf(p, "Jitter.Offset.X", jx);
    p_setf(p, "Jitter.Offset.Y", jy);
    p_setf(p, "Sharpness", 0.0f);
    p_seti(p, "Reset", D.reset);
    p_setp(p, "DLSS.Input.Bias.Current.Color.Mask", NULL);
    p_setf(p, "MV.Scale.X", 1.0f);
    p_setf(p, "MV.Scale.Y", 1.0f);
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Width", w);
    p_setu(p, "DLSS.Render.Subrect.Dimensions.Height", h);
    p_setf(p, "DLSS.Pre.Exposure", 1.0f);
    p_setf(p, "DLSS.Exposure.Scale", 1.0f);
    uint64_t now = plat_now_ns();
    double dt = D.last_eval_ns ? (now - D.last_eval_ns) / 1e6 : 0;
    D.last_eval_ns = now;
    p_setf(p, "FrameTimeDeltaInMsec", dt > 0 && dt < 100 ? (float)dt : 16.67f);
    int fg_reset = D.reset;   // frame generation starts its history over with DLSS
    RING1("dlss evaluate %ux%u", w, h);
    NgxResult rr = N.evaluate(cb, e->feat, p, NULL);
    if (!NGX_OK(rr)) return dl_fail("DLSS evaluation", rr), 0;
    D.reset = 0, D.last = e, e->used = ++D.evals, D.last_w = w, D.last_h = h;
    full_barrier(cb);
    // SR_DLSS_MVDEBUG=1 (test): the motion vectors in place of the picture (x red, y green, render pixels)
    int mvdebug = env_knob("SR_DLSS_MVDEBUG", 0);
    VkImageBlit b = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .srcOffsets = {{0, 0, 0}, {(int32_t)(mvdebug ? w : fw), (int32_t)(mvdebug ? h : fh), 1}},
                     .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
                     .dstOffsets = {{(int32_t)r[4], (int32_t)r[5], 0}, {(int32_t)r[6] + 1, (int32_t)r[7] + 1, 1}}};
    vkCmdBlitImage(cb, mvdebug ? e->mv : e->out, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                   ow == fw && oh == fh && !mvdebug ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    full_barrier(cb);
    if (sl_fg_active() || fsr_fg_on()) {   // frame generation's inputs, before the HUD is drawn into dst (sr/vk_dlssg.cpp, vk_fsrfg.cpp)
        VkImageView hv = vk_exec_target_view(dst, dst->format, VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1, 0);
        SlImage sd = {z->image, zv, z->format, z->width, z->height, w, h, z->img->usage};
        SlImage sm = {e->mv, e->mv_view, VK_FORMAT_R16G16_SFLOAT, e->mw, e->mh, w, h,
                      VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                          VK_IMAGE_USAGE_TRANSFER_DST_BIT};
        SlImage sh = {hv ? dst->image : VK_NULL_HANDLE, hv, dst->format, dst->width, dst->height, ow, oh, dst->usage};
        memcpy(D.fg_cam.cur_vp, D.cur_vp, sizeof D.cur_vp);
        memcpy(D.fg_cam.prev_vp, D.prev_vp, sizeof D.prev_vp);
        D.fg_cam.jx = jx, D.fg_cam.jy = jy, D.fg_cam.reset = fg_reset;
        if (sl_fg_debug_mode() == 1) {   // F6 test: frame generation gets zero vectors (DLSS has read them already)
            VkClearColorValue zero = {{0, 0, 0, 0}};
            VkImageSubresourceRange all = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cb, e->mv, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &all);
            full_barrier(cb);
        }
        rec_drain();   // Streamline records its copies into the command buffer itself
        sl_fg_tag(cb, &sd, &sm, &sh, &D.fg_cam);
        full_barrier(cb);
    }
    ex->recorded += 6;
    vk_exec_draw_new_cb();   // NGX bound pipelines and descriptor sets of its own: the next draw binds everything again
    D.jdepth = z->image, D.evaluated = 1, D.cam_fresh = 0;
    next_jitter(w, ow);
    if (D.evals == 1 || D.evals == 600 || D.evals % 7200 == 0)
        rt_log("dlss: %llu frames (render %ux%u -> %ux%u; camera motion in %llu, none in %llu; %llu jittered frames "
               "missed, %llu upscales left to the game)", (unsigned long long)D.evals, w, h, ow, oh,
               (unsigned long long)D.mv_frames, (unsigned long long)D.no_cam, (unsigned long long)D.missed,
               (unsigned long long)D.fallbacks);
    return 1;
}
