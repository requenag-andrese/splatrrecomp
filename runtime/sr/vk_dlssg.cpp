// sr-recomp: DLSS Frame Generation (DLSS-G, multi frame generation up to 4x) through NVIDIA Streamline 2.14, manual
// hooking (docs/ProgrammingGuideManualHooking.md of the SDK). sr-config.ini dlss_fg = 2 | 3 | 4 (or off, the default) is
// read at start, since Streamline must be up before the Vulkan instance exists; F7 cycles off / 2x / 3x / 4x live.
//
// - sl.interposer.dll is loaded from the folder of sr.exe after WinVerifyTrust accepted its signature and the signer is
//   NVIDIA Corporation; the plugins (sl.common, sl.dlss_g, sl.reflex, sl.pcl, nvngx_dlssg) are taken from there too.
//   Over-the-air updates are off: only the verified files run.
// - The runtime keeps calling vulkan-1.dll; only the calls Streamline intercepts go through its proxies (sr_vk*).
//   Its vkCreateDevice adds the extensions, features and queues DLSS-G needs (the optical flow queue).
// - Each frame the DLSS pass (sr/vk_dlss.c) tags the scene depth, the motion vectors and the HUD-less picture (the
//   upscaled picture before the game draws its HUD into it) and sets the camera constants; the present path sets the
//   Reflex / PCL markers that tie the tags to the frame Streamline presents with its generated frames.
// Built only with SR_HAVE_STREAMLINE (tools/build_runtime.py --streamline); without it the wrappers call the driver.
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
extern "C" int cfg_bool(const char *name);
extern "C" const char *cfg_get(const char *name);
extern "C" void cfg_set(const char *name, const char *value);
extern "C" int cfg_save(void);

#ifndef SR_HAVE_STREAMLINE
// ---------------------------------------------------------------- no Streamline in this build
extern "C" {
void sl_fg_preinit(void) {
    if (fsr_fg_requested()) return;   // FSR 3 frame generation instead (sr/vk_fsrfg.cpp)
}
int sl_fg_requested(void) { return 0; }
void sl_fg_device_ready(VkPhysicalDevice) {}
int sl_fg_active(void) { return 0; }
VkResult sr_vkCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *o) { return vkCreateInstance(ci, a, o); }
VkResult sr_vkCreateDevice(VkPhysicalDevice p, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *o) { return vkCreateDevice(p, ci, a, o); }
VkResult sr_vkEnumeratePhysicalDevices(VkInstance i, uint32_t *n, VkPhysicalDevice *pd) { return vkEnumeratePhysicalDevices(i, n, pd); }
VkResult sr_vkCreateWin32SurfaceKHR(VkInstance i, const void *ci, const VkAllocationCallbacks *a, VkSurfaceKHR *o) {
    auto f = (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(i, "vkCreateWin32SurfaceKHR");
    return f ? f(i, (const VkWin32SurfaceCreateInfoKHR *)ci, a, o) : VK_ERROR_EXTENSION_NOT_PRESENT;
}
void sr_vkDestroySurfaceKHR(VkInstance i, VkSurfaceKHR s, const VkAllocationCallbacks *a) { vkDestroySurfaceKHR(i, s, a); }
VkResult sr_vkCreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *o) {
    if (fsr_fg_on()) return fsr_vk_create_swapchain(d, ci, a, o); return vkCreateSwapchainKHR(d, ci, a, o); }
void sr_vkDestroySwapchainKHR(VkDevice d, VkSwapchainKHR s, const VkAllocationCallbacks *a) {
    if (fsr_fg_on()) { fsr_vk_destroy_swapchain(d, s, a); return; } vkDestroySwapchainKHR(d, s, a); }
VkResult sr_vkGetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR s, uint32_t *n, VkImage *im) {
    if (fsr_fg_on()) return fsr_vk_get_images(d, s, n, im); return vkGetSwapchainImagesKHR(d, s, n, im); }
VkResult sr_vkAcquireNextImageKHR(VkDevice d, VkSwapchainKHR s, uint64_t t, VkSemaphore se, VkFence f, uint32_t *i) {
    if (fsr_fg_on()) return fsr_vk_acquire(d, s, t, se, f, i); return vkAcquireNextImageKHR(d, s, t, se, f, i); }
VkResult sr_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi) {
    if (fsr_fg_on()) return fsr_vk_present(q, pi); return vkQueuePresentKHR(q, pi); }
