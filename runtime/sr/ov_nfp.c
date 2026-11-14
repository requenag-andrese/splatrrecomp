// The nn::nfp functions Splatoon Raiders imports beyond TotK's (core/ov_system.c has those: initialize, devices,
// detection, mount, tag and model info): common and register info, the application area (the game's own data on a
// figure) and the amiibo settings applets. They serve the figure core/amiibo.c holds; its application area is
// simulated in a file of the profile (amiibo.c says why), so writing never touches the dump.
//
// Results: nfp's module 115 with the descriptions Switchbrew's error codes and yuzu's nfc_result.h name: 64 device not
// found (no figure; the code core/ov_system.c answers), 68 wrong application area size, 128 application area not
// initialized (none on the figure: the game creates one), 152 wrong application area id (another game's), 168 an
// application area exists (CreateApplicationArea on a figure that has one), 88 write failed.
// The signatures are those nx2android PR #88 hle_sdk16.c lists for these functions (MIT).
#include <stdint.h>
#include <string.h>

#include "../core/amiibo.h"
#include "../core/host.h"

#define NFP_R(desc) (115u | (uint32_t)(desc) << 9)

static uint32_t result_of(AmiiboAppResult r) {
    switch (r) {
    case AMIIBO_APP_OK: return 0;
    case AMIIBO_APP_NO_TAG: return NFP_R(64);
    case AMIIBO_APP_NONE: return NFP_R(128);
    case AMIIBO_APP_NOT_OPEN: return NFP_R(128);
    case AMIIBO_APP_OTHER_ID: return NFP_R(152);
    case AMIIBO_APP_BAD_SIZE: return NFP_R(68);
    case AMIIBO_APP_EXISTS: return NFP_R(168);
    case AMIIBO_APP_WRITE_FAILED: return NFP_R(88);
    }
    return NFP_R(64);
}

// Result GetCommonInfo(CommonInfo *, const DeviceHandle &)
static void ov_common_info(Cpu *c) {
    void *out = arg_p(c, 0);
    if (!amiibo_is_mounted()) return ret_x(c, NFP_R(64));
    if (out) amiibo_get_common_info(out);
    ret_x(c, 0);
}

// Result GetRegisterInfo(RegisterInfo *, const DeviceHandle &): a registered figure named after its file
static void ov_register_info(Cpu *c) {
    void *out = arg_p(c, 0);
    if (!amiibo_is_mounted()) return ret_x(c, NFP_R(64));
    if (out) amiibo_get_register_info(out);
    ret_x(c, 0);
}

// Result OpenApplicationArea(const DeviceHandle &, u32 access id)
static void ov_open_area(Cpu *c) { ret_x(c, result_of(amiibo_app_open((uint32_t)arg_x(c, 1)))); }

// Result GetApplicationArea(void *buf, size_t *size, const DeviceHandle &, size_t buf size)
static void ov_get_area(Cpu *c) {
    uint64_t *size = arg_p(c, 1);
    if (size) *size = 0;
    ret_x(c, result_of(amiibo_app_get(arg_p(c, 0), arg_x(c, 3), size)));
}

// Result SetApplicationArea(const DeviceHandle &, const void *data, size_t size): kept until Flush
static void ov_set_area(Cpu *c) { ret_x(c, result_of(amiibo_app_set(arg_p(c, 1), arg_x(c, 2)))); }

// Result CreateApplicationArea / RecreateApplicationArea(const DeviceHandle &, const ApplicationAreaCreateInfo &):
// { u32 access id; void *initial data (at +8); size_t its size (+16); reserved }
static void create_area(Cpu *c, int recreate) {
    const uint8_t *info = arg_p(c, 1);
    if (!info) return ret_x(c, NFP_R(65));   // invalid argument
    uint32_t id;
    uint64_t data, size;
    memcpy(&id, info, 4), memcpy(&data, info + 8, 8), memcpy(&size, info + 16, 8);
    ret_x(c, result_of(amiibo_app_create(id, (const void *)(uintptr_t)data, size, recreate)));
}
static void ov_create_area(Cpu *c) { create_area(c, 0); }
static void ov_recreate_area(Cpu *c) { create_area(c, 1); }

// Result Flush(const DeviceHandle &): the area the game set goes to the figure (its file)
static void ov_flush(Cpu *c) { ret_x(c, result_of(amiibo_app_flush())); }

// Result StartNicknameAndOwnerSettings(DeviceHandle *, bool *changed, RegisterInfo *, const AmiiboSettingsStartParam &,
// const TagInfo & [, const RegisterInfo &]): the console's amiibo settings applet. None here: nothing changed, the
// figure keeps its (made-up) owner and nickname.
static void ov_nickname_settings(Cpu *c) {
    uint8_t *changed = arg_p(c, 1);
    void *reg = arg_p(c, 2);
    if (changed) *changed = 0;
    if (reg && amiibo_has_tag()) amiibo_get_register_info(reg);
    rt_log("nfp: amiibo settings applet (nickname and owner) asked for: the figure stays as it is");
    ret_x(c, 0);
}

// Result StartGameDataEraser(DeviceHandle *, const AmiiboSettingsStartParam &, const TagInfo &): the applet that erases
// a game's data on the figure: the simulated area goes
static void ov_data_eraser(Cpu *c) {
    amiibo_app_erase();
    ret_x(c, 0);
}

// Result StartRestorer(DeviceHandle *, const AmiiboSettingsStartParam &, const TagInfo &): the applet that restores a
// damaged figure from the console's backup; a simulated figure is never damaged
static void ov_restorer(Cpu *c) {
    rt_log("nfp: amiibo restorer applet asked for: nothing to restore");
    ret_x(c, 0);
}

HOST_OVERRIDE("_ZN2nn3nfp13GetCommonInfoEPNS0_10CommonInfoERKNS0_12DeviceHandleE", ov_common_info);
HOST_OVERRIDE("_ZN2nn3nfp15GetRegisterInfoEPNS0_12RegisterInfoERKNS0_12DeviceHandleE", ov_register_info);
HOST_OVERRIDE("_ZN2nn3nfp19OpenApplicationAreaERKNS0_12DeviceHandleEj", ov_open_area);
HOST_OVERRIDE("_ZN2nn3nfp18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm", ov_get_area);
HOST_OVERRIDE("_ZN2nn3nfp18SetApplicationAreaERKNS0_12DeviceHandleEPKvm", ov_set_area);
HOST_OVERRIDE("_ZN2nn3nfp21CreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE", ov_create_area);
HOST_OVERRIDE("_ZN2nn3nfp23RecreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE",
              ov_recreate_area);
HOST_OVERRIDE("_ZN2nn3nfp5FlushERKNS0_12DeviceHandleE", ov_flush);
HOST_OVERRIDE("_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE",
              ov_nickname_settings);
HOST_OVERRIDE("_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoERKS4_",
              ov_nickname_settings);
HOST_OVERRIDE("_ZN2nn3nfp19StartGameDataEraserEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE",
              ov_data_eraser);
HOST_OVERRIDE("_ZN2nn3nfp13StartRestorerEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE",
              ov_restorer);
