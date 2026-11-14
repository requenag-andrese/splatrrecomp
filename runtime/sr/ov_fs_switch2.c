// Host overrides for the nn::fs calls of the Switch 2 SDK (21.4.4) that totk-pc-recomp's runtime (SDK 20) does not
// override. The host file system is totk's (fs_host.c, romfs.c).
#include <stdint.h>

#include "../core/fs.h"
#include "../core/host.h"

// Result MountRom(const char *name): SDK 21's form without a cache buffer (totk overrides the one with a cache).
static void ov_mount_rom(Cpu *c) { ret_x(c, romfs_mount(arg_p(c, 0))); }
// Result EnableGlobalFileDataCache(void *buffer, size_t size): the host reads files directly and keeps no fs data
// cache; the buffer stays the game's.
static void ov_global_cache(Cpu *c) {
    rt_log("fs: global file data cache of %#llx bytes not used (host reads directly)", (unsigned long long)arg_x(c, 1));
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn2fs8MountRomEPKc", ov_mount_rom);
HOST_OVERRIDE("_ZN2nn2fs25EnableGlobalFileDataCacheEPvm", ov_global_cache);
