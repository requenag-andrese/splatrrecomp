// Host overrides for the nn::account network-service-account calls the title's main imports and totk-pc-recomp's
// runtime does not override. The host's user (totk ov_account.c) has no Nintendo Account linked: the account is
// reported as unavailable and every request for it fails with the account module's "unavailable" result, as on a
// console whose user never linked one. The SDK bodies would dereference the internals of a user handle that the host
// made itself.
#include <stdint.h>

#include "../core/host.h"

// nn::account::ResultNetworkServiceAccountUnavailable: module 124 (account), description 280.
#define R_NSA_UNAVAILABLE (124u | (280u << 9))

static void nsa_log(const char *what) {
    static int seen;
    if (!__atomic_exchange_n(&seen, 1, __ATOMIC_RELAXED))
        rt_log("account: %s: no network service account on this host (unavailable)", what);
}

// Result IsNetworkServiceAccountAvailable(bool *out, const UserHandle &)
static void ov_nsa_available(Cpu *c) {
    uint8_t *out = arg_p(c, 0);
    if (out) *out = 0;
    nsa_log("IsNetworkServiceAccountAvailable");
    ret_x(c, 0);
}
// Result GetNetworkServiceAccountId(NetworkServiceAccountId *, const UserHandle &), EnsureNetworkServiceAccountAvailable
// (const UserHandle &), LoadNetworkServiceAccountIdTokenCache(size_t *, char *, size_t, const UserHandle &),
// EnsureNetworkServiceAccountIdTokenCacheAsync(AsyncContext *, const UserHandle &)
static void ov_nsa_unavailable(Cpu *c) {
    nsa_log("network service account request");
    ret_x(c, R_NSA_UNAVAILABLE);
}

HOST_OVERRIDE("_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE", ov_nsa_available);
HOST_OVERRIDE("_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE",
              ov_nsa_unavailable);
HOST_OVERRIDE("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", ov_nsa_unavailable);
HOST_OVERRIDE("_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", ov_nsa_unavailable);
HOST_OVERRIDE("_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE",
              ov_nsa_unavailable);
