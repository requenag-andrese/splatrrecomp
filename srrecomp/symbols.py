"""Group dynamic symbols by namespace so reports carry counts, not raw symbol lists."""
import re

_C_PREFIX = re.compile(r'^_*[a-z]+')


def _source_name(s, i):
    m = re.match(r'\d+', s[i:])
    if not m:
        return None, i
    n = int(m.group())
    j = i + len(m.group())
    return s[j:j + n], j + n


def namespace(sym, depth=2):
    """'_ZN2nn2fs8OpenFileE...' -> 'nn::fs'; C symbols -> 'C:<lowercase prefix>'."""
    if sym.startswith('_ZN'):
        i = 3
        while i < len(sym) and sym[i] in 'KVrLO':
            i += 1
        if sym.startswith('St', i):
            parts, i = ['std'], i + 2
        else:
            parts = []
        while len(parts) < depth:
            name, j = _source_name(sym, i)
            if not name:
                break
            parts.append(name)
            i = j
        # A namespace path ends one component before the entity name; keep only real prefixes.
        return '::'.join(parts) if parts else '?mangled'
    if sym.startswith('_ZSt') or sym.startswith('_ZNSt'):
        return 'std'
    if sym.startswith('_Z'):
        return '::(global)'
    m = _C_PREFIX.match(sym)
    return 'C:' + (m.group() if m else sym[:1])
