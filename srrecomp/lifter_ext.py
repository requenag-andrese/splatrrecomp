"""Extension layer over a vendored lifter (totk-pc-recomp / LASRecomp) for the ARMv8.3 instructions the exefs
inventory found uncovered. It decodes only the encodings below and hands every other word to the base lifter, so the
vendored code stays untouched.

Pointer authentication (FEAT_PAuth): the PAC hints (PACIASP, AUTIASP, ...) already lift to NOPs, so return
addresses and pointers are never signed. Every PAC/AUT/XPAC on a register is therefore the identity, and the
authenticated branches (RETAA/RETAB, BRAA, BLRAA, ...) are plain RET/BR/BLR. PACGA computes a 32-bit MAC that
the game only compares with other PACGA results in the same process; it needs any deterministic keyed function,
provided by the runtime as `rt_pacga(x, m)` (top 32 bits, low 32 zero).

LDAPR/LDAPRB/LDAPRH (FEAT_LRCPC) are RCpc acquire loads; LDAR's RCsc ordering is stronger, so they reuse the
base lifter's `lda{N}`.

FABS/FNEG on FP16 vectors (FEAT_FP16) flip or clear lane sign bits; lowered to C only in simd='c' mode (in
'asm' mode the base lifter passes them through as inline assembly).
"""

RUNTIME_HELPERS = ('rt_pacga',)

PAC_NOTE = '/* pac/aut/xpac: pointers are never signed (PAC hints lift to NOPs) */'
FP16_SIGN = '0x8000800080008000ull'


def bits(w, lo, n):
    return (w >> lo) & ((1 << n) - 1)


class ExtLifter:
    """Drop-in for the vendored Lifter: unknown attributes (base address, sc, ...) come from the wrapped one."""

    def __init__(self, inner, mod):
        self._inner = inner
        self._m = mod

    def __getattr__(self, name):
        if name.startswith('__') or name in ('_inner', '_m'):  # not set yet (copy/pickle): no recursion
            raise AttributeError(name)
        return getattr(self._inner, name)

    def lift(self, w, pc):
        r = self.ext(w, pc)
        if r is None:
            return self._inner.lift(w, pc)
        return r

    def ext(self, w, pc):
        m = self._m
        # Authenticated branches: 1101011 Z 0 op(2) 11111 0000 1 M Rn Rm
        if w >> 25 == 0b1101011 and bits(w, 16, 5) == 31 and bits(w, 12, 4) == 0 and bits(w, 11, 1):
            opc, rn, rm = bits(w, 21, 4), bits(w, 5, 5), bits(w, 0, 5)
            if opc == 0 and rm == 31:      # BRAAZ / BRABZ
                return m.Insn('br', target=m.X(rn))
            if opc == 1 and rm == 31:      # BLRAAZ / BLRABZ
                return m.Insn('blr', target=m.X(rn))
            if opc == 2 and rn == 31 and rm == 31:  # RETAA / RETAB
                return m.Insn('ret')
            if opc == 8:                   # BRAA / BRAB (modifier in Xm|SP)
                return m.Insn('br', target=m.X(rn))
            if opc == 9:                   # BLRAA / BLRAB
                return m.Insn('blr', target=m.X(rn))
            return None
        # LDAPR / LDAPRB / LDAPRH: size 111000 101 11111 110000 Rn Rt
        if (w & 0x3FFFFC00) == 0x38BFC000:
            size, rn, rt = bits(w, 30, 2), bits(w, 5, 5), bits(w, 0, 5)
            return m.Insn('op', m.set_reg(1, rt, f'lda{8 << size}({m.XS(rn)})'))
        # PACIA..AUTDB, PACIZA..AUTDZB, XPACI, XPACD: 1 1 0 11010110 00001 opcode Rn Rd
        if (w & 0xFFFF0000) == 0xDAC10000 and bits(w, 10, 6) <= 0x11:
            return m.Insn('op', PAC_NOTE)
        # PACGA Xd, Xn, Xm|SP: 1 0 0 11010110 Rm 001100 Rn Rd
        if (w & 0xFFE0FC00) == 0x9AC03000:
            rm, rn, rd = bits(w, 16, 5), bits(w, 5, 5), bits(w, 0, 5)
            return m.Insn('op', m.set_reg(1, rd, f'rt_pacga({m.X(rn)}, {m.XS(rm)})'))
        # FABS / FNEG (vector, half precision): 0 Q U 01110 1 11100 0 01111 10 Rn Rd
        if self._inner.sc is not None and (w & 0x9FFFFC00) == 0x0EF8F800:
            q, u, rn, rd = bits(w, 30, 1), bits(w, 29, 1), bits(w, 5, 5), bits(w, 0, 5)
            mask = f'(((__uint128_t){FP16_SIGN} << 64) | {FP16_SIGN})' if q else FP16_SIGN
            src = f'c->v[{rn}]' if q else f'(uint64_t)c->v[{rn}]'
            op = f'{src} ^ {mask}' if u else f'{src} & ~{mask}'
            return m.Insn('op', f'c->v[{rd}] = {op};')
        return None
