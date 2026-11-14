// sr.exe --probe-amiibo: the amiibo reader (core/amiibo.c) and every nn::nfp function the game imports (core/ov_system.c,
// sr/ov_nfp.c) on a real figure dump, SR_PROBE_AMIIBO=<540-byte NTAG215 .bin> (read in place, never copied; without it
// the probe is skipped). As the game would, on a guest thread: initialize, list the reader, detect, the figure tapped
// (as the menu's Amiibo tab does), mount, tag / model / common / register info, then the application area: none yet,
// create, read back, set, flush; the figure removed and tapped again reads what was flushed (the profile's file);
// another game's id, a second create and the data eraser applet answer as on the console.
// The profile is SR_PROFILE_DIR's (tools/run_probes.py: a fresh one), so no area of the player's is touched.
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../core/amiibo.h"
#include "../core/cfg.h"
#include "../core/host.h"
#include "../core/platform/platform.h"

#define U(x) ((uint64_t)(uintptr_t)(x))
#define NFP_R(desc) (115u | (uint32_t)(desc) << 9)

static int failures, checks;
#define CHECK(cond, ...) \
    do { checks++; if (!(cond)) { failures++; printf("  FAIL: " __VA_ARGS__); putchar('\n'); } } while (0)

static Cpu *C;
static uint64_t call(const char *name, int n, ...) {
    GuestFn f = hostfn_override(name);
    if (!f) rt_fatal("probe-amiibo: no override for %s", name);
    va_list ap;
    va_start(ap, n);
    for (int i = 0; i < n && i < 8; i++) C->x[i] = va_arg(ap, uint64_t);
    va_end(ap);
    f(C);
    return C->x[0];
}
#define NFP(x) "_ZN2nn3nfp" x
static const char *const S_INIT = NFP("10InitializeEv"), *const S_FIN = NFP("8FinalizeEv"),
                         *const S_LIST = NFP("11ListDevicesEPNS0_12DeviceHandleEPii"),
                         *const S_STATE = NFP("14GetDeviceStateERKNS0_12DeviceHandleE"),
                         *const S_START = NFP("14StartDetectionERKNS0_12DeviceHandleE"),
                         *const S_STOP = NFP("13StopDetectionERKNS0_12DeviceHandleE"),
                         *const S_MOUNT = NFP("5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE"),
                         *const S_TAG = NFP("10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE"),
                         *const S_MODEL = NFP("12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE"),
                         *const S_COMMON = NFP("13GetCommonInfoEPNS0_10CommonInfoERKNS0_12DeviceHandleE"),
                         *const S_REG = NFP("15GetRegisterInfoEPNS0_12RegisterInfoERKNS0_12DeviceHandleE"),
                         *const S_OPEN = NFP("19OpenApplicationAreaERKNS0_12DeviceHandleEj"),
                         *const S_GET = NFP("18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm"),
                         *const S_SET = NFP("18SetApplicationAreaERKNS0_12DeviceHandleEPKvm"),
                         *const S_CREATE = NFP("21CreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE"),
                         *const S_RECREATE = NFP("23RecreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE"),
                         *const S_FLUSH = NFP("5FlushERKNS0_12DeviceHandleE"),
                         *const S_ERASER = NFP("19StartGameDataEraserEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE"),
                         *const S_NICK = NFP("29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE");

static const char *dump_path;
static uint8_t dump[AMIIBO_DUMP_SIZE];