VkResult sr_vkDeviceWaitIdle(VkDevice d) { return vkDeviceWaitIdle(d); }
void sl_fg_tag(VkCommandBuffer cb, const SlImage *depth, const SlImage *mv, const SlImage *hudless, const SlCamera *c) {
    if (fsr_fg_on()) { fsr_fg_prepare(cb, depth, mv, hudless, c); return; }}
void sl_fg_render_end(void) {}
void sl_fg_present(int) {}
void sl_fg_frame_end(void) {
    if (fsr_fg_on()) { fsr_fg_frame_end(); return; }}
uint64_t sl_fg_present_token(void) {
    if (fsr_fg_on()) return fsr_fg_frame(); return 0; }
void sl_fg_present_as(uint64_t frame, int start) {
    if (fsr_fg_on()) { if (start) fsr_fg_configure(frame); return; }}
void sl_fg_cycle(void) {
    if (fsr_fg_on()) { fsr_fg_toggle(); return; }}
void sl_fg_pause(int paused) {
    if (fsr_fg_on()) { fsr_fg_pause(paused); return; }}
const char *sl_fg_overlay_line(void) { return fsr_fg_on() ? fsr_fg_overlay_line() : nullptr; }
void sl_fg_debug_cycle(void) {
    if (fsr_fg_on()) { fsr_fg_debug_cycle(); return; }}
int sl_fg_debug_mode(void) { return 0; }
int sl_fg_strict_order(void) { return fsr_fg_on() ? fsr_fg_strict_order() : 0; }
}
#else
// ---------------------------------------------------------------- Streamline
#include <softpub.h>
#include <wintrust.h>
#include <wincrypt.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_hooks.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

namespace {

struct SlState {
    HMODULE mod;
    PFun_slInit *init;
    PFun_slShutdown *shutdown;
    PFun_slIsFeatureSupported *supported;
    PFun_slGetFeatureRequirements *reqs;
    PFun_slSetTagForFrame *tag;
    PFun_slSetConstants *constants;
    PFun_slGetFeatureFunction *feature_fn;
    PFun_slGetNewFrameToken *token;
    PFun_slDLSSGSetOptions *dlssg_set;
    PFun_slDLSSGGetState *dlssg_state;
    PFun_slReflexSetOptions *reflex_set;
    PFun_slPCLSetMarker *marker;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
    // proxies of the intercepted calls (sl_hooks.h)
    PFN_vkCreateInstance create_instance;
    PFN_vkCreateDevice create_device;
    PFN_vkEnumeratePhysicalDevices enumerate;
    PFN_vkCreateWin32SurfaceKHR create_surface;
    PFN_vkDestroySurfaceKHR destroy_surface;
    PFN_vkCreateSwapchainKHR create_swapchain;
    PFN_vkDestroySwapchainKHR destroy_swapchain;
    PFN_vkGetSwapchainImagesKHR swapchain_images;
    PFN_vkAcquireNextImageKHR acquire;
    PFN_vkQueuePresentKHR present;
    PFN_vkDeviceWaitIdle wait_idle;
    int up;                  // slInit succeeded: the proxies are in use
    int supported_fg;        // DLSS-G supported on this device
    int mult;                // 1 off, 2..4
    int max_mult;            // what the GPU allows (DLSSGState::numFramesToGenerateMax + 1)
    volatile int mult_dirty; // F7 changed it: options are set from the replay thread
    volatile int paused;     // DLSS is off (F8): no inputs, generation off
    volatile int dbg;        // F6 test mode (sl_fg_debug_cycle)
    uint32_t bb_w, bb_h;     // the swapchain's size: the HUD-less picture is tagged only when it matches
    uint64_t frame;          // the frame being built (PCL frame index)
    int sim_ended;           // eSimulationEnd / eRenderSubmitStart set for `frame`
    uint64_t tagged;         // frames tagged
    uint32_t presented;      // DLSSGState::numFramesActuallyPresented of the last check
    int status;              // DLSSGState::status of the last check
    VkInstance instance;
} S{};   // all zero: Streamline not up; sl_fg_preinit sets mult, sl_fg_device_ready max_mult

int wanted_mult(void) {
    const char *v = cfg_get("SR_DLSS_FG");
    if (!v || !*v) return 1;
    int n = v[0] - '0';
    return !v[1] && n >= 2 && n <= 4 ? n : 1;
}

// WinVerifyTrust on the file, then the signer's name from the signature's certificate (the SDK's sl_security.h does the
// same with structures mingw's headers do not have).
bool signed_by_nvidia(const wchar_t *path) {
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
        if (pc && pc->pCert)
            CertGetNameStringW(pc->pCert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, NULL, name, 256);
        ok = !wcscmp(name, L"NVIDIA Corporation");
        if (!ok) rt_log("dlss-fg: sl.interposer.dll is signed, but not by NVIDIA Corporation: Streamline off");
    } else {
        rt_log("dlss-fg: sl.interposer.dll signature check failed (%08lx): Streamline off", (unsigned long)r);
    }
    wd.dwStateAction = WTD_STATEACTION_CLOSE;
    WinVerifyTrust(NULL, &action, &wd);
    return ok;
}

