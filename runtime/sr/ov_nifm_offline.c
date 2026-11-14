// Host overrides for nn::nifm, the network interface manager, which totk-pc-recomp's runtime does not serve. The host
// is an offline console: the library initializes, a network request completes without a connection, and the network
// is never available. Calls that need a connection fail with a nifm result and are logged once each.
#include <stdint.h>

#include "../core/host.h"

// nifm module (110). The description is not one the game is expected to tell apart: it only sees a failure.
#define R_NIFM_NO_NETWORK (110u | (300u << 9))

#define ONCE(msg)                                                                                                    \
    do {                                                                                                             \
        static int seen_;                                                                                            \
        if (!__atomic_exchange_n(&seen_, 1, __ATOMIC_RELAXED)) rt_log("nifm: " msg);                                  \
    } while (0)

static void ov_nifm_initialize(Cpu *c) { ONCE("initialized (offline host: no network)"); ret_x(c, 0); }
static void ov_nifm_available(Cpu *c) { ret_x(c, 0); }
static void ov_nifm_local_mode(Cpu *c) { (void)c; }
static void ov_nifm_cancel(Cpu *c) { (void)c; }
// void SubmitNetworkRequestAndWait(): the request ends at once, unfulfilled (IsNetworkAvailable stays false).
static void ov_nifm_submit_wait(Cpu *c) { (void)c; ONCE("network request submitted: none available"); }
// Result HandleNetworkRequestErrorResult(): no error applet to show; the request stays failed.
static void ov_nifm_handle_error(Cpu *c) { ONCE("network request error: no error applet, still offline"); ret_x(c, R_NIFM_NO_NETWORK); }
// Result GetCurrentIpConfigInfo(addr *, mask *, gateway *, dns1 *, dns2 *), GetCurrentProxySetting(ProxySetting *)
static void ov_nifm_no_network(Cpu *c) { ONCE("connection settings asked: no connection"); ret_x(c, R_NIFM_NO_NETWORK); }

HOST_OVERRIDE("_ZN2nn4nifm10InitializeEv", ov_nifm_initialize);
HOST_OVERRIDE("_ZN2nn4nifm18IsNetworkAvailableEv", ov_nifm_available);
HOST_OVERRIDE("_ZN2nn4nifm19SetLocalNetworkModeEb", ov_nifm_local_mode);
HOST_OVERRIDE("_ZN2nn4nifm20CancelNetworkRequestEv", ov_nifm_cancel);
HOST_OVERRIDE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", ov_nifm_submit_wait);
HOST_OVERRIDE("_ZN2nn4nifm31HandleNetworkRequestErrorResultEv", ov_nifm_handle_error);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentIpConfigInfoEP7in_addrS2_S2_S2_S2_", ov_nifm_no_network);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentIpConfigInfoEPNS_6socket6InAddrES3_S3_S3_S3_", ov_nifm_no_network);
HOST_OVERRIDE("_ZN2nn4nifm22GetCurrentProxySettingEPNS0_12ProxySettingE", ov_nifm_no_network);
