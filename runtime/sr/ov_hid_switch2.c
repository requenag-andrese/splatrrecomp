// nn::hid entry points main imports that totk-pc-recomp's runtime does not override (ov_input.c covers the Npad).
// Their SDK bodies reach the hid service, which the host does not serve, so each is answered here as a console with
// no such device: no touches, no vibration, and none of Switch 2's Joy-Con 2 mouse sensors (Jcms, Nmouse). The
// six-axis sensor and the device handles are served by core/ov_input.c (public StartSixAxisSensor /
// GetSixAxisSensorStates included, fed from the gamepad's gyro and accelerometer); only the zero-drift mode is here. The software-only parts of the vibration API (VibrationPlayer, VibrationMixer, VibrationNode) keep
// running as lifted code; only VibrationWriter's way out to SendVibrationValue lands here.
//
// The layouts of Switch 2's Jcms / Nmouse types are not public: those calls only report "none", writing no structure
// whose size is not known.
#include <stdint.h>
#include <string.h>

#include "../core/host.h"

static void *gp(uint64_t a) { return (void *)(uintptr_t)a; }

static void log_once(int *seen, const char *what) {
    if (!*seen) {
        *seen = 1;
        rt_log("hid: %s: no such device on this host", what);
    }
}

// ---- touch screen
static int touch_seen;
// void InitializeTouchScreen()
static void ov_touch_init(Cpu *c) { (void)c; log_once(&touch_seen, "touch screen"); }
// int GetTouchScreenStates<16>(TouchScreenState<16> *out, int count): one state with no touches (sampling number 0,
// touch count 0). The rest of the structure is left as the caller zeroed or not: with a count of 0 none of it is read.
static void ov_touch_states(Cpu *c) {
    if ((int64_t)c->x[1] < 1) { c->x[0] = 0; return; }
    memset(gp(c->x[0]), 0, 16);   // int64 sampling number, int32 count, reserved
    c->x[0] = 1;
}

// ---- six-axis sensors (the rest is core/ov_input.c)
// GyroscopeZeroDriftMode GetGyroscopeZeroDriftMode(const SixAxisSensorHandle &): Standard (1), the SDK's default
static void ov_zero_drift_get(Cpu *c) { c->x[0] = 1; }
// void SetGyroscopeZeroDriftMode(const SixAxisSensorHandle &, const GyroscopeZeroDriftMode &)
static void ov_zero_drift_set(Cpu *c) { (void)c; }

// ---- vibration devices
static int vib_seen;
// The handles come from core/ov_input.c, whose detail:: entry points rumble the gamepad; these public ones drop values.
// void InitializeVibrationDevice(const VibrationDeviceHandle &)
static void ov_vib_init(Cpu *c) { (void)c; log_once(&vib_seen, "vibration device"); }
// void SendVibrationValue(const VibrationDeviceHandle &, const VibrationValue &)
static void ov_vib_send(Cpu *c) { (void)c; }

// ---- Switch 2 Joy-Con 2 mouse sensors (Jcms) and mouse (Nmouse): none connected
static int jcms_seen, nmouse_seen;
static void ov_jcms_init(Cpu *c) { (void)c; log_once(&jcms_seen, "Joy-Con 2 mouse sensor (Jcms)"); }   // InitializeJcms(void *, size_t)
static void ov_jcms_none(Cpu *c) { (void)c; }                                    // FinalizeJcms, StartJcms, StopJcms
// JcmsHandle GetJcmsHandle(const uint32_t &id, const JcmsType &type): a handle naming the player, as the Npad ids do
static void ov_jcms_handle(Cpu *c) { c->x[0] = c->x[0] ? *(uint32_t *)gp(c->x[0]) : 0; }
// int GetJcmsStates(JcmsState *out, int count, const JcmsHandle &): no state
static void ov_jcms_states(Cpu *c) { c->x[0] = 0; }
// int GetNmouseHandles(NmouseHandle *out, int count, uint32_t): no mouse
static void ov_nmouse_handles(Cpu *c) { log_once(&nmouse_seen, "Joy-Con 2 mouse (Nmouse)"); c->x[0] = 0; }
// BitFlagSet<32, NmouseType> GetNmouseTypeSet(NmouseHandle): the empty set, returned in w0 (never reached without a handle)
static void ov_nmouse_types(Cpu *c) { c->x[0] = 0; }
static void ov_nmouse_finalize(Cpu *c) { (void)c; }

HOST_OVERRIDE("_ZN2nn3hid21InitializeTouchScreenEv", ov_touch_init);
HOST_OVERRIDE("_ZN2nn3hid20GetTouchScreenStatesILm16EEEiPNS0_16TouchScreenStateIXT_EEEi", ov_touch_states);
HOST_OVERRIDE("_ZN2nn3hid25GetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleE", ov_zero_drift_get);
HOST_OVERRIDE("_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE",
              ov_zero_drift_set);
HOST_OVERRIDE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", ov_vib_init);
HOST_OVERRIDE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", ov_vib_send);
HOST_OVERRIDE("_ZN2nn3hid14InitializeJcmsEPvm", ov_jcms_init);
HOST_OVERRIDE("_ZN2nn3hid12FinalizeJcmsEv", ov_jcms_none);
HOST_OVERRIDE("_ZN2nn3hid9StartJcmsERKNS0_10JcmsHandleE", ov_jcms_none);
HOST_OVERRIDE("_ZN2nn3hid8StopJcmsERKNS0_10JcmsHandleE", ov_jcms_none);
HOST_OVERRIDE("_ZN2nn3hid13GetJcmsHandleERKjRKNS0_8JcmsTypeE", ov_jcms_handle);
HOST_OVERRIDE("_ZN2nn3hid13GetJcmsStatesEPNS0_9JcmsStateEiRKNS0_10JcmsHandleE", ov_jcms_states);
HOST_OVERRIDE("_ZN2nn3hid16GetNmouseHandlesEPNS0_12NmouseHandleEij", ov_nmouse_handles);
HOST_OVERRIDE("_ZN2nn3hid16GetNmouseTypeSetEPNS_4util10BitFlagSetILi32ENS0_10NmouseTypeEEENS0_12NmouseHandleE",
              ov_nmouse_types);
HOST_OVERRIDE("_ZN2nn3hid14FinalizeNmouseEv", ov_nmouse_finalize);