// Streamline's warnings and errors into our log; a message from the same source line is logged 3 times, then at every
// power of two (some repeat each frame: "missing common constants" while no 3D scene is drawn).
void sl_log(sl::LogType type, const char *msg) {
    if (type != sl::LogType::eError && type != sl::LogType::eWarn) return;
    static struct { char where[64]; uint32_t n; } seen[32];
    const char *b = strstr(msg, ".cpp:");
    char where[64] = "";
    if (b) {
        const char *a = b;
        while (a > msg && a[-1] != ']') a--;
        const char *e = strchr(b, '[');
        size_t len = e ? (size_t)(e - a) : 0;
        if (len && len < sizeof where) memcpy(where, a, len), where[len] = 0;
    }
    uint32_t count = 1;
    if (where[0]) {
        int i = 0;
        while (i < 32 && seen[i].where[0] && strcmp(seen[i].where, where)) i++;
        if (i < 32) {
            if (!seen[i].where[0]) strcpy(seen[i].where, where);
            count = ++seen[i].n;
        }
    }
    if (count > 3 && (count & (count - 1))) return;
    size_t n = strlen(msg);
    while (n && (msg[n - 1] == '\n' || msg[n - 1] == '\r')) n--;
    rt_log("streamline %s%s: %.*s", type == sl::LogType::eError ? "error" : "warning", count > 3 ? " (repeated)" : "",
           (int)n, msg);
}


sl::FrameToken *token_for(uint64_t frame) {
    sl::FrameToken *t = nullptr;
    uint32_t idx = (uint32_t)frame;
    if (S.token(t, &idx) != sl::Result::eOk) return nullptr;
    return t;
}

void mark(sl::PCLMarker m, uint64_t frame) {
    if (!S.marker) return;
    sl::FrameToken *t = token_for(frame);
    if (t) S.marker(m, *t);
}

void set_options(void) {
    if (!S.dlssg_set) return;
    sl::DLSSGOptions o{};
    o.mode = S.mult > 1 && !S.paused ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
    o.numFramesToGenerate = (uint32_t)(S.mult > 1 ? S.mult - 1 : 1);
    if (S.dbg == 4) o.flags = sl::DLSSGFlags::eShowOnlyInterpolatedFrame;   // F6 mode 4: only the generated frames
    sl::Result r = S.dlssg_set(sl::ViewportHandle(0), o);
    rt_log("dlss-fg: %s (result %d)", S.mult > 1 ? (S.mult == 2 ? "2x" : S.mult == 3 ? "3x" : "4x") : "off", (int)r);
}

// column-vector matrix (row-major) -> Streamline's row-major for row vectors, with clip depth -w..w taken to 0..w
// (d = z/2 + w/2: the depth buffer's convention): S * m, transposed.
void to_sl(sl::float4x4 &out, const double m[16]) {
    double s[16];
    for (int k = 0; k < 4; k++) {
        s[0 * 4 + k] = m[0 * 4 + k];
        s[1 * 4 + k] = m[1 * 4 + k];
        s[2 * 4 + k] = 0.5 * m[2 * 4 + k] + 0.5 * m[3 * 4 + k];
        s[3 * 4 + k] = m[3 * 4 + k];
    }
    for (int r = 0; r < 4; r++)
        out.row[r] = sl::float4((float)s[0 * 4 + r], (float)s[1 * 4 + r], (float)s[2 * 4 + r], (float)s[3 * 4 + r]);
}

int inv4(const double m[16], double o[16]) {
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

void mul4(const double a[16], const double b[16], double o[16]) {
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) {
            double v = 0;
            for (int j = 0; j < 4; j++) v += a[r * 4 + j] * b[j * 4 + k];
            o[r * 4 + k] = v;
        }
}

