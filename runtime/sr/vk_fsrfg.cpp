// sr-recomp: AMD FSR 3 frame generation on Vulkan (FidelityFX SDK 2.4, FidelityFX API), as an alternative to DLSS-G
// (sr/vk_dlssg.cpp). sr-config.ini fsr_fg = 1 is read at start (the device needs FSR's queues); F7 turns generation off and on.
//
// - amd_fidelityfx_loader_vk.dll is loaded from the folder of sr.exe once WinVerifyTrust accepted it and the signer is
//   Advanced Micro Devices; it loads amd_fidelityfx_framegeneration_vk.dll (also checked) from there.
// - The device gets three more queues for FSR (vk_device.c): present and image acquire in the graphics family, async
//   compute in a compute family. All four FSR queues must be different queues.
// - The swapchain is FSR's frame generation swapchain: sr_vk* in sr/vk_dlssg.cpp send create, images, acquire, present
//   and destroy here. A rebuild (resize, fullscreen) recreates it in place; a new size also makes a new FG context.
// - Each frame the DLSS pass (sr/vk_dlss.c) records FSR's "prepare" (depth, motion vectors, camera) into the replay's
//   command buffer under that frame's number; the present thread configures the frame with the same number right
//   before its present, and FSR generates the extra frame inside that present (frame generation callback).
//   FSR is created with async workload support, so its prepared data is kept per frame number: the replay may prepare
//   frame N+1 before frame N is presented (the runtime's present thread trails the replay by up to a frame).
// - No HUD-less picture yet: the HUD is interpolated with the scene.
// Built only with SR_HAVE_FSRFG (tools/build_runtime.py --fidelityfx); otherwise every entry point answers "off".
#ifdef _WIN32
#include <windows.h>
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#include <vulkan/vulkan.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "vk_dlssg.h"

extern "C" void rt_log(const char *fmt, ...);
extern "C" const char *cfg_get(const char *name);
extern "C" void vk_queue_lock(void);
extern "C" void vk_queue_unlock(void);

#ifndef SR_HAVE_FSRFG
extern "C" {
int fsr_fg_requested(void) { return 0; }
int fsr_fg_on(void) { return 0; }
void fsr_fg_device_ready(VkInstance, VkPhysicalDevice, VkDevice, const FsrQueues *) {}
VkResult fsr_vk_create_swapchain(VkDevice, const VkSwapchainCreateInfoKHR *, const VkAllocationCallbacks *, VkSwapchainKHR *) { return VK_ERROR_INITIALIZATION_FAILED; }
void fsr_vk_destroy_swapchain(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks *) {}
VkResult fsr_vk_get_images(VkDevice, VkSwapchainKHR, uint32_t *, VkImage *) { return VK_ERROR_INITIALIZATION_FAILED; }
VkResult fsr_vk_acquire(VkDevice, VkSwapchainKHR, uint64_t, VkSemaphore, VkFence, uint32_t *) { return VK_ERROR_INITIALIZATION_FAILED; }
VkResult fsr_vk_present(VkQueue, const VkPresentInfoKHR *) { return VK_ERROR_INITIALIZATION_FAILED; }
void fsr_fg_prepare(VkCommandBuffer, const SlImage *, const SlImage *, const SlImage *, const SlCamera *) {}
uint64_t fsr_fg_frame(void) { return 0; }
void fsr_fg_frame_end(void) {}
void fsr_fg_configure(uint64_t) {}
void fsr_fg_toggle(void) {}
void fsr_fg_pause(int) {}
void fsr_fg_debug_cycle(void) {}
int fsr_fg_strict_order(void) { return 0; }
const char *fsr_fg_overlay_line(void) { return nullptr; }
}
#else
#include <softpub.h>
#include <wintrust.h>
#include <wincrypt.h>

#include <ffx_api.h>
#include <ffx_api_types.h>
#include <vk/ffx_api_vk.h>
#include <ffx_framegeneration.h>
#include <vk/ffx_api_framegeneration_vk.h>

