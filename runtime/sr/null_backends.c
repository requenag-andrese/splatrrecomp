// Stand-ins for the parts of runtime/core this port does not build. In every profile: movies (ov_movie.c, ov_media.c),
// the TotK resolution and camera rewrites, Discord, the mods probe and rt_pacga. In the null profile
// (SR_NULL_PROFILE) also the Vulkan back end, the window, audio output and input, answered as a machine without a
// GPU, audio device or gamepad would; gfx_null.c then always picks the null graphics back end.
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../core/audren.h"
#include "../core/gfx.h"
#include "../core/host.h"
#include "../core/input.h"
#include "../core/input_win.h"
#include "../core/movie.h"
#include "../core/tex.h"
#include "../core/vk.h"

#ifdef SR_NULL_PROFILE   // the Vulkan profile builds the real back end, window, audio output and input
// ---- graphics: the Vulkan back end is not built. gfx_null.c never selects this one (SR_NULL_PROFILE).
const GfxBackend gfx_vulkan_backend;
void gfx_vsync_bind(uint64_t system_event) { (void)system_event; }
HWND gfx_window_handle(void) { return NULL; }
double gfx_window_refresh_hz(void) { return 0; }

VkCtx *vk_ctx(void) { return NULL; }
int vk_init(void) { rt_log("vk: no Vulkan back end in the null profile"); return 0; }
void vk_shutdown(void) {}
long vk_validation_errors_total(void) { return 0; }
void vk_gpu_written(uint64_t a, uint64_t n) { (void)a, (void)n; }
void vk_guest_written(uint64_t a, uint64_t n) { (void)a, (void)n; }
void vk_hdr_shader_swap(uint32_t **spirv, uint64_t *words) { (void)spirv, (void)words; }
int vk_mem_changed(uint64_t a, uint64_t n, uint32_t since, uint32_t *now) { (void)a, (void)n, (void)since; if (now) *now = 0; return 1; }
void vk_mem_gpu_pending(uint64_t a, uint64_t n) { (void)a, (void)n; }
int vk_mem_resolve(uint64_t a, uint64_t n, VkMemSpan *out) { (void)a, (void)n, (void)out; return 0; }
void vk_mem_shadow_sync(void) {}
void vk_mem_shutdown(void) {}
void vk_mem_stats(VkMemStats *s) { memset(s, 0, sizeof *s); }
void vk_pipeline_async_enable(void) {}
int vk_pipeline_prepare_shaders(void) { return 1; }
void vk_pipeline_prewarm(void) {}
void vk_pipeline_save(void) {}
int vk_pipeline_warm_shaders(void) { return 1; }
void vk_pool_fini(pool_t *p) { (void)p; }
int vk_pool_init(pool_t *p) { (void)p; return 0; }
int vk_pool_map_virtual(pool_t *v, int n, const void *req) { (void)v, (void)n, (void)req; return 0; }
void vk_read_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row, uint64_t img) {
    (void)t, (void)v, (void)r, (void)dst, (void)row, (void)img;
}
void vk_write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, uint64_t row,
                     uint64_t img) {
    (void)t, (void)v, (void)r, (void)src, (void)row, (void)img;
}
VkSampler vk_sampler_by_id(int id) { (void)id; return VK_NULL_HANDLE; }
VkSampler vk_sampler_for(const samp_par *s) { (void)s; return VK_NULL_HANDLE; }
void vk_sampler_register(int id, const samp_par *s) { (void)id, (void)s; }
void *vk_staging(VkDeviceSize size, VkBuffer *buf, VmaAllocation *alloc) { (void)size, (void)buf, (void)alloc; return NULL; }
const VkTexImage *vk_texture_by_id(int id, view_t *view, int *has_view) { (void)id, (void)view; if (has_view) *has_view = 0; return NULL; }
const VkTexImage *vk_texture_get(const tex_t *t) { (void)t; return NULL; }
int vk_texture_read_image(const tex_t *t, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, uint64_t row,
                          uint64_t img) {
    (void)t, (void)v, (void)r, (void)dst, (void)row, (void)img;
    return 0;
}
void vk_texture_register(int id, const tex_t *t, const view_t *v) { (void)id, (void)t, (void)v; }
void vk_texture_release(const tex_t *t) { (void)t; }
void vk_texture_stats(uint64_t *images, uint64_t *uploads, uint64_t *reuploads) { *images = *uploads = *reuploads = 0; }
VkCommandBuffer vk_upload_begin(void) { return VK_NULL_HANDLE; }
void vk_upload_end(void) {}
uint64_t vk_upload_flush(int wait) { (void)wait; return 0; }
int tex_vk_format(int nvn_format, TexVkFormat *out) { (void)nvn_format, (void)out; return 0; }
int tex_vk_format_supported(const TexVkFormat *f) { (void)f; return 0; }

