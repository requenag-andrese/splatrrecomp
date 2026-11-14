// Host overrides for SDK functions a Switch 2 title calls that totk-pc-recomp's runtime does not override. Their SDK
// bodies talk to services through sessions the overridden initialisers never open (nn::oe::Initialize is the host's,
// ov_system.c), so running them lifted dereferences a null session.
#include <stdint.h>
#include <string.h>

#include "../core/host.h"

extern const char sr_display_version[];   // control.nacp display version (tools/build_runtime.py modules.c)

// ---- nn::oe

// void GetDisplayVersion(DisplayVersion *out): char[16], NUL-terminated
static void ov_display_version(Cpu *c) {
    char v[16] = {0};
    strncpy(v, sr_display_version, sizeof v - 1);
    memcpy((void *)(uintptr_t)c->x[0], v, sizeof v);
}

// GraphicsScale GetCurrentGraphicsScale(): the SDK maps the applet's value 20 to 1 (10 and 30 to the other two);
// the host answers 1, the value of a scale of 20.
static void ov_graphics_scale(Cpu *c) { c->x[0] = 1; }

// void SetGraphicsScaleChangedNotificationEnabled(bool): the scale never changes here, so no message will come.
static void ov_graphics_scale_notify(Cpu *c) {
    rt_log("oe: graphics scale change notifications %s (the scale stays fixed)", c->x[0] & 1 ? "on" : "off");
}

// void EnterExitRequestHandlingSection() / LeaveExitRequestHandlingSection(): the host never asks the game to exit.
static void ov_exit_section(Cpu *c) { (void)c; }

// void SendServerMaintenanceOverlayNotification(PosixTime begin, PosixTime end): no system overlay to show it.
static void ov_maintenance_overlay(Cpu *c) {
    rt_log("oe: server maintenance overlay %lld..%lld (not shown)", (long long)c->x[0], (long long)c->x[1]);
}

HOST_OVERRIDE("_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", ov_display_version);
HOST_OVERRIDE("_ZN2nn2oe23GetCurrentGraphicsScaleEv", ov_graphics_scale);
HOST_OVERRIDE("_ZN2nn2oe42SetGraphicsScaleChangedNotificationEnabledEb", ov_graphics_scale_notify);
HOST_OVERRIDE("_ZN2nn2oe31EnterExitRequestHandlingSectionEv", ov_exit_section);
HOST_OVERRIDE("_ZN2nn2oe31LeaveExitRequestHandlingSectionEv", ov_exit_section);
HOST_OVERRIDE("_ZN2nn2oe40SendServerMaintenanceOverlayNotificationENS_4time9PosixTimeES2_", ov_maintenance_overlay);

// ---- nn::audio: output volume auto-tune (IAudioDevice commands the host's audio device does not serve). The setting
// is kept for the getter, off at start, and not applied (as nx2android PR #88's nxnative hle_ounce.c).
static int volume_autotune;
static void ov_set_autotune(Cpu *c) { __atomic_store_n(&volume_autotune, (int)(c->x[0] & 1), __ATOMIC_RELAXED); }
static void ov_get_autotune(Cpu *c) { c->x[0] = (uint64_t)__atomic_load_n(&volume_autotune, __ATOMIC_RELAXED); }
HOST_OVERRIDE("_ZN2nn5audio40SetDeviceOutputVolumeAutoTuneModeEnabledEb", ov_set_autotune);
HOST_OVERRIDE("_ZN2nn5audio39IsDeviceOutputVolumeAutoTuneModeEnabledEv", ov_get_autotune);
