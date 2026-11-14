"""Made-up parameter tables in the game's format (BYML, little-endian), for the tests of runtime/sr: a writer and a
reader. The game's own tables never enter the repository.

Python values: dict, list, str, bool, float, int (signed; U(n) for an unsigned one), HashMap({key: value}) for a hash
map with 32-bit keys and plain values."""
import struct


class U(int):
    """An unsigned 32-bit value (the tables' hashed names)."""


class HashMap(dict):
    """{32-bit key: int} written as a hash map node (0x20)."""


class Byml:
    """The writer. Equal containers are written once and shared, as the game's tool does, unless share is off."""

    def __init__(self, root, share=True):
        self.keys, self.strs = sorted(self._names(root, set())), sorted(self._values(root, set()))
        self.share, self.done, self.out = share, {}, bytearray(16)
        key_at, str_at = self._table(self.keys), self._table(self.strs)
        root_at = self._node(root)
        self.out[0:16] = b'YB' + struct.pack('<HIII', 7, key_at, str_at, root_at)

    def _names(self, v, acc):
        if isinstance(v, HashMap):
            return acc
        for k, x in (v.items() if isinstance(v, dict) else enumerate(v) if isinstance(v, list) else ()):
            if isinstance(v, dict):
                acc.add(k)
            self._names(x, acc)
        return acc

    def _values(self, v, acc):
        if isinstance(v, str):
            acc.add(v)
        if isinstance(v, HashMap):
            return acc
        for x in (v.values() if isinstance(v, dict) else v if isinstance(v, list) else ()):
            self._values(x, acc)
        return acc

    def _pad(self):
        self.out += b'\0' * (-len(self.out) % 4)

    def _table(self, names):
        self._pad()
        at, body, offs = len(self.out), b'', []
        base = 4 + 4 * (len(names) + 1)
        for n in names:
            offs.append(base + len(body))
            body += n.encode() + b'\0'
        offs.append(base + len(body))
        self.out += bytes([0xC2]) + len(names).to_bytes(3, 'little') + struct.pack(f'<{len(offs)}I', *offs) + body
        return at

    def _leaf(self, v):
        if isinstance(v, bool):
            return 0xD0, int(v)
        if isinstance(v, U):
            return 0xD3, int(v)
        if isinstance(v, int):
            return 0xD1, v & 0xFFFFFFFF
        if isinstance(v, float):
            return 0xD2, struct.unpack('<I', struct.pack('<f', v))[0]
        if isinstance(v, str):
            return 0xA0, self.strs.index(v)
        if isinstance(v, HashMap):
            return 0x20, self._node(v)
        return (0xC1 if isinstance(v, dict) else 0xC0), self._node(v)

    def _node(self, v):
        mark = type(v).__name__ + repr(v)
        if self.share and mark in self.done:
            return self.done[mark]
        if isinstance(v, HashMap):
            pairs = sorted(v.items())
            body = b''.join(struct.pack('<II', x & 0xFFFFFFFF, k) for k, x in pairs)
            types = bytes(0xD1 for _ in pairs)
            body += types + b'\0' * (-len(types) % 4)
            head = bytes([0x20]) + len(pairs).to_bytes(3, 'little')
        elif isinstance(v, dict):
            items = [(self.keys.index(k), *self._leaf(x)) for k, x in sorted(v.items())]
            body = b''.join(k.to_bytes(3, 'little') + bytes([t]) + struct.pack('<I', raw) for k, t, raw in items)
            head = bytes([0xC1]) + len(items).to_bytes(3, 'little')
        else:
            items = [self._leaf(x) for x in v]
            types = bytes(t for t, _ in items)
            body = types + b'\0' * (-len(types) % 4) + b''.join(struct.pack('<I', raw) for _, raw in items)
            head = bytes([0xC0]) + len(items).to_bytes(3, 'little')
        self._pad()
        at = len(self.out)
        self.out += head + body
        self.done[mark] = at
        return at