// Vulkan and VMA calls made through the recorder macros (vk_rec.h) or directly, behind vk_ctx() checks.
void rc_vkCmdCopyBuffer(VkCommandBuffer cb, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r) { (void)cb, (void)s, (void)d, (void)n, (void)r; }
void rc_vkCmdFillBuffer(VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, VkDeviceSize n, uint32_t v) { (void)cb, (void)b, (void)o, (void)n, (void)v; }
void rc_vkCmdPipelineBarrier2(VkCommandBuffer cb, const VkDependencyInfo *di) { (void)cb, (void)di; }
VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice d, VkPhysicalDeviceProperties2 *p) { (void)d, (void)p; }
void vmaDestroyBuffer(VmaAllocator a, VkBuffer b, VmaAllocation al) { (void)a, (void)b, (void)al; }
VkResult vmaFlushAllocation(VmaAllocator a, VmaAllocation al, VkDeviceSize o, VkDeviceSize n) { (void)a, (void)al, (void)o, (void)n; return VK_SUCCESS; }
VkResult vmaInvalidateAllocation(VmaAllocator a, VmaAllocation al, VkDeviceSize o, VkDeviceSize n) { (void)a, (void)al, (void)o, (void)n; return VK_SUCCESS; }

// ---- audio: no output device. The renderer keeps its timing (audren.c paces on audio_mixer_pace_ns).
void audio_device_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) { (void)s, (void)rq; rs->result = 0; rs->data_size = 0; }
void audio_mixer_open(AudioRenderer *r) { (void)r; }
void audio_mixer_close(AudioRenderer *r) { (void)r; }
void audio_mixer_effect_result(AudioRenderer *r, int id, void *out, unsigned size) { (void)r, (void)id; memset(out, 0, size); }
int64_t audio_mixer_pace_ns(AudioRenderer *r, uint64_t now) { (void)r, (void)now; return 0; }
void audio_mixer_render(AudioRenderer *r, double seconds) { (void)r, (void)seconds; }
void audio_mixer_set_running(AudioRenderer *r, int running) { (void)r, (void)running; }
uint32_t audio_mixer_sink_written(AudioRenderer *r, int id) { (void)r, (void)id; return 0; }
void audio_mixer_update_effects(AudioRenderer *r) { (void)r; }
void audio_mixer_update_routing(AudioRenderer *r, const uint8_t *splitters, uint32_t splitter_bytes, const uint8_t *mixes,
                                uint32_t mix_bytes, int mix_count) {
    (void)r, (void)splitters, (void)splitter_bytes, (void)mixes, (void)mix_bytes, (void)mix_count;
}
uint64_t audio_voice_consumed(const AudioRenderer *r, ServerVoice *v, int frames) { (void)r, (void)v, (void)frames; return 0; }