sl::Resource resource(const SlImage *im, uint32_t aspect) {
    sl::Resource r(sl::ResourceType::eTex2d, (void *)im->image, nullptr, (void *)im->view, VK_IMAGE_LAYOUT_GENERAL);
    r.width = im->width, r.height = im->height, r.nativeFormat = (uint32_t)im->format;
    r.mipLevels = 1, r.arrayLayers = 1, r.flags = 0, r.usage = (uint32_t)im->usage;
    (void)aspect;
    return r;
}

}   // namespace

extern "C" {

// Streamline's Vulkan back end names its objects through VK_EXT_debug_utils; without the extension sl.common calls a
// null vkSetDebugUtilsObjectNameEXT while its plugins start (sl.common.dll+0x565c0, 2.14.16), so the instance gets it.
int sl_fg_requested(void) { return wanted_mult() > 1; }

void sl_fg_preinit(void) {
    if (fsr_fg_requested()) return;   // FSR 3 frame generation instead (sr/vk_fsrfg.cpp)
    int mult = wanted_mult();
    if (mult <= 1) return;
    wchar_t dir[MAX_PATH], path[MAX_PATH + 32];
    DWORD len = GetModuleFileNameW(NULL, dir, MAX_PATH);
    while (len && dir[len - 1] != L'\\') len--;
    dir[len] = 0;
    swprintf(path, MAX_PATH + 32, L"%lssl.interposer.dll", dir);
    if (GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
        rt_log("dlss-fg: sl.interposer.dll is not next to sr.exe (tools/build_runtime.py --streamline): off");
        return;
    }
    if (!signed_by_nvidia(path)) return;
    S.mod = LoadLibraryW(path);
    if (!S.mod) return rt_log("dlss-fg: cannot load sl.interposer.dll (error %lu): off", GetLastError());
#define FN(f, name) S.f = (decltype(S.f))GetProcAddress(S.mod, name)
    FN(init, "slInit");
    FN(shutdown, "slShutdown");
    FN(supported, "slIsFeatureSupported");
    FN(reqs, "slGetFeatureRequirements");
    FN(tag, "slSetTagForFrame");
    FN(constants, "slSetConstants");
    FN(feature_fn, "slGetFeatureFunction");
    FN(token, "slGetNewFrameToken");
    FN(gipa, "vkGetInstanceProcAddr");
    FN(gdpa, "vkGetDeviceProcAddr");
#undef FN
    if (!S.init || !S.tag || !S.constants || !S.feature_fn || !S.token || !S.gipa || !S.gdpa)
        return rt_log("dlss-fg: sl.interposer.dll lacks entry points: off");
    static const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
    static wchar_t plugins_dir[MAX_PATH], logs_dir[MAX_PATH + 8];
    wcscpy(plugins_dir, dir);
    swprintf(logs_dir, MAX_PATH + 8, L"%lslogs", dir);
    static const wchar_t *paths[] = {plugins_dir};
    sl::Preferences p{};
    p.showConsole = false;
    p.logLevel = sl::LogLevel::eDefault;
    p.pathsToPlugins = paths;
    p.numPathsToPlugins = 1;
    p.pathToLogsAndData = logs_dir;
    p.logMessageCallback = sl_log;
    // no over-the-air downloads: only the verified files next to sr.exe are loaded
    p.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking |
              sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    p.featuresToLoad = features;
    p.numFeaturesToLoad = 3;
    p.engine = sl::EngineType::eCustom;
    p.engineVersion = "sr-recomp";
    p.projectId = "6f0c3b8e-5a1d-4c2e-9b7f-2d4e8a1c5b90";
    p.renderAPI = sl::RenderAPI::eVulkan;
    sl::Result r = S.init(p, sl::kSDKVersion);
    if (r != sl::Result::eOk) return rt_log("dlss-fg: slInit failed (%d): off", (int)r);
    S.create_instance = (PFN_vkCreateInstance)S.gipa(VK_NULL_HANDLE, "vkCreateInstance");
    S.up = S.create_instance != nullptr;
    S.mult = mult;
    rt_log("dlss-fg: Streamline up (manual hooking, plugins and logs next to sr.exe), %dx asked", mult);
}

VkResult sr_vkCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *o) {
    if (!S.up) return vkCreateInstance(ci, a, o);
    VkResult r = S.create_instance(ci, a, o);
    if (r == VK_SUCCESS) {
        S.instance = *o;
        S.create_device = (PFN_vkCreateDevice)S.gipa(*o, "vkCreateDevice");
        S.enumerate = (PFN_vkEnumeratePhysicalDevices)S.gipa(*o, "vkEnumeratePhysicalDevices");
        S.create_surface = (PFN_vkCreateWin32SurfaceKHR)S.gipa(*o, "vkCreateWin32SurfaceKHR");
        S.destroy_surface = (PFN_vkDestroySurfaceKHR)S.gipa(*o, "vkDestroySurfaceKHR");
    }
    return r;
}

