// sr-recomp: DLSS Frame Generation through NVIDIA Streamline (sr/vk_dlssg.cpp), manual hooking. Built with
// tools/build_runtime.py --streamline <sdk>; otherwise every function here is a pass-through or does nothing.
//
// Streamline intercepts a few Vulkan calls (sl_hooks.h): the runtime makes them through the sr_vk* wrappers below,
// which take Streamline's proxy once dlss_fg is on and Streamline came up, and the driver's function otherwise.
#pragma once
#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

// Before vkCreateInstance (vk_device.c create_instance): loads and starts Streamline when dlss_fg asks for it.
void sl_fg_preinit(void);
int sl_fg_requested(void);   // dlss_fg asks for frame generation (the instance then also gets VK_EXT_debug_utils)
// After the device exists: checks DLSS-G on this GPU and sets Reflex and the generation mode.
void sl_fg_device_ready(VkPhysicalDevice phys);
int sl_fg_active(void);   // Streamline is up and DLSS-G supported: the scene's inputs are worth tagging

VkResult sr_vkCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out);
// Streamline's vkCreateDevice finds the instance through the physical devices its vkEnumeratePhysicalDevices handed
// out (wrapper.cpp instanceDeviceMap): without it the plugins start with an empty instance table and call a null
// vkGetPhysicalDeviceMemoryProperties. So the enumeration goes through Streamline too.
VkResult sr_vkEnumeratePhysicalDevices(VkInstance inst, uint32_t *n, VkPhysicalDevice *out);
VkResult sr_vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a,
                           VkDevice *out);
VkResult sr_vkCreateWin32SurfaceKHR(VkInstance inst, const void *ci, const VkAllocationCallbacks *a, VkSurfaceKHR *out);
void sr_vkDestroySurfaceKHR(VkInstance inst, VkSurfaceKHR s, const VkAllocationCallbacks *a);
VkResult sr_vkCreateSwapchainKHR(VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a,
                                 VkSwapchainKHR *out);
void sr_vkDestroySwapchainKHR(VkDevice d, VkSwapchainKHR sc, const VkAllocationCallbacks *a);
VkResult sr_vkGetSwapchainImagesKHR(VkDevice d, VkSwapchainKHR sc, uint32_t *n, VkImage *images);
VkResult sr_vkAcquireNextImageKHR(VkDevice d, VkSwapchainKHR sc, uint64_t timeout, VkSemaphore s, VkFence f,
                                  uint32_t *index);
VkResult sr_vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *pi);
VkResult sr_vkDeviceWaitIdle(VkDevice d);

// An image handed to Streamline: the image, a view of it, its format, size, usage and layout (GENERAL here).
typedef struct {
    VkImage image;
    VkImageView view;
    VkFormat format;
    uint32_t width, height;   // the image's size
    uint32_t ew, eh;          // the part in use (render size), 0: the whole image
    VkImageUsageFlags usage;
} SlImage;

// The scene camera of this frame as the game's camera block holds it (sr/vk_dlss.c): row-major 4x4 for column vectors,
// OpenGL clip depth. view_inv: camera-to-world rows (3x4).
typedef struct {
    double cur_vp[16], prev_vp[16], proj[16], view_inv[12];
    float near_z, far_z, aspect;
    float jx, jy;             // the jitter of this frame's scene, render pixels
    int reset;                // no history (a cut, a size change)
} SlCamera;

// The replay, right after DLSS wrote the picture the HUD is drawn over (sr/vk_dlss.c, recorder drained): tags depth,
// motion vectors (render pixels) and the HUD-less picture for this frame, copied now (the HUD is drawn into the same
// image afterwards), and sets the frame's constants.
void sl_fg_tag(VkCommandBuffer cb, const SlImage *depth, const SlImage *mv, const SlImage *hudless, const SlCamera *cam);
// The present path (gfx_present.c): the frame's latency markers. render_end before the blit is submitted; present
// around vkQueuePresentKHR (start 1 / end 0); frame_end after it, which opens the next frame.
void sl_fg_render_end(void);
void sl_fg_present(int start);
void sl_fg_frame_end(void);
uint64_t sl_fg_present_token(void);   // the frame a present belongs to (kept by a deferred flip)
void sl_fg_present_as(uint64_t frame, int start);

void sl_fg_cycle(void);                    // F7: off, 2x, 3x, 4x
void sl_fg_pause(int paused);              // DLSS switched off (F8): frame generation has no inputs, so it pauses
const char *sl_fg_overlay_line(void);      // the overlay's line, NULL when Streamline is not in use
// F6 (test): how frame generation gets its inputs. 0 normal, 1 zero motion vectors (sr/vk_dlss.c clears them after DLSS
// read them), 2 vectors with the opposite sign, 3 no HUD-less picture, 4 only the generated frames shown, 5 the old
// present order (see sl_fg_strict_order).
void sl_fg_debug_cycle(void);
int sl_fg_debug_mode(void);
// While frame generation is generating (DLSS-G or FSR): the next frame's submissions wait until this frame's present
// went out (vk_backend.c), so the GPU runs a frame's generation before the next frame's work and its input copies.
// Both SDKs assume that order; the replay otherwise queues frame N+1 before frame N's held present.
int sl_fg_strict_order(void);

// ---- AMD FSR 3 frame generation (sr/vk_fsrfg.cpp, sr-config.ini fsr_fg = 1). The sr_vk* swapchain wrappers and the
// sl_fg_* frame hooks above send their work there when it is on; Streamline then stays off.
typedef struct {
    VkQueue game, present, acquire, async;   // four different queues
    uint32_t game_family, present_family, acquire_family, async_family;
} FsrQueues;
int fsr_fg_requested(void);   // fsr_fg = 1: vk_device.c asks for FSR's queues
int fsr_fg_on(void);          // loaded and handed the device: the swapchain is FSR's
void fsr_fg_device_ready(VkInstance inst, VkPhysicalDevice phys, VkDevice dev, const FsrQueues *q);
VkResult fsr_vk_create_swapchain(VkDevice d, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a,
                                 VkSwapchainKHR *out);
void fsr_vk_destroy_swapchain(VkDevice d, VkSwapchainKHR s, const VkAllocationCallbacks *a);
VkResult fsr_vk_get_images(VkDevice d, VkSwapchainKHR s, uint32_t *n, VkImage *im);
VkResult fsr_vk_acquire(VkDevice d, VkSwapchainKHR s, uint64_t t, VkSemaphore se, VkFence f, uint32_t *i);
VkResult fsr_vk_present(VkQueue q, const VkPresentInfoKHR *pi);
void fsr_fg_prepare(VkCommandBuffer cb, const SlImage *depth, const SlImage *mv, const SlImage *hudless, const SlCamera *cam);
uint64_t fsr_fg_frame(void);
void fsr_fg_frame_end(void);
void fsr_fg_configure(uint64_t frame);
void fsr_fg_toggle(void);
void fsr_fg_pause(int paused);
void fsr_fg_debug_cycle(void);
int fsr_fg_strict_order(void);
const char *fsr_fg_overlay_line(void);

#ifdef __cplusplus
}
#endif
