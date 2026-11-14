// sr-recomp: names beyond the TotK list (nvn_procs_extra.inc, SR_NVN_EXTRA) get logged stubs returning 0.
// NVN proc table. Every name the game's proc loader asks for gets a synthetic host
// address through hostfn_register before hostfn_seal; nvnBootstrapLoader and nvnDeviceGetProcAddress hand them out.
// nvnp_<name> is the implementation of one proc: the weak definitions here stop with the proc's name, nvn_api_*.c
// and nvn_ext.c define the strong ones. The name list is runtime/nvn_procs.inc (tools/gen_nvn_procs.py).
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "host.h"
#include "platform/platform.h"

// The strong nvnp_nvnDeviceGetProcAddress below replaces the stand-in generated for the same row.
#define nvnp_nvnDeviceGetProcAddress nvnp_stub_nvnDeviceGetProcAddress

#define NVN_PROC(n) void nvnp_##n(Cpu *c);
#include "nvn_procs.inc"
#undef NVN_PROC

#define NVN_PROC(n)                                                                                                  \
    __attribute__((weak)) void nvnp_##n(Cpu *c) {                                                                     \
        (void)c;                                                                                                     \
        rt_fatal("NVN " #n " is not implemented");                                                                   \
    }
#include "nvn_procs.inc"
#undef NVN_PROC
#undef nvnp_nvnDeviceGetProcAddress
void nvnp_nvnDeviceGetProcAddress(Cpu *c);

// sr-recomp: the NVN names the title's main holds that the TotK list does not (NVN2 entry points, mostly), written by
// tools/build_runtime.py into nvn_procs_extra.inc. Each gets its own synthetic address, as nx2android's nvn_thunks.S
// does, so a call names itself: it is logged once and returns 0 instead of jumping through NULL.
#ifdef SR_NVN_EXTRA
// Weak: runtime/sr/nvn2_api.c defines the strong nvnx_<name> of each NVN2 entry point it implements.
#define NVN_PROC(n)                                                                                                  \
    void nvnx_##n(Cpu *c);                                                                                           \
    __attribute__((weak)) void nvnx_##n(Cpu *c) {                                                                    \
        static int seen;                                                                                             \
        if (!__atomic_exchange_n(&seen, 1, __ATOMIC_RELAXED)) rt_log("nvn: %s is not implemented (returns 0)", #n); \
        ret_x(c, 0);                                                                                                 \
    }
#include "nvn_procs_extra.inc"
#undef NVN_PROC
#define NVN_EXTRA_COUNT(n) +1
#define NVN_PROC(n) NVN_EXTRA_COUNT(n)
enum { PROC_EXTRA = 0
#include "nvn_procs_extra.inc"
};
#undef NVN_PROC
#else
enum { PROC_EXTRA = 0 };
#endif

typedef struct { const char *name; uint64_t addr; } Proc;

#define NVN_PROC(n) +1
enum { PROC_COUNT = 0
#include "nvn_procs.inc"
};
#undef NVN_PROC

static Proc procs[PROC_COUNT + PROC_EXTRA];
static int nprocs;

static int by_name(const void *a, const void *b) { return strcmp(((const Proc *)a)->name, ((const Proc *)b)->name); }

void nvn_procs_register(void) {
    if (nprocs) return;
#define NVN_PROC(n) procs[nprocs++] = (Proc){#n, hostfn_register("nvn:" #n, nvnp_##n)};
#include "nvn_procs.inc"
#undef NVN_PROC
#ifdef SR_NVN_EXTRA
#define NVN_PROC(n) procs[nprocs++] = (Proc){#n, hostfn_register("nvn:" #n, nvnx_##n)};
#include "nvn_procs_extra.inc"
#undef NVN_PROC
    rt_log("nvn: %d procs, %d of them not implemented beyond a logged stub (nvn_procs_extra.inc)", nprocs,
           (int)PROC_EXTRA);
#endif
    qsort(procs, (size_t)nprocs, sizeof *procs, by_name);
}

uint64_t nvn_proc_address(const char *name) {
    if (!name) return 0;
    const Proc *p = bsearch(&(Proc){name, 0}, procs, (size_t)nprocs, sizeof *procs, by_name);
    if (p) return p->addr;
    // Unknown names get NULL, as on hardware; each distinct one is logged once.
    static PlatMutex lock = PLAT_MUTEX_INIT;
    static char seen[32][96];
    static int nseen;
    plat_mutex_lock(&lock);
    int fresh = 1;
    for (int i = 0; i < nseen; i++)
        if (!strncmp(seen[i], name, sizeof seen[i] - 1)) fresh = 0;
    if (fresh && nseen < 32) {
        strncpy(seen[nseen], name, sizeof seen[0] - 1);
        nseen++;
    } else {
        fresh = 0;
    }
    plat_mutex_unlock(&lock);
    if (fresh) rt_log("nvn: no such proc \"%s\", returning NULL", name);
    return 0;
}

// PFNNVNGENERICFUNC nvnDeviceGetProcAddress(const NVNdevice *device, const char *name)
void nvnp_nvnDeviceGetProcAddress(Cpu *c) {
    const char *name = arg_p(c, 1);
    ret_x(c, nvn_proc_address(name));
}
