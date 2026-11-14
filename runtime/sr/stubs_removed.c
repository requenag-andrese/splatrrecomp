// Neutral implementations of the totk-pc-recomp modules left out of runtime/core (see runtime/core/NOTICE.md), so the
// copied files that call them build unchanged: no nfp IPC service (the amiibo reader is core/amiibo.c, and every nn::nfp
// function the game imports is overridden by symbol: core/ov_system.c, sr/ov_nfp.c), no player mods, no cheats, no flight
// recorder. Each answers as the Switch does with the feature absent or switched off.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "../core/cheats.h"
#include "../core/flight.h"
#include "../core/mods.h"
#include "../core/nfp.h"

// ---- nfp:user: every command fails with nfp's "device not found" (module 115, description 64)
void nfp_user_manager_handler(IpcSession *s, const IpcRequest *rq, IpcResponse *rs) {
    (void)s;
    (void)rq;
    rs->result = 115 | (64u << 9);
    rs->data_size = 0;
}

// ---- mods: none
void mods_scan(void) {}
int mods_count(void) { return 0; }
ModInfo *mods_at(int i) { (void)i; return NULL; }
void mods_exefs_switch(void) {}

// ---- cheats: none
void cheats_init(void) {}

// ---- flight recorder and its thumbnails: off
int flight_on;
void flight_init(void) {}
void flight_frame(uint64_t frame, uint64_t now_ns, uint64_t interval_ns, const uint64_t blocked_ns[],
                  const volatile int64_t counts[]) {
    (void)frame, (void)now_ns, (void)interval_ns, (void)blocked_ns, (void)counts;
}
#ifdef SR_NULL_PROFILE   // gfx_present.c has the thumbnails in the Vulkan profile
void vk_thumbs_enable(int n) { (void)n; }
void vk_thumbs_frame(uint64_t frame) { (void)frame; }
int vk_thumbs_read(uint64_t frame, uint8_t *rgba) { (void)frame, (void)rgba; return 0; }
#endif