VkResult sr_vkEnumeratePhysicalDevices(VkInstance i, uint32_t *n, VkPhysicalDevice *pd) {
    return S.up && S.enumerate ? S.enumerate(i, n, pd) : vkEnumeratePhysicalDevices(i, n, pd);
}

VkResult sr_vkCreateDevice(VkPhysicalDevice p, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *o) {
    if (!S.up || !S.create_device) return vkCreateDevice(p, ci, a, o);
    VkResult r = S.create_device(p, ci, a, o);
    if (r == VK_SUCCESS) {
#define DP(f, name) S.f = (decltype(S.f))S.gdpa(*o, name)
        DP(create_swapchain, "vkCreateSwapchainKHR");
        DP(destroy_swapchain, "vkDestroySwapchainKHR");
        DP(swapchain_images, "vkGetSwapchainImagesKHR");
        DP(acquire, "vkAcquireNextImageKHR");
        DP(present, "vkQueuePresentKHR");
        DP(wait_idle, "vkDeviceWaitIdle");
#undef DP
    } else {
        rt_log("dlss-fg: Streamline's vkCreateDevice failed (%d)", (int)r);
    }
    return r;
}

void sl_fg_device_ready(VkPhysicalDevice phys) {
    if (!S.up) return;
    sl::AdapterInfo ai{};
    ai.vkPhysicalDevice = (void *)phys;
    sl::Result r = S.supported ? S.supported(sl::kFeatureDLSS_G, ai) : sl::Result::eErrorFeatureNotSupported;
    if (r != sl::Result::eOk) {
        rt_log("dlss-fg: DLSS Frame Generation not supported here (result %d; an RTX 40/50 GPU, a recent driver and "
               "hardware-accelerated GPU scheduling are needed): off", (int)r);
        return;
    }
    void *f = nullptr;
    if (S.feature_fn(sl::kFeatureDLSS_G, "slDLSSGSetOptions", f) == sl::Result::eOk) S.dlssg_set = (PFun_slDLSSGSetOptions *)f;
    if (S.feature_fn(sl::kFeatureDLSS_G, "slDLSSGGetState", f) == sl::Result::eOk) S.dlssg_state = (PFun_slDLSSGGetState *)f;
    if (S.feature_fn(sl::kFeatureReflex, "slReflexSetOptions", f) == sl::Result::eOk) S.reflex_set = (PFun_slReflexSetOptions *)f;
    if (S.feature_fn(sl::kFeaturePCL, "slPCLSetMarker", f) == sl::Result::eOk) S.marker = (PFun_slPCLSetMarker *)f;
    if (!S.dlssg_set || !S.marker) return rt_log("dlss-fg: DLSS-G or PCL functions missing: off");
    if (S.reflex_set) {   // Reflex low latency: frame generation adds latency, Reflex takes some back
        sl::ReflexOptions ro{};
        ro.mode = sl::ReflexMode::eLowLatency;
        S.reflex_set(ro);
    }
    if (S.dlssg_state) {
        sl::DLSSGState st{};
        if (S.dlssg_state(sl::ViewportHandle(0), st, nullptr) == sl::Result::eOk) {
            S.max_mult = (int)st.numFramesToGenerateMax + 1;
            if (S.max_mult < 2) S.max_mult = 2;
            if (S.max_mult > 4) S.max_mult = 4;
        }
    }
    if (S.mult > S.max_mult) S.mult = S.max_mult;
    S.supported_fg = 1;
    rt_log("dlss-fg: DLSS Frame Generation supported, up to %dx on this GPU (F7 cycles off / 2x / 3x / 4x)", S.max_mult);
    set_options();
}

int sl_fg_active(void) { return S.up && S.supported_fg; }

