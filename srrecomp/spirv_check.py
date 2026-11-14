"""Structural SPIR-V check (not a substitute for spirv-val): header, instruction framing, id bound, entry point,
balanced functions/blocks. Returns None when the module passes, else a short reason."""
import struct

OP_ENTRY_POINT, OP_FUNCTION, OP_FUNCTION_END, OP_LABEL = 15, 54, 56, 248
# Branch, BranchConditional, Switch, Kill, Return, ReturnValue, Unreachable, TerminateInvocation,
# IgnoreIntersectionKHR, TerminateRayKHR, EmitMeshTasksEXT
TERMINATORS = {249, 250, 251, 252, 253, 254, 255, 4416, 4448, 4449, 5294}


def check(data):
    if len(data) < 20 or len(data) % 4:
        return 'size'
    w = struct.unpack(f'<{len(data) // 4}I', data)
    if w[0] != 0x07230203:
        return 'magic'
    if w[1] >> 16 != 1 or w[1] & 0xFF:
        return 'version'
    bound = w[3]
    i, entry, funcs, ends, in_block = 5, 0, 0, 0, False
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xFFFF
        if n == 0 or i + n > len(w):
            return 'framing'
        if op == OP_ENTRY_POINT:
            entry += 1
        elif op == OP_FUNCTION:
            funcs += 1
            if w[i + 2] >= bound:
                return 'id bound'
        elif op == OP_FUNCTION_END:
            ends += 1
            if in_block:
                return 'block without terminator'
        elif op == OP_LABEL:
            if in_block:
                return 'block without terminator'
            in_block = True
            if w[i + 1] >= bound:
                return 'id bound'
        elif op in TERMINATORS:
            in_block = False
        i += n
    if not entry:
        return 'no entry point'
    if funcs != ends or not funcs:
        return 'unbalanced functions'
    return None