def parse(data):
    """A table back as Python values."""
    keys_at, strs_at, root = struct.unpack_from('<III', data, 4)

    def names(at):
        n = int.from_bytes(data[at + 1:at + 4], 'little')
        offs = struct.unpack_from(f'<{n}I', data, at + 4)
        return [data[at + o:data.index(b'\0', at + o)].decode() for o in offs]

    keys, strs = names(keys_at), names(strs_at)

    def value(t, raw):
        if t == 0xD2:
            return struct.unpack('<f', struct.pack('<I', raw))[0]
        if t == 0xD1:
            return struct.unpack('<i', struct.pack('<I', raw))[0]
        if t == 0xD3:
            return U(raw)
        return {0xA0: lambda: strs[raw], 0xD0: lambda: bool(raw)}.get(t, lambda: node(raw))()

    def node(at):
        t, n = data[at], int.from_bytes(data[at + 1:at + 4], 'little')
        if t == 0x20:
            return HashMap({struct.unpack_from('<I', data, at + 8 + 8 * i)[0]: struct.unpack_from('<I', data, at + 4 + 8 * i)[0]
                            for i in range(n)})
        if t == 0xC1:
            return {keys[int.from_bytes(data[at + 4 + 8 * i:at + 7 + 8 * i], 'little')]:
                    value(data[at + 7 + 8 * i], struct.unpack_from('<I', data, at + 8 + 8 * i)[0]) for i in range(n)}
        base = at + 4 + (n + 3 & ~3)
        return [value(data[at + 4 + i], struct.unpack_from('<I', data, base + 4 * i)[0]) for i in range(n)]

    return node(root)


def sorted_keys(data):
    """True when every dictionary of the table has its keys in rising order of key index (the game bisects them)."""
    _, _, root = struct.unpack_from('<III', data, 4)

    def node(at):
        t, n = data[at], int.from_bytes(data[at + 1:at + 4], 'little')
        if t == 0xC1:
            ks = [int.from_bytes(data[at + 4 + 8 * i:at + 7 + 8 * i], 'little') for i in range(n)]
            kids = [(data[at + 7 + 8 * i], struct.unpack_from('<I', data, at + 8 + 8 * i)[0]) for i in range(n)]
            return ks == sorted(ks) and all(node(raw) for vt, raw in kids if vt in (0xC0, 0xC1))
        if t == 0xC0:
            base = at + 4 + (n + 3 & ~3)
            return all(node(struct.unpack_from('<I', data, base + 4 * i)[0]) for i in range(n) if data[at + 4 + i] in (0xC0, 0xC1))
        return True

    return node(root)


def murmur3(name):
    """MurmurHash3 (x86, 32 bits, seed 0) of a name: how the game makes its ids."""
    data = name.encode()
    c1, c2, h = 0xcc9e2d51, 0x1b873593, 0
    n = len(data) & ~3
    for i in range(0, n, 4):
        k = struct.unpack_from('<I', data, i)[0]
        k = (k * c1) & 0xFFFFFFFF
        k = ((k << 15) | (k >> 17)) & 0xFFFFFFFF
        k = (k * c2) & 0xFFFFFFFF
        h ^= k
        h = ((h << 13) | (h >> 19)) & 0xFFFFFFFF
        h = (h * 5 + 0xe6546b64) & 0xFFFFFFFF
    k, t = 0, data[n:]
    if len(t) >= 3:
        k ^= t[2] << 16
    if len(t) >= 2:
        k ^= t[1] << 8
    if len(t) >= 1:
        k ^= t[0]
        k = (k * c1) & 0xFFFFFFFF
        k = ((k << 15) | (k >> 17)) & 0xFFFFFFFF
        k = (k * c2) & 0xFFFFFFFF
        h ^= k
    h ^= len(data)
    h ^= h >> 16
    h = (h * 0x85ebca6b) & 0xFFFFFFFF
    h ^= h >> 13
    h = (h * 0xc2b2ae35) & 0xFFFFFFFF
    h ^= h >> 16
    return h
