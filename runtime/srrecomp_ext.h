// Runtime helpers the srrecomp lifter extension (srrecomp/lifter_ext.py) emits calls to.
// Force-included when compiling generated chunks (-include srrecomp_ext.h).
#pragma once
#include <stdint.h>

// PACGA: a 32-bit MAC of x keyed by m, in the top half. Any deterministic keyed function is enough: the game only
// compares PACGA results computed in the same process. Defined by the runtime.
uint64_t rt_pacga(uint64_t x, uint64_t m);
