"""Measure which decoded AArch64 instructions a vendored lifter cannot lift.

Each lifter directory (totk-pc-recomp/recomp, LASRecomp/tools) holds `lifter.py` and `simd_c.py`, which import each
other by bare name, so each is loaded with a clean `sys.modules` entry and kept apart.

An instruction counts as uncovered when capstone decodes it and the lifter
  * raises its `Unsupported` exception,
  * raises any other exception (counted separately as `error`), or
  * returns a `trap` Insn for something other than UDF/padding (a silent gap).
"""
import importlib
import re
import sys
from collections import Counter

import capstone
import numpy as np

_HEX = re.compile(r'(0x)?\b[0-9a-fA-F]{6,16}\b')


def load_lifter(directory, simd, ext=False):
    """Returns (lifter, Unsupported). ext=True wraps it in srrecomp.lifter_ext.ExtLifter."""
    saved = {k: sys.modules.pop(k) for k in ('lifter', 'simd_c') if k in sys.modules}
    sys.path.insert(0, directory)
    try:
        mod = importlib.import_module('lifter')
        inst = mod.Lifter(0x80004000, simd)  # SimdC is imported inside the constructor
    finally:
        sys.path.remove(directory)
        for k in ('lifter', 'simd_c'):
            sys.modules.pop(k, None)
        sys.modules.update(saved)
    if ext:
        from srrecomp.lifter_ext import ExtLifter
        inst = ExtLifter(inst, mod)
    return inst, mod.Unsupported


class Decoder:
    def __init__(self):
        self.cs = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
        self.cache = {}

    def mnemonic(self, w):
        m = self.cache.get(w, 0)
        if m == 0:
            r = next(self.cs.disasm_lite(int(w).to_bytes(4, 'little'), 0x1000, 1), None)
            m = r[2] if r else None
            self.cache[w] = m
        return m


def unique_words(text_bytes):
    words = np.frombuffer(text_bytes[:len(text_bytes) // 4 * 4], dtype='<u4')
    u, c = np.unique(words, return_counts=True)
    return u.tolist(), c.tolist(), len(words)


def coverage(lifter, unsupported, dec, uw, counts, pc=0x80004000, top=40):
    by_mn = Counter()
    uniq_by_mn = Counter()
    reasons = Counter()
    kinds = Counter()
    decoded = covered = 0
    for w, n in zip(uw, counts):
        mn = dec.mnemonic(w)
        if mn is None:
            kinds['undecodable_words'] += n
            continue
        decoded += n
        try:
            r = lifter.lift(w, pc)
            if r.kind == 'trap' and mn not in ('udf',):
                status, why = 'silent_trap', 'returned trap'
            else:
                covered += n
                kinds['lifted_' + r.kind] += n
                continue
        except unsupported as e:
            # Drop instruction words from messages such as 'dp_reg 9ac00c20': keep only the category.
            status, why = 'unsupported', _HEX.sub('', str(e)).strip()[:60]
        except Exception as e:  # noqa: BLE001 - any crash is a coverage gap worth counting
            status, why = 'error', type(e).__name__
        kinds[status] += n
        by_mn[mn] += n
        uniq_by_mn[mn] += 1
        reasons[why] += n
    return {
        'decoded_instructions': decoded,
        'covered': covered,
        'uncovered': decoded - covered,
        'covered_pct': round(100 * covered / decoded, 4) if decoded else None,
        'kinds': dict(kinds),
        'uncovered_mnemonics': [{'mnemonic': m, 'occurrences': c, 'unique_encodings': uniq_by_mn[m]}
                                for m, c in by_mn.most_common(top)],
        'uncovered_mnemonic_count': len(by_mn),
        'reasons': dict(reasons.most_common(top)),
    }
