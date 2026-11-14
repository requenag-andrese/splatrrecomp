// NVN2 (Switch 2) entry points that totk-pc-recomp's NVN1 front end does not have. Each is the strong nvnx_<name> that
// replaces the logged stub of runtime/core/nvn_procs.c (SR_NVN_EXTRA). Behaviour follows nx2android PR #88's
// nxnative dm/gfx/nvn/hle_nvn.c (MIT), section "NVN2 (Switch 2)".
#include <stdint.h>
#include <string.h>

#include "../core/host.h"
#include "../core/nvn.h"

void nvnp_nvnProgramInitialize(Cpu *c);
void nvnp_nvnProgramSetShaders(Cpu *c);

// ---- Programs. NVN2 builds them from a builder: SetDefaults, SetDevice, SetShaders(count, NVNshaderData[]), then
// InitializeFromBuilder. The builder keeps the caller's array, which lives until the program is initialized.
typedef struct {
    uint64_t device;
    uint64_t shaders;   // const NVNshaderData *
    int32_t count;
    int32_t pad;
} prog_builder;

static prog_builder *builder(Cpu *c) {
    prog_builder *b = arg_p(c, 0);
    if (!b) rt_fatal("nvn: null NVNprogramBuilder");
    return b;
}
void nvnx_nvnProgramBuilderSetDefaults(Cpu *c) { memset(builder(c), 0, sizeof(prog_builder)); }
void nvnx_nvnProgramBuilderSetDevice(Cpu *c) { builder(c)->device = arg_x(c, 1); }
void nvnx_nvnProgramBuilderSetShaders(Cpu *c) {
    prog_builder *b = builder(c);
    b->count = (int32_t)arg_x(c, 1);
    b->shaders = arg_x(c, 2);
}
// NVNboolean nvnProgramInitializeFromBuilder(NVNprogram *p, const NVNprogramBuilder *b)
void nvnx_nvnProgramInitializeFromBuilder(Cpu *c) {
    uint64_t program = arg_x(c, 0);
    prog_builder b = *(const prog_builder *)arg_p(c, 1);
    c->x[0] = program, c->x[1] = b.device;
    nvnp_nvnProgramInitialize(c);
    if (!(c->x[0] & 0xff) || !b.count) return;
    c->x[0] = program, c->x[1] = (uint64_t)(int64_t)b.count, c->x[2] = b.shaders;
    nvnp_nvnProgramSetShaders(c);
}

// ---- Queues. A queue tied to a primary queue: the back end runs every queue's submissions in order on one device
// queue already.
void nvnx_nvnQueueBuilderSetPrimaryQueue(Cpu *c) { (void)c; }

// NVNboolean nvnQueueWaitSync(NVNqueue *q, const NVNsync *s): a GPU-side wait. The back end replays every queue's
// work in submission order on one device queue, so anything fenced before is already ordered ahead (as nxnative).
// An NVN1 entry point TotK never called: the strong nvnp_ replaces the weak fatal of runtime/core/nvn_procs.c.
void nvnp_nvnQueueWaitSync(Cpu *c) { ret_x(c, 1); }

// How the GPU front end runs a command buffer (recording, geometry processing, compute overlap): the replay runs every
// command in order, so none changes a result (as nxnative).
void nvnx_nvnCommandBufferSetRecordingMode(Cpu *c) { (void)c; }
void nvnx_nvnCommandBufferSetGeometryProcessingMode(Cpu *c) { (void)c; }
void nvnx_nvnCommandBufferSetComputeOverlapMode(Cpu *c) { (void)c; }

// Stencil culling tunes the GPU's early rejection, not what is drawn (as nxnative). An NVN1 entry point TotK never
// called: the strong nvnp_ replaces the weak fatal.
void nvnp_nvnCommandBufferSetStencilCullCriteria(Cpu *c) { (void)c; }

// Variable-rate shading: the host shades every pixel at full rate, which draws the same picture at a higher cost.
static void vrs_off(void) {
    static int seen;
    if (!__atomic_exchange_n(&seen, 1, __ATOMIC_RELAXED)) rt_log("nvn: variable-rate shading requested: shaded at full rate");
}
void nvnx_nvnCommandBufferShadingRateImagePalette(Cpu *c) { (void)c; vrs_off(); }
void nvnx_nvnCommandBufferSetShadingRatePaletteMode(Cpu *c) { (void)c; vrs_off(); }
void nvnx_nvnCommandBufferBindShadingRateImage(Cpu *c) { (void)c; vrs_off(); }