static void run(Cpu *c) {
    C = c;
    uint8_t handle[8] = {0}, tag[0x58], model[0x40], common[0x40], reg[0x100], area[0x100], param[0x40] = {0};
    int32_t count = 0;
    const uint32_t id = 0x5352AA01u, other = 0x12345678u;
    CHECK(call(S_INIT, 0) == 0, "Initialize");
    CHECK((uint32_t)call(S_LIST, 3, U(handle), U(&count), 4ull) == 0 && count == 1, "ListDevices: %d readers", count);
    CHECK((uint32_t)call(S_START, 1, U(handle)) == 0 && call(S_STATE, 1, U(handle)) == AMIIBO_DEV_STATE_SEARCHING,
          "StartDetection: searching");
    CHECK(amiibo_scan_and_tap(dump_path), "the dump loads and is tapped (as the menu's Amiibo tab does)");
    CHECK(call(S_STATE, 1, U(handle)) == AMIIBO_DEV_STATE_TAG_FOUND, "a figure found");
    CHECK((uint32_t)call(S_MOUNT, 3, U(handle), 0ull, 1ull) == 0 && call(S_STATE, 1, U(handle)) == AMIIBO_DEV_STATE_TAG_MOUNTED,
          "Mount");

    CHECK((uint32_t)call(S_TAG, 2, U(tag), U(handle)) == 0 && !memcmp(tag, dump, 3) && !memcmp(tag + 3, dump + 4, 4) &&
          tag[0x0A] == 7, "TagInfo: the dump's 7-byte UID");
    CHECK((uint32_t)call(S_MODEL, 2, U(model), U(handle)) == 0 && !memcmp(model, dump + 0x54, 3) &&
          model[3] == dump[0x5A] && model[4] == dump[0x59] && model[5] == dump[0x58] && model[6] == dump[0x57],
          "ModelInfo: character, series, model number and type of the dump");
    uint32_t area_size = 0;
    CHECK((uint32_t)call(S_COMMON, 2, U(common), U(handle)) == 0 && (memcpy(&area_size, common + 8, 4), area_size == 0xD8),
          "CommonInfo: application area size %u", area_size);
    char nick[48];
    amiibo_nickname(nick, sizeof nick);
    CHECK((uint32_t)call(S_REG, 2, U(reg), U(handle)) == 0 && reg[0x5C] && !strcmp((char *)reg + 0x5C, nick),
          "RegisterInfo: nickname \"%s\"", (char *)reg + 0x5C);
    printf("figure: nickname \"%s\" (from the file name), character %02x%02x%02x, series %02x, model %02x%02x, type %u\n",
           nick, model[0], model[1], model[2], model[3], model[5], model[4], model[6]);

    // the application area: none yet, create, read back, set, flush
    CHECK((uint32_t)call(S_OPEN, 2, U(handle), (uint64_t)id) == NFP_R(128), "OpenApplicationArea on a new figure: not "
          "initialized (128)");
    uint8_t init[32];
    for (int i = 0; i < 32; i++) init[i] = (uint8_t)(0xA0 + i);
    uint8_t info[0x40] = {0};
    uint64_t p = U(init), n = sizeof init;
    memcpy(info, &id, 4), memcpy(info + 8, &p, 8), memcpy(info + 16, &n, 8);
    CHECK((uint32_t)call(S_CREATE, 2, U(handle), U(info)) == 0, "CreateApplicationArea");
    CHECK((uint32_t)call(S_CREATE, 2, U(handle), U(info)) == NFP_R(168), "a second CreateApplicationArea: exists (168)");
    CHECK((uint32_t)call(S_OPEN, 2, U(handle), (uint64_t)other) == NFP_R(152), "another game's id: wrong id (152)");
    uint64_t got = 0;
    memset(area, 0, sizeof area);
    CHECK((uint32_t)call(S_OPEN, 2, U(handle), (uint64_t)id) == 0 &&
          (uint32_t)call(S_GET, 4, U(area), U(&got), U(handle), (uint64_t)sizeof area) == 0 && got == 0xD8 &&
          !memcmp(area, init, sizeof init), "the created area reads back (%llu bytes)", (unsigned long long)got);
    uint8_t next[0xD8];
    for (int i = 0; i < 0xD8; i++) next[i] = (uint8_t)(i * 7 + 3);
    CHECK((uint32_t)call(S_SET, 3, U(handle), U(next), (uint64_t)sizeof next) == 0, "SetApplicationArea");
    CHECK((uint32_t)call(S_SET, 3, U(handle), U(next), 0xD9ull) == NFP_R(68), "an area of 0xd9 bytes: wrong size (68)");
    CHECK((uint32_t)call(S_FLUSH, 1, U(handle)) == 0, "Flush");
    uint16_t writes = 0;
    call(S_COMMON, 2, U(common), U(handle));
    memcpy(&writes, common + 4, 2);
    CHECK(writes == 2, "CommonInfo write counter %u after create and flush", writes);

    // removed and tapped again: the flushed area comes back from the profile's file
    amiibo_remove_tag();
    CHECK(call(S_STATE, 1, U(handle)) == AMIIBO_DEV_STATE_SEARCHING || call(S_STATE, 1, U(handle)) ==
          AMIIBO_DEV_STATE_INITIALIZED, "removed: no figure");
    CHECK((uint32_t)call(S_OPEN, 2, U(handle), (uint64_t)id) == NFP_R(64), "OpenApplicationArea without a figure (64)");
    amiibo_tap_tag();
    CHECK((uint32_t)call(S_MOUNT, 3, U(handle), 0ull, 1ull) == 0, "Mount again");
    memset(area, 0, sizeof area);
    CHECK((uint32_t)call(S_OPEN, 2, U(handle), (uint64_t)id) == 0 &&
          (uint32_t)call(S_GET, 4, U(area), U(&got), U(handle), (uint64_t)sizeof area) == 0 && !memcmp(area, next, 0xD8),
          "the flushed area after the figure came back");
    uint8_t changed = 1;
    CHECK((uint32_t)call(S_NICK, 5, U(handle), U(&changed), U(reg), U(param), U(tag)) == 0 && changed == 0,
          "nickname and owner applet: nothing changed");
    CHECK((uint32_t)call(S_RECREATE, 2, U(handle), U(info)) == 0 &&
          (uint32_t)call(S_GET, 4, U(area), U(&got), U(handle), (uint64_t)sizeof area) == 0 && !memcmp(area, init, 32) &&
          area[32] == 0, "RecreateApplicationArea: the new initial data");
    CHECK((uint32_t)call(S_ERASER, 3, U(handle), U(param), U(tag)) == 0, "data eraser applet");
    CHECK((uint32_t)call(S_OPEN, 2, U(handle), (uint64_t)id) == NFP_R(128), "after the eraser: no area (128)");

    char file[700];
    snprintf(file, sizeof file, "%s/amiibo/%02x%02x%02x%02x%02x%02x%02x.bin", rt_config()->profile, dump[0], dump[1],
             dump[2], dump[4], dump[5], dump[6], dump[7]);
    CHECK(!plat_file_exists(file), "the eraser removed the area file");
    uint64_t size = 0;
    uint8_t *again = plat_file_read(dump_path, &size);
    CHECK(again && size == AMIIBO_DUMP_SIZE && !memcmp(again, dump, size), "the dump on disk is unchanged");
    free(again);
    amiibo_remove_tag();
    CHECK((uint32_t)call(S_STOP, 1, U(handle)) == 0 && call(S_FIN, 0) == 0, "StopDetection, Finalize");
    printf("application area: created, read back, set, flushed (write %u), back after the figure was removed and tapped "
           "again, recreated, erased; the dump untouched\n", writes);
}

int probe_amiibo(void) {
    dump_path = cfg_get("SR_PROBE_AMIIBO");
    if (!dump_path || !*dump_path) {
        printf("probe-amiibo: skipped: no figure dump (SR_PROBE_AMIIBO=<540-byte .bin>)\n");
        return 0;
    }
    uint64_t size = 0;
    uint8_t *d = plat_file_read(dump_path, &size);
    if (!d || size != AMIIBO_DUMP_SIZE) {
        printf("probe-amiibo: FAILED: %s is not a 540-byte dump (%llu bytes)\n", dump_path, (unsigned long long)size);
        free(d);
        return 1;
    }
    memcpy(dump, d, sizeof dump);
    free(d);
    ldr_load(rt_config()->exefs);
    kern_run_on_guest_main(run);
    if (failures) {
        printf("probe-amiibo: FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    printf("probe-amiibo: passed (%d checks)\n", checks);
    return 0;
}