VkResult sr_vkCreateWin32SurfaceKHR(VkInstance i, const void *ci, const VkAllocationCallbacks *a, VkSurfaceKHR *o) {
    PFN_vkCreateWin32SurfaceKHR f = S.up && S.create_surface ? S.create_surface
                                                              : (PFN_vkCreateWin32SurfaceKHR)vkGetInstanceProcAddr(i, "vkCreateWin32SurfaceKHR");
    return f ? f(i, (const VkWin32SurfaceCreateInfoKHR *)ci, a, o) : VK_ERROR_EXTENSION_NOT_PRESENT;
}
void sr_vkDestroySurfaceKHR(VkInstance i, VkSurfaceKHR s, const VkAllocationCallbacks *a) {
    if (S.up && S.destroy_surface) S.destroy_surface(i, s, a);
    else vkDestroySurfaceKHR(i, s, a);
}
VkResult sr_vkCreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *o) {
    if (fsr_fg_on()) return fsr_vk_create_swapchain(d, ci, a, o);
    S.bb_w = ci->imageExtent.width, S.bb_h = ci->imageExtent.height;
    return S.up && S.create_swapchain ? S.create_swapchain(d, ci, a, o) : vkCreateSwapchainKHR(d, ci, a, o);
}
void sr_vkDestroySwapchainKHR(VkDevice d, VkSwapchainKHR s, const VkAllocationCallbacks *a) {
    if (fsr_fg_on()) { fsr_vk_destroy_swapchain(d, s, a); return; }
    if (S.up && S.destroy_swapchain) S.destroy_swapchain(d, s, a);
    else vkDestroySwapchainKHR(d, s, a);
}
VkResult sr_vkGetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR s, uint32_t *n, VkImage *im) {
    if (fsr_fg_on()) return fsr_vk_get_images(d, s, n, im);
    return S.up && S.swapchain_images ? S.swapchain_images(d, s, n, im) : vkGetSwapchainImagesKHR(d, s, n, im);
}
VkResult sr_vkAcquireNextImageKHR(VkDevice d, VkSwapchainKHR s, uint64_t t, VkSemaphore se, VkFence f, uint32_t *i) {
    if (fsr_fg_on()) return fsr_vk_acquire(d, s, t, se, f, i);
    return S.up && S.acquire ? S.acquire(d, s, t, se, f, i) : vkAcquireNextImageKHR(d, s, t, se, f, i);
}
VkResult sr_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi) {
    if (fsr_fg_on()) return fsr_vk_present(q, pi);
    return S.up && S.present ? S.present(q, pi) : vkQueuePresentKHR(q, pi);
}
VkResult sr_vkDeviceWaitIdle(VkDevice d) {
    return S.up && S.wait_idle ? S.wait_idle(d) : vkDeviceWaitIdle(d);
}