// ---- input: no gamepad, keyboard or motion
void input_block(int on) { (void)on; }
void input_motion_axes(const float a[3], const float g[3], double acc[3], double gyro[3]) {
    (void)a, (void)g;
    acc[0] = acc[1] = gyro[0] = gyro[1] = gyro[2] = 0;
    acc[2] = -1;   // at rest, 1 g down
}
int input_motion_enabled(void) { return 0; }
void input_motion_start(void) {}
static void no_input(uint64_t *b, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) { *b = 0; *lx = *ly = *rx = *ry = 0; }
void input_poll(uint64_t *b, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) { no_input(b, lx, ly, rx, ry); }
void input_poll_ui(uint64_t *b, int32_t *lx, int32_t *ly, int32_t *rx, int32_t *ry) { no_input(b, lx, ly, rx, ry); }
int input_rumble(uint16_t low, uint16_t high, uint32_t ms) { (void)low, (void)high, (void)ms; return 0; }
int input_rumble_triggers_enabled(void) { return 0; }
uint16_t rumble_motor(float a, float curve, float strength) { (void)a, (void)curve, (void)strength; return 0; }

#endif   // SR_NULL_PROFILE

// ---- movies: no decoder (ov_movie.c is not built, so movie:: is not overridden and runs as recompiled)
const char *const movie_symbols[MOVIE_ENTRIES];
int movie_quiet(uint64_t obj) { (void)obj; return 1; }
void movie_set_clock(uint64_t (*clock)(void)) { (void)clock; }
void movie_set_guest(const MovieGuest *g) { (void)g; }
void movie_set_pace(int on) { (void)on; }
int movie_stats(uint64_t obj, MovieStats *out) { (void)obj; memset(out, 0, sizeof *out); return 0; }
void movie_tick(Cpu *c) { (void)c; }

// ---- left out of runtime/core (resolution.c, discord_rpc.c) and probes not built in this profile
void res_fn_hit(uint64_t addr, uint64_t x0) { (void)addr, (void)x0; }
uint64_t res_heap_extra_bytes(void) { return 0; }
void res_init(void) {}
void res_present(Cpu *c) { (void)c; }
// romfs.c's TotK file rewrites (resolution.c HeapDef growth and wide overlay, camera_speed.c camera pack): all off.
uint64_t res_heap_extra_kib(void) { return 0; }
int res_heapdef_build(const void *in, size_t n, void **out, size_t *out_n) { (void)in, (void)n, (void)out, (void)out_n; return 0; }
void res_heapdef_served(void) {}
void res_wanted(unsigned out[2]) { out[0] = out[1] = 0; }
int res_wants_wide_overlay(const unsigned out[2]) { (void)out; return 0; }
float cam_speed_wanted(void) { return 1.0f; }
int cam_pack_build(const void *in, size_t n, const void *d, size_t dn, void **out, size_t *out_n) {
    (void)in, (void)n, (void)d, (void)dn, (void)out, (void)out_n;
    return 0;
}
void cam_pack_served(void) {}
void discord_init(void) {}
// A probe of something this build does not have: "skipped" with the reason, exit 0 (tools/run_probes.py counts it so).
static int not_built(const char *what) {
    printf("%s: skipped: not built in this profile\n", what + 2);
    return 0;
}
static int not_ported(const char *what) {
    printf("%s: skipped: not part of this port (runtime/core/NOTICE.md)\n", what + 2);
    return 0;
}
int probe_discord(void) { return not_ported("--probe-discord"); }
int probe_mods(void) { return not_ported("--probe-mods"); }
#ifdef SR_NULL_PROFILE
int probe_astc(void) { return not_built("--probe-astc"); }

int probe_vkdevice(void) { return not_built("--probe-vkdevice"); }
int probe_vkexec(void) { return not_built("--probe-vkexec"); }
int probe_window(void) { return not_built("--probe-window"); }
#endif

// ---- srrecomp lifter extension (runtime/srrecomp_ext.h): PACGA as a keyed 32-bit mix (splitmix64 finaliser)
uint64_t rt_pacga(uint64_t x, uint64_t m) {
    uint64_t z = x ^ (m * 0x9E3779B97F4A7C15ull) ^ 0x5352524543304D50ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return z & 0xFFFFFFFF00000000ull;
}
