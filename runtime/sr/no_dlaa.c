// totk-pc-recomp features built for Zelda's renderer and left out of this port (runtime/core/NOTICE.md), as the Vulkan
// back end sees them: totk's per-object motion-vector tools, contact shadows, RenderDoc capture, the FOV and
// camera-speed settings. Each is off. DLSS is sr/vk_dlss.c.
#ifdef SR_VULKAN_PROFILE
#include <stdint.h>

#include "../core/vk_dlaa.h"

// vk_mvsurvey_draw / vk_mvsurvey_frame: sr/vk_mvsurvey.c (SR_MV_SURVEY=1).
// vk_mvprev_*: sr/vk_mvprev.c (last frame's object blocks, SR_DLAA_OBJMV); vk_mvobj_execute is in vk_exec_draw.c.

// ---- the other left-out TotK features the Vulkan back end calls: contact shadows (strength 0 = off), RenderDoc
// capture (never active), the TotK FOV and camera-speed settings (unchanged).
void vk_contact_flush(VkExec *ex) { (void)ex; }
void vk_contact_pass_begin(VkExec *ex) { (void)ex; }
void vk_contact_release(void) {}
int vk_contact_strength(void) { return 0; }
int rdoc_active(void) { return 0; }
void rdoc_after_present(uint64_t frame) { (void)frame; }
void rdoc_init(void) {}
void res_fov_live(void) {}
float cam_speed_applied(void) { return 1.0f; }
#endif