// ---- per frame
void sl_fg_tag(VkCommandBuffer cb, const SlImage *depth, const SlImage *mv, const SlImage *hudless, const SlCamera *c) {
    if (fsr_fg_on()) { fsr_fg_prepare(cb, depth, mv, hudless, c); return; }
    if (!sl_fg_active()) return;
    if (S.mult_dirty) S.mult_dirty = 0, set_options();
    if (!S.sim_ended) mark(sl::PCLMarker::eSimulationEnd, S.frame), mark(sl::PCLMarker::eRenderSubmitStart, S.frame),
                      S.sim_ended = 1;
    sl::FrameToken *t = token_for(S.frame);
    if (!t) return;
    sl::Resource rd = resource(depth, VK_IMAGE_ASPECT_DEPTH_BIT), rm = resource(mv, VK_IMAGE_ASPECT_COLOR_BIT);
    sl::Resource rh = resource(hudless, VK_IMAGE_ASPECT_COLOR_BIT);
    sl::Extent ed{0, 0, depth->ew ? depth->ew : depth->width, depth->eh ? depth->eh : depth->height};
    sl::Extent em{0, 0, mv->ew ? mv->ew : mv->width, mv->eh ? mv->eh : mv->height};
    sl::Extent eh{0, 0, hudless->ew ? hudless->ew : hudless->width, hudless->eh ? hudless->eh : hudless->height};
    // copied now: the HUD is drawn into the HUD-less image afterwards, the next frame reuses the others
    sl::ResourceTag tags[3] = {sl::ResourceTag(&rd, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &ed),
                               sl::ResourceTag(&rm, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &em),
                               sl::ResourceTag(&rh, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eOnlyValidNow, &eh)};
    // The HUD-less picture must match the backbuffer pixel for pixel (Streamline's DLSS-G guide, 5.1). It does in
    // full screen at the game's output size; in a window, or while dynamic resolution shrinks the output rectangle,
    // tagging it misled the interpolation (heavy smudging): then only depth and vectors go.
    int hud_ok = hudless->image && eh.width == S.bb_w && eh.height == S.bb_h && S.dbg != 3;   // F6 mode 3: no HUD-less picture
    uint32_t ntags = hud_ok ? 3 : 2;
    sl::Result r = S.tag(*t, sl::ViewportHandle(0), tags, ntags, (sl::CommandBuffer *)cb);
    // constants: Streamline's matrices are for row vectors, with clip depth 0..1
    double ci[16], m[16];
    sl::Constants k{};
    to_sl(k.cameraViewToClip, c->proj);
    double pinv[16];
    if (inv4(c->proj, pinv)) {   // clip (0..1 depth) to view: inverse of S * P = P^-1 * S^-1
        double sinv[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2, -1, 0, 0, 0, 1}, o[16];
        mul4(pinv, sinv, o);
        for (int rr = 0; rr < 4; rr++)
            k.clipToCameraView.row[rr] = sl::float4((float)o[0 * 4 + rr], (float)o[1 * 4 + rr], (float)o[2 * 4 + rr], (float)o[3 * 4 + rr]);
    }
    if (inv4(c->cur_vp, ci)) {
        mul4(c->prev_vp, ci, m);   // this clip -> last clip (OpenGL depth)
        // in 0..1 depth: S * m * S^-1, then for row vectors
        double sinv[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 2, -1, 0, 0, 0, 1}, ms[16], back[16];
        mul4(m, sinv, ms);
        to_sl(k.clipToPrevClip, ms);
        if (inv4(m, back)) {
            mul4(back, sinv, ms);
            to_sl(k.prevClipToClip, ms);
        }
    }
    k.jitterOffset = sl::float2(c->jx, c->jy);
    k.mvecScale = sl::float2(1.0f / (float)em.width, 1.0f / (float)em.height);   // render pixels to [-1, 1]
    if (S.dbg == 2) k.mvecScale = sl::float2(-k.mvecScale.x, -k.mvecScale.y);   // F6 mode 2: opposite sign
    k.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    const double *v = c->view_inv;   // rows of camera-to-world: columns are right, up, back (OpenGL looks down -z)
    k.cameraPos = sl::float3((float)v[3], (float)v[7], (float)v[11]);
    k.cameraRight = sl::float3((float)v[0], (float)v[4], (float)v[8]);
    k.cameraUp = sl::float3((float)v[1], (float)v[5], (float)v[9]);
    k.cameraFwd = sl::float3((float)-v[2], (float)-v[6], (float)-v[10]);
    k.cameraNear = c->near_z;
    k.cameraFar = c->far_z;
    k.cameraFOV = c->proj[5] != 0 ? 2.0f * atanf(1.0f / (float)c->proj[5]) : 1.0f;
    k.cameraAspectRatio = c->aspect;
    k.depthInverted = sl::Boolean::eFalse;
    k.cameraMotionIncluded = sl::Boolean::eTrue;
    k.motionVectors3D = sl::Boolean::eFalse;
    k.reset = c->reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
    k.orthographicProjection = sl::Boolean::eFalse;
    k.motionVectorsDilated = sl::Boolean::eFalse;
    k.motionVectorsJittered = sl::Boolean::eFalse;
    sl::Result rc = S.constants(k, *t, sl::ViewportHandle(0));
    if (S.dlssg_state && S.tagged && (S.tagged == 60 || S.tagged % 1800 == 0)) {   // what DLSS-G does, now and then
        sl::DLSSGState st{};
        if (S.dlssg_state(sl::ViewportHandle(0), st, nullptr) == sl::Result::eOk)
            rt_log("dlss-fg: %llu frames tagged; status %d, %u frames shown per rendered frame, VRAM %.0f MiB, "
                   "minimum size %ux%u", (unsigned long long)S.tagged, (int)st.status, st.numFramesActuallyPresented,
                   st.estimatedVRAMUsageInBytes / 1048576.0, st.minWidthOrHeight, st.minWidthOrHeight);
    }
    if (++S.tagged == 1 || r != sl::Result::eOk || rc != sl::Result::eOk) {
        static int logged;
        if (logged++ < 4)
            rt_log("dlss-fg: frame %llu tagged (%u tags, result %d; constants %d; depth %ux%u, vectors %ux%u, HUD-less %ux%u)",
                   (unsigned long long)S.frame, ntags, (int)r, (int)rc, ed.width, ed.height, em.width, em.height,
                   eh.width, eh.height);
    }
}

