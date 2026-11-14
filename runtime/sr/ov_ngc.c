// Host overrides for nn::ngc::ProfanityFilter, which talks to the ngc:u service (the console's word list from system
// data). The host has no word list: the filter initialises, and masking finds nothing, so the player's text is kept as
// typed. The constructor, destructor and nn::ngc::CountNumbers do not reach the service and run recompiled.
#include <stdint.h>

#include "../core/host.h"

// Result ProfanityFilter::Initialize(void *work, size_t size)
static void ov_ngc_init(Cpu *c) {
    static int seen;
    if (!__atomic_exchange_n(&seen, 1, __ATOMIC_RELAXED))
        rt_log("ngc: profanity filter without a word list (text is not masked)");
    ret_x(c, 0);
}
// void ProfanityFilter::SkipAtSignCheck(SkipMode)
static void ov_ngc_skip_at(Cpu *c) { (void)c; }
// Result ProfanityFilter::MaskProfanityWordsInText(int *masked, char *text, uint32_t regions): nothing masked
static void ov_ngc_mask(Cpu *c) {
    int32_t *masked = arg_p(c, 1);
    if (masked) *masked = 0;
    ret_x(c, 0);
}
// Result ProfanityFilter::Finalize()
static void ov_ngc_fin(Cpu *c) { ret_x(c, 0); }

HOST_OVERRIDE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvm", ov_ngc_init);
HOST_OVERRIDE("_ZN2nn3ngc15ProfanityFilter15SkipAtSignCheckENS1_8SkipModeE", ov_ngc_skip_at);
HOST_OVERRIDE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", ov_ngc_mask);
HOST_OVERRIDE("_ZN2nn3ngc15ProfanityFilter8FinalizeEv", ov_ngc_fin);
