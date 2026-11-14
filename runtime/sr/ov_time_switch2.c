// Host overrides for the nn::time calls the title's main imports and totk-pc-recomp's runtime does not override. Time
// zones: the host has one, its own local time (totk ov_system.c ov_to_calendar), so the device location is reported
// as "UTC" by name only, LoadTimeZoneRule accepts any name without filling the rule, and ToCalendarTime with a rule
// gives the host's local time (registered in ov_system.c). The network clock is never synchronised (offline host).
#include <stdint.h>
#include <string.h>

#include "../core/host.h"

// void GetDeviceLocationName(LocationName *out): char[0x24]
static void ov_location_name(Cpu *c) {
    char *out = arg_p(c, 0);
    if (!out) rt_fatal("nn::time::GetDeviceLocationName: null output");
    memset(out, 0, 0x24);
    memcpy(out, "UTC", 3);
}
// Result LoadTimeZoneRule(TimeZoneRule *out, const LocationName &name)
static void ov_load_rule(Cpu *c) {
    static int seen;
    if (!__atomic_exchange_n(&seen, 1, __ATOMIC_RELAXED))
        rt_log("time: LoadTimeZoneRule: rules are the host's local time zone");
    ret_x(c, 0);
}
// bool StandardNetworkSystemClock::IsAccuracySufficient()
static void ov_network_accuracy(Cpu *c) { ret_x(c, 0); }

HOST_OVERRIDE("_ZN2nn4time21GetDeviceLocationNameEPNS0_12LocationNameE", ov_location_name);
HOST_OVERRIDE("_ZN2nn4time16LoadTimeZoneRuleEPNS0_12TimeZoneRuleERKNS0_12LocationNameE", ov_load_rule);
HOST_OVERRIDE("_ZN2nn4time26StandardNetworkSystemClock20IsAccuracySufficientEv", ov_network_accuracy);