void sl_fg_render_end(void) {
    if (!sl_fg_active()) return;
    if (!S.sim_ended) mark(sl::PCLMarker::eSimulationEnd, S.frame), mark(sl::PCLMarker::eRenderSubmitStart, S.frame),
                      S.sim_ended = 1;
    mark(sl::PCLMarker::eRenderSubmitEnd, S.frame);
}

uint64_t sl_fg_present_token(void) {
    if (fsr_fg_on()) return fsr_fg_frame(); return S.frame; }

void sl_fg_present_as(uint64_t frame, int start) {
    if (fsr_fg_on()) { if (start) fsr_fg_configure(frame); return; }
    if (!sl_fg_active()) return;
    mark(start ? sl::PCLMarker::ePresentStart : sl::PCLMarker::ePresentEnd, frame);
}

void sl_fg_present(int start) { sl_fg_present_as(S.frame, start); }

void sl_fg_frame_end(void) {
    if (fsr_fg_on()) { fsr_fg_frame_end(); return; }
    if (!sl_fg_active()) return;
    S.frame++;
    S.sim_ended = 0;
    mark(sl::PCLMarker::eSimulationStart, S.frame);
}

void sl_fg_pause(int paused) {
    if (fsr_fg_on()) { fsr_fg_pause(paused); return; }
    if (!sl_fg_active() || S.paused == paused) return;
    S.paused = paused;
    set_options();
}

void sl_fg_cycle(void) {
    if (fsr_fg_on()) { fsr_fg_toggle(); return; }
    if (!sl_fg_active()) {
        rt_log("dlss-fg: not available (dlss_fg = 2, 3 or 4 in sr-config.ini at start, an RTX 40/50 GPU and the Streamline "
               "files next to sr.exe)");
        return;
    }
    int next = S.mult >= S.max_mult ? 1 : S.mult + 1;
    S.mult = next;
    S.mult_dirty = 1;
    static const char *const names[] = {"off", "off", "2", "3", "4"};
    cfg_set("SR_DLSS_FG", names[next]);
    cfg_save();
}

const char *sl_fg_overlay_line(void) {
    if (fsr_fg_on()) return fsr_fg_overlay_line();
    if (!S.up) return nullptr;
    if (!S.supported_fg) return "FG NOT SUPPORTED";
    static char line[72];
    static unsigned calls;
    if (S.dlssg_state && calls++ % 30 == 0) {   // twice a second at 60 fps
        sl::DLSSGState st{};
        if (S.dlssg_state(sl::ViewportHandle(0), st, nullptr) == sl::Result::eOk)
            S.presented = st.numFramesActuallyPresented, S.status = (int)st.status;
    }
    if (S.mult <= 1) return "FG OFF";
    static const char *const dbg_name[6] = {"", "  F6 ZERO MV", "  F6 FLIP MV", "  F6 NO HUDLESS", "  F6 GEN ONLY",
                                            "  F6 OLD ORDER"};
    snprintf(line, sizeof line, "FG %dX  SHOWN %u%s%s", S.mult, S.presented, S.status ? "  ERROR" : "", dbg_name[S.dbg]);
    return line;
}

void sl_fg_debug_cycle(void) {
    if (fsr_fg_on()) { fsr_fg_debug_cycle(); return; }
    if (!sl_fg_active()) return;
    S.dbg = (S.dbg + 1) % 6;
    S.mult_dirty = 1;   // generated-only is an option flag: set from the replay thread with the next frame
    rt_log("dlss-fg: F6 test mode %d", S.dbg);
}
int sl_fg_debug_mode(void) { return sl_fg_active() ? S.dbg : 0; }
int sl_fg_strict_order(void) {
    if (fsr_fg_on()) return fsr_fg_strict_order();
    return sl_fg_active() && S.mult > 1 && !S.paused && S.dbg != 5;   // F6 mode 5: the old, overlapping order
}

}   // extern "C"
#endif   // SR_HAVE_STREAMLINE