namespace {

struct FsrState {
    HMODULE mod;
    PfnFfxCreateContext create;
    PfnFfxDestroyContext destroy;
    PfnFfxConfigure configure;
    PfnFfxQuery query;
    PfnFfxDispatch dispatch;
    int on;                      // DLL loaded, device and queues handed over
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice device;
    FsrQueues q;
    // swapchain context and its replacement functions
    ffxContext sc_ctx;
    VkSwapchainKHR sc;
    VkSwapchainKHR skip_destroy;  // the handle a rebuild kept: the runtime destroys "the old one" right after
    ffxQueryDescSwapchainReplacementFunctionsVK fn;
    ffxCreateContextDescFrameGenerationSwapChainVK sc_desc;   // FSR keeps pointers into these until the context goes
    ffxCreateContextDescFrameGenerationSwapChainVersionVK sc_ver;
    VkSwapchainKHR sc_handle;
    // frame generation context
    ffxContext fg_ctx;
    ffxCreateContextDescFrameGeneration fg_desc;
    ffxCreateBackendVKDesc fg_backend;
    ffxCreateContextDescFrameGenerationVersion fg_ver;
    VkExtent2D ext;
    VkFormat fmt;
    // per frame
    uint64_t frame;              // the frame being built (same role as the Streamline frame token)
    uint64_t prepared;           // last frame number prepared + 1 (0: none)
    volatile uint64_t cfg_frame; // the frame number configured for the present under way
    volatile uint64_t presented; // last frame number presented + 1
    int enabled;                 // F7
    volatile int paused;         // DLSS off (F8): no inputs
    int dbg;                     // F6: 0 normal, 1 debug view, 2 only generated frames, 3 tear lines + pacing, 4 old order, 5 no vectors (optical flow only)
    uint64_t configured, generated;
    int last_cfg_ok;
    CRITICAL_SECTION lock;       // FSR's contexts are not thread safe: prepare (replay thread) vs configure/present
} F{};

wchar_t g_dir[MAX_PATH];

bool signed_by_amd(const wchar_t *path) {
    WINTRUST_FILE_INFO fi = {};
    fi.cbStruct = sizeof fi;
    fi.pcwszFilePath = path;
    GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    WINTRUST_DATA wd = {};
    wd.cbStruct = sizeof wd;
    wd.dwUIChoice = WTD_UI_NONE;
    wd.fdwRevocationChecks = WTD_REVOKE_NONE;
    wd.dwUnionChoice = WTD_CHOICE_FILE;
    wd.pFile = &fi;
    wd.dwStateAction = WTD_STATEACTION_VERIFY;
    LONG r = WinVerifyTrust(NULL, &action, &wd);
    bool ok = false;
    if (r == ERROR_SUCCESS) {
        CRYPT_PROVIDER_DATA *pd = WTHelperProvDataFromStateData(wd.hWVTStateData);
        CRYPT_PROVIDER_SGNR *sg = pd ? WTHelperGetProvSignerFromChain(pd, 0, FALSE, 0) : nullptr;
        CRYPT_PROVIDER_CERT *pc = sg ? WTHelperGetProvCertFromChain(sg, 0) : nullptr;
        wchar_t name[256] = {};
        if (pc && pc->pCert) CertGetNameStringW(pc->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, name, 256);
        ok = !wcsncmp(name, L"Advanced Micro Devices", 22);
        if (!ok) rt_log("fsr-fg: %ls is signed, but not by Advanced Micro Devices: FSR frame generation off", path);
    } else {
        rt_log("fsr-fg: %ls signature check failed (%08lx): FSR frame generation off", path, (unsigned long)r);
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &action, &wd);
    return ok;
}

void ffx_message(uint32_t type, const wchar_t *msg) {
    static unsigned n;
    if (n++ < 200) rt_log("fsr-fg %s: %ls", type == FFX_API_MESSAGE_TYPE_ERROR ? "error" : "warning", msg);
}

// The game queue is used by FSR only inside vkQueuePresentKHR, where the runtime holds the queue lock already (the lock
// is recursive); the other three queues are FSR's own.
VkResult submit_game(uint32_t n, const VkSubmitInfo *si, VkFence f) {
    vk_queue_lock();
    VkResult r = vkQueueSubmit(F.q.game, n, si, f);
    vk_queue_unlock();
    return r;
}

ffxReturnCode_t fg_callback(ffxDispatchDescFrameGeneration *params, void *ctx) {
    F.generated++;
    return F.dispatch((ffxContext *)ctx, &params->header);
}

// The FG context for the current swapchain size and format. Caller holds F.lock.
bool create_fg_context(void) {
    F.fg_ver = {};
    F.fg_ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION_VERSION;
    F.fg_ver.version = FFX_FRAMEGENERATION_VERSION;
    F.fg_backend = {};
    F.fg_backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    F.fg_backend.header.pNext = &F.fg_ver.header;
    F.fg_backend.deviceContext.vkDevice = F.device;
    F.fg_backend.deviceContext.vkPhysicalDevice = F.phys;
    F.fg_backend.deviceContext.vkDeviceProcAddr = vkGetDeviceProcAddr;
    F.fg_desc = {};
    F.fg_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    F.fg_desc.header.pNext = &F.fg_backend.header;
    // async workload support: prepared data is kept per frame number (the replay prepares N+1 before N is presented);
    // the work itself stays on the game queue (allowAsyncWorkloads = false in fsr_fg_configure)
    F.fg_desc.flags = FFX_FRAMEGENERATION_ENABLE_ASYNC_WORKLOAD_SUPPORT;
    if (getenv("SR_FSR_DEBUG_CHECK")) F.fg_desc.flags |= FFX_FRAMEGENERATION_ENABLE_DEBUG_CHECKING;
    F.fg_desc.displaySize = {F.ext.width, F.ext.height};
    F.fg_desc.maxRenderSize = {2560, 1440};   // the game's largest scene (DLSS input at most its output size)
    F.fg_desc.backBufferFormat = ffxApiGetSurfaceFormatVK(F.fmt);
    ffxReturnCode_t r = F.create(&F.fg_ctx, &F.fg_desc.header, nullptr);
    if (r != FFX_API_RETURN_OK) {
        rt_log("fsr-fg: frame generation context creation failed (%u)", (unsigned)r);
        F.fg_ctx = nullptr;
        return false;
    }
    ffxConfigureDescGlobalDebug dbg{};
    dbg.header.type = FFX_API_CONFIGURE_DESC_TYPE_GLOBALDEBUG;
    dbg.fpMessage = ffx_message;
    dbg.debugLevel = FFX_API_CONFIGURE_GLOBALDEBUG_LEVEL_WARNINGS;
    F.configure(&F.fg_ctx, &dbg.header);
    F.prepared = 0;
    rt_log("fsr-fg: frame generation context for %ux%u (format %d)", F.ext.width, F.ext.height, (int)F.fmt);
    return true;
}

// Turns generation off on the swapchain and destroys the FG context. Caller holds F.lock.
void destroy_fg_context(void) {
    if (!F.fg_ctx) return;
    if (F.sc_ctx) {
        ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK w{};
        w.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
        F.dispatch(&F.sc_ctx, &w.header);
    }
    ffxConfigureDescFrameGeneration c{};
    c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    c.swapChain = F.sc;
    c.frameGenerationEnabled = false;
    F.configure(&F.fg_ctx, &c.header);
    F.destroy(&F.fg_ctx, nullptr);
    F.fg_ctx = nullptr;
}

void fill_resource(FfxApiResource *r, const SlImage *im, bool depth) {
    VkImageCreateInfo ci = {};
    ci.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = im->format;
    ci.extent = {im->width, im->height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.usage = im->usage | (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : 0);
    *r = ffxApiGetResourceVK((void *)im->image, ffxApiGetImageResourceDescriptionVK(im->image, ci, 0),
                             FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);   // every image of the back end is in GENERAL
}

}   // namespace

extern "C" {

int fsr_fg_requested(void) {
    const char *v = cfg_get("SR_FSR_FG");
    return v && v[0] == '1' && !v[1];
}

int fsr_fg_on(void) { return F.on; }

void fsr_fg_device_ready(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, const FsrQueues *q) {
    if (!fsr_fg_requested() || F.on) return;
    if (!q || !q->game || !q->present || !q->acquire || !q->async) {
        rt_log("fsr-fg: the device has no separate present / acquire / compute queues for FSR: frame generation off");
        return;
    }
    DWORD len = GetModuleFileNameW(NULL, g_dir, MAX_PATH);
    while (len && g_dir[len - 1] != L'\\') len--;
    g_dir[len] = 0;
    wchar_t loader[MAX_PATH + 64], fg[MAX_PATH + 64];
    swprintf(loader, MAX_PATH + 64, L"%lsamd_fidelityfx_loader_vk.dll", g_dir);
    swprintf(fg, MAX_PATH + 64, L"%lsamd_fidelityfx_framegeneration_vk.dll", g_dir);
    if (GetFileAttributesW(loader) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(fg) == INVALID_FILE_ATTRIBUTES) {
        rt_log("fsr-fg: amd_fidelityfx_loader_vk.dll / amd_fidelityfx_framegeneration_vk.dll not next to sr.exe: off");
        return;
    }
    if (!signed_by_amd(loader) || !signed_by_amd(fg)) return;
    F.mod = LoadLibraryW(loader);
    if (!F.mod) { rt_log("fsr-fg: cannot load the FidelityFX loader (%lu)", GetLastError()); return; }
    F.create = (PfnFfxCreateContext)GetProcAddress(F.mod, "ffxCreateContext");
    F.destroy = (PfnFfxDestroyContext)GetProcAddress(F.mod, "ffxDestroyContext");
    F.configure = (PfnFfxConfigure)GetProcAddress(F.mod, "ffxConfigure");
    F.query = (PfnFfxQuery)GetProcAddress(F.mod, "ffxQuery");
    F.dispatch = (PfnFfxDispatch)GetProcAddress(F.mod, "ffxDispatch");
    if (!F.create || !F.destroy || !F.configure || !F.query || !F.dispatch) {
        rt_log("fsr-fg: the FidelityFX loader lacks the API entry points: off");
        return;
    }
    InitializeCriticalSection(&F.lock);
    F.instance = inst, F.phys = phys, F.device = dev, F.q = *q;
    F.enabled = 1;
    F.on = 1;
    rt_log("fsr-fg: FidelityFX loaded (queues: game family %u, present/acquire family %u, compute family %u); "
           "F7 turns generation off and on", q->game_family, q->present_family, q->async_family);
}

VkResult fsr_vk_create_swapchain(VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a,
                                 VkSwapchainKHR *out) {
    EnterCriticalSection(&F.lock);
    VkResult res = VK_SUCCESS;
    if (F.sc_ctx) {   // rebuild in place: same handle; a new size or format needs a new FG context
        destroy_fg_context();
        VkSwapchainCreateInfoKHR c = *ci;
        c.oldSwapchain = VK_NULL_HANDLE;   // F.sc is FSR's object, not a Vulkan swapchain: FSR builds the real one from c
        VkSwapchainKHR sc = F.sc;
        res = F.fn.pOutCreateSwapchainFFXAPI(d, &c, a, &sc, (void *)F.sc);
        if (res == VK_SUCCESS) {
            F.skip_destroy = F.sc;   // the runtime destroys its "old" swapchain next: that is this one
            F.sc = sc;
        }
    } else {
        F.sc_ver = {};
        F.sc_ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATIONSWAPCHAIN_VERSION_VK;
        F.sc_ver.version = FFX_FRAMEGENERATION_SWAPCHAIN_VK_VERSION;
        F.sc_desc = {};
        F.sc_desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FGSWAPCHAIN_VK;
        F.sc_desc.header.pNext = &F.sc_ver.header;
        F.sc_desc.physicalDevice = F.phys;
        F.sc_desc.device = d;
        F.sc_handle = ci->oldSwapchain;   // FSR destroys the swapchain it replaces
        F.sc_desc.swapchain = &F.sc_handle;
        F.sc_desc.allocator = nullptr;
        F.sc_desc.createInfo = *ci;
        F.sc_desc.gameQueue = {F.q.game, F.q.game_family, nullptr};   // only used inside present (lock held)
        F.sc_desc.asyncComputeQueue = {F.q.async, F.q.async_family, nullptr};
        F.sc_desc.presentQueue = {F.q.present, F.q.present_family, nullptr};
        F.sc_desc.imageAcquireQueue = {F.q.acquire, F.q.acquire_family, nullptr};
        (void)submit_game;
        ffxReturnCode_t r = F.create(&F.sc_ctx, &F.sc_desc.header, nullptr);
        if (r != FFX_API_RETURN_OK) {
            rt_log("fsr-fg: frame generation swapchain creation failed (%u)", (unsigned)r);
            F.sc_ctx = nullptr;
            res = VK_ERROR_INITIALIZATION_FAILED;
        } else {
            F.fn = {};
            F.fn.header.type = FFX_API_QUERY_DESC_TYPE_FGSWAPCHAIN_FUNCTIONS_VK;
            F.query(&F.sc_ctx, &F.fn.header);
            F.sc = F.sc_handle;
            if (ci->oldSwapchain) F.skip_destroy = ci->oldSwapchain;   // already destroyed by FSR
        }
    }
    if (res == VK_SUCCESS) {
        *out = F.sc;
        F.ext = ci->imageExtent;
        F.fmt = ci->imageFormat;
        create_fg_context();
    } else {
        rt_log("fsr-fg: swapchain (re)creation failed (%d)", (int)res);
    }
    LeaveCriticalSection(&F.lock);
    return res;
}

void fsr_vk_destroy_swapchain(VkDevice d, VkSwapchainKHR s, const VkAllocationCallbacks *a) {
    EnterCriticalSection(&F.lock);
    if (s && s == F.skip_destroy) {
        F.skip_destroy = VK_NULL_HANDLE;
    } else if (s && s == F.sc) {
        destroy_fg_context();
        if (F.sc_ctx) {
            ffxDispatchDescFrameGenerationSwapChainWaitForPresentsVK w{};
            w.header.type = FFX_API_DISPATCH_DESC_TYPE_FGSWAPCHAIN_WAIT_FOR_PRESENTS_VK;
            F.dispatch(&F.sc_ctx, &w.header);
            F.destroy(&F.sc_ctx, nullptr);
            F.sc_ctx = nullptr;
        }
        if (F.fn.pOutDestroySwapchainFFXAPI) F.fn.pOutDestroySwapchainFFXAPI(d, s, a, nullptr);
        F.sc = VK_NULL_HANDLE;
    } else if (s) {
        vkDestroySwapchainKHR(d, s, a);
    }
    LeaveCriticalSection(&F.lock);
}

VkResult fsr_vk_get_images(VkDevice d, VkSwapchainKHR s, uint32_t *n, VkImage *im) {
    if (s == F.sc && F.fn.pOutGetSwapchainImagesKHR) return F.fn.pOutGetSwapchainImagesKHR(d, s, n, im);
    return vkGetSwapchainImagesKHR(d, s, n, im);
}

VkResult fsr_vk_acquire(VkDevice d, VkSwapchainKHR s, uint64_t t, VkSemaphore se, VkFence f, uint32_t *i) {
    if (s == F.sc && F.fn.pOutAcquireNextImageKHR) return F.fn.pOutAcquireNextImageKHR(d, s, t, se, f, i);
    return vkAcquireNextImageKHR(d, s, t, se, f, i);
}

VkResult fsr_vk_present(VkQueue q, const VkPresentInfoKHR *pi) {
    if (!F.fn.pOutQueuePresentKHR) return vkQueuePresentKHR(q, pi);
    EnterCriticalSection(&F.lock);   // the FG callback runs inside and uses the FG context
    VkResult r = F.fn.pOutQueuePresentKHR(q, pi);
    LeaveCriticalSection(&F.lock);
    __atomic_store_n(&F.presented, F.cfg_frame + 1, __ATOMIC_RELEASE);
    return r;
}

// FSR's generation for frame N reads the dilated depth and vectors of the *latest* prepare (two slots that flip on
// every prepare, ffx_provider_fsr3framegeneration.cpp), not of frame N: a prepare for N+1 recorded before frame N's
// present would hand frame N the next frame's data (ghosts and streaks). So the prepare waits until the last blitted
// frame went out (its generation is recorded inside that present) and the frame number moved past the last prepare.
static void wait_previous_present(void) {
    if (!fsr_fg_strict_order()) return;   // F6 "old order": as before, for comparison
    LARGE_INTEGER fq, t0, t;
    QueryPerformanceFrequency(&fq);
    QueryPerformanceCounter(&t0);
    static unsigned timeouts;
    for (;;) {
        uint64_t f = __atomic_load_n(&F.frame, __ATOMIC_ACQUIRE), p = __atomic_load_n(&F.presented, __ATOMIC_ACQUIRE);
        if (f >= F.prepared && p >= f) return;
        QueryPerformanceCounter(&t);
        if ((t.QuadPart - t0.QuadPart) * 1000 > 50 * fq.QuadPart) {   // a skipped present: go on rather than stall
            if (timeouts++ < 8) rt_log("fsr-fg: prepare waited 50 ms for the previous present (frame %llu, presented %llu)",
                                       (unsigned long long)f, (unsigned long long)p);
            return;
        }
        SwitchToThread();
    }
}


// The DLSS pass of frame F.frame: FSR's prepare for that frame number, recorded into the replay's command buffer.
void fsr_fg_prepare(VkCommandBuffer cb, const SlImage *depth, const SlImage *mv, const SlImage *hudless,
                    const SlCamera *c) {
    (void)hudless;
    if (!F.on || F.paused) return;
    wait_previous_present();
    EnterCriticalSection(&F.lock);
    if (!F.fg_ctx) { LeaveCriticalSection(&F.lock); return; }
    ffxDispatchDescFrameGenerationPrepareV2 p{};
    p.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    p.frameID = F.frame;
    p.flags = 0;
    p.commandList = (void *)cb;
    p.renderSize = {depth->ew ? depth->ew : depth->width, depth->eh ? depth->eh : depth->height};
    p.jitterOffset = {c->jx, c->jy};
    // Render pixels, this frame's position to last frame's (FSR's convention): the camera's motion plus, since
    // 2026-11-07, each moving object's own (vk_exec_draw.c's per-object pass with last frame's place and bones). With the
    // camera's alone the character, its shadow and walking figures got the scenery's motion and FSR pulled them to the
    // wrong place. F6 mode 5: optical flow only, for comparison.
    p.motionVectorScale = F.dbg == 5 ? FfxApiFloatCoords2D{0.0f, 0.0f} : FfxApiFloatCoords2D{1.0f, 1.0f};
    p.frameTimeDelta = 1000.0f / 60.0f;
    p.reset = c->reset != 0;
    p.cameraNear = c->near_z;
    p.cameraFar = c->far_z;
    p.cameraFovAngleVertical = c->proj[5] != 0 ? 2.0f * atanf(1.0f / (float)c->proj[5]) : 1.0f;
    p.viewSpaceToMetersFactor = 1.0f;
    fill_resource(&p.depth, depth, true);
    fill_resource(&p.motionVectors, mv, false);
    const double *v = c->view_inv;   // rows of camera-to-world: columns are right, up, back (OpenGL looks down -z)
    p.cameraPosition[0] = (float)v[3], p.cameraPosition[1] = (float)v[7], p.cameraPosition[2] = (float)v[11];
    p.cameraRight[0] = (float)v[0], p.cameraRight[1] = (float)v[4], p.cameraRight[2] = (float)v[8];
    p.cameraUp[0] = (float)v[1], p.cameraUp[1] = (float)v[5], p.cameraUp[2] = (float)v[9];
    p.cameraForward[0] = (float)-v[2], p.cameraForward[1] = (float)-v[6], p.cameraForward[2] = (float)-v[10];
    ffxReturnCode_t r = F.dispatch(&F.fg_ctx, &p.header);
    if (r == FFX_API_RETURN_OK) F.prepared = F.frame + 1;
    static int logged;
    if (r != FFX_API_RETURN_OK || !logged++)
        rt_log("fsr-fg: prepare frame %llu (render %ux%u, depth %ux%u, vectors %ux%u): %u", (unsigned long long)F.frame,
               p.renderSize.width, p.renderSize.height, depth->width, depth->height, mv->width, mv->height, (unsigned)r);
    LeaveCriticalSection(&F.lock);
}

uint64_t fsr_fg_frame(void) { return F.frame; }

void fsr_fg_frame_end(void) {
    if (F.on) __atomic_add_fetch(&F.frame, 1, __ATOMIC_RELEASE);
}

// Right before the present of frame `frame`: generation on for it when its prepare went in, off otherwise (menus,
// loading, DLSS off), with that frame number.
void fsr_fg_configure(uint64_t frame) {
    if (!F.on) return;
    F.cfg_frame = frame;
    EnterCriticalSection(&F.lock);
    if (F.fg_ctx && F.sc) {
        ffxConfigureDescFrameGeneration c{};
        c.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
        c.swapChain = F.sc;
        c.frameGenerationCallback = fg_callback;
        c.frameGenerationCallbackUserContext = &F.fg_ctx;
        // prepared data is kept for the last few frame numbers; a frame without it is shown as rendered
        int have = F.prepared && frame + 1 <= F.prepared && frame + 3 >= F.prepared;
        c.frameGenerationEnabled = F.enabled && !F.paused && have;
        c.allowAsyncWorkloads = false;
        c.flags = F.dbg == 1 ? FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_VIEW
                : F.dbg == 3 ? (FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_TEAR_LINES | FFX_FRAMEGENERATION_FLAG_DRAW_DEBUG_PACING_LINES)
                : 0;
        c.onlyPresentGenerated = F.dbg == 2;
        c.generationRect = {0, 0, (int32_t)F.ext.width, (int32_t)F.ext.height};
        c.frameID = frame;
        ffxReturnCode_t r = F.configure(&F.fg_ctx, &c.header);
        F.configured++;
        F.last_cfg_ok = c.frameGenerationEnabled && r == FFX_API_RETURN_OK;
        if (F.configured % 1200 == 0)
            rt_log("fsr-fg: %llu frames configured, %llu generated frames dispatched, generation %s",
                   (unsigned long long)F.configured, (unsigned long long)F.generated, F.last_cfg_ok ? "on" : "off");
        static int logged;
        if (r != FFX_API_RETURN_OK && logged++ < 8) rt_log("fsr-fg: configure frame %llu failed (%u)", (unsigned long long)frame, (unsigned)r);
    }
    LeaveCriticalSection(&F.lock);
}

void fsr_fg_toggle(void) {
    if (!F.on) return;
    F.enabled = !F.enabled;
    rt_log("fsr-fg: generation %s (F7)", F.enabled ? "on" : "off");
}

void fsr_fg_pause(int paused) {
    if (F.on) F.paused = paused;
}

void fsr_fg_debug_cycle(void) {
    if (!F.on) return;
    F.dbg = (F.dbg + 1) % 6;
    rt_log("fsr-fg: F6 view %d", F.dbg);
}

int fsr_fg_strict_order(void) { return F.on && F.enabled && !F.paused && F.dbg != 4; }

const char *fsr_fg_overlay_line(void) {
    if (!F.on) return nullptr;
    static char line[72];
    static const char *const dbg[6] = {"", "  F6 DEBUG VIEW", "  F6 GEN ONLY", "  F6 PACING LINES", "  F6 OLD ORDER", "  F6 ZERO MV"};
    if (!F.enabled) return "FSR FG OFF";
    snprintf(line, sizeof line, "FSR FG 2X  %s%s", F.last_cfg_ok ? "GENERATING" : "WAITING", dbg[F.dbg]);
    return line;
}

}   // extern "C"
#endif
