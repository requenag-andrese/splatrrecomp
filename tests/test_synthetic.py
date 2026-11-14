"""Tests on synthetic, hand-built containers only: no game data is needed or used."""
import io
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), 'tools'))

from srrecomp import npdm, sources  # noqa: E402
from srrecomp.nso import Nso  # noqa: E402
from srrecomp.symbols import namespace  # noqa: E402
from exefs_inventory import svc_scan  # noqa: E402


def make_nso(text_words):
    text = b''.join(struct.pack('<I', w) for w in text_words)
    ro = b'\0' * 8 + b'x' * 0x18
    data = b'\0' * 0x10
    hdr = bytearray(0x100)
    hdr[0:4] = b'NSO0'
    off = 0x100
    for i, (seg, moff) in enumerate(((text, 0), (ro, 0x1000), (data, 0x2000))):
        struct.pack_into('<III', hdr, 0x10 + i * 0x10, off, moff, len(seg))
        struct.pack_into('<I', hdr, 0x60 + i * 4, len(seg))
        off += len(seg)
    return bytes(hdr) + text + ro + data


def make_pfs0(files):
    names = b''
    ents = b''
    blob = b''
    for name, data in files.items():
        ents += struct.pack('<QQII', len(blob), len(data), len(names), 0)
        names += name.encode() + b'\0'
        blob += data
    return b'PFS0' + struct.pack('<II', len(files), len(names)) + b'\0' * 4 + ents + names + blob


class T(unittest.TestCase):
    def test_pfs0(self):
        raw = make_pfs0({'a': b'123', 'main.npdm': b'META'})
        files = sources.parse_pfs0(sources.FileSlice(io.BytesIO(raw), 0, len(raw)))
        self.assertEqual(files['a'].read(), b'123')
        self.assertEqual(files['main.npdm'].read(0, 4), b'META')

    def test_nso_and_svc(self):
        words = [0x14000001, 0xD4000521, 0xD65F03C0, 0xD4000521, 0xD4000001]  # b; svc 0x29; ret; svc 0x29; svc 0
        nso = Nso(make_nso(words))
        self.assertEqual(nso.text, (0, 20))
        self.assertEqual(svc_scan(nso.text_words()), {0x29: 2, 0: 1})

    def test_kcaps(self):
        # EnableSystemCalls index 1, bit 5 -> svc 29; index 0, bit 1 -> svc 1
        d1 = (1 << 29) | (1 << (5 + 5)) | 0xF
        d0 = (0 << 29) | (1 << (5 + 1)) | 0xF
        svcs, other = npdm._kcaps(struct.pack('<III', d1, d0, 0b0111))
        self.assertEqual(svcs, [1, 29])
        self.assertEqual(other, {'ThreadInfo': 1})

    def test_namespace(self):
        self.assertEqual(namespace('_ZN2nn2fs8OpenFileEv'), 'nn::fs')
        self.assertEqual(namespace('nvnDeviceInitialize'), 'C:nvn')
        self.assertEqual(namespace('_Z3foov'), '::(global)')

    def test_sarc_members_linear(self):
        from shader_census import sarc_members_linear
        names = b'a.bin\0\0\0bb\0\0'
        n = 3
        hl = 0x14
        sfat = b'SFAT' + struct.pack('<HHI', 12, n, 0x65)
        sfat += struct.pack('<IIII', 0, 1 << 24 | 0, 0, 3)
        sfat += struct.pack('<IIII', 0, 1 << 24 | 2, 4, 6)
        sfat += struct.pack('<IIII', 0, 0, 8, 8)
        sfnt = b'SFNT' + struct.pack('<HH', 8, 0) + names
        doff = hl + len(sfat) + len(sfnt)
        body = b'xyz\0pq\0\0'
        d = b'SARC' + struct.pack('<HHII', hl, 0xFEFF, doff + len(body), doff) + struct.pack('<HH', 0x100, 0) + sfat + sfnt + body
        self.assertEqual(list(sarc_members_linear(d)), [('a.bin', b'xyz'), ('bb', b'pq'), ('#2', b'')])

    def test_spirv_structural(self):
        from srrecomp import spirv_check

        def ins(op, *ops):
            return [(len(ops) + 1) << 16 | op, *ops]
        body = (ins(17, 1) + ins(14, 0, 1) + ins(15, 4, 3, 0x6E69616D, 0) + ins(19, 1) + ins(33, 2, 1)
                + ins(54, 1, 3, 0, 2) + ins(248, 4) + ins(253) + ins(56))
        good = struct.pack(f'<{5 + len(body)}I', 0x07230203, 0x10000, 0, 5, 0, *body)
        self.assertIsNone(spirv_check.check(good))
        no_ret = body[:-2] + body[-1:]  # drop OpReturn: the block has no terminator
        bad = struct.pack(f'<{5 + len(no_ret)}I', 0x07230203, 0x10000, 0, 5, 0, *no_ret)
        self.assertEqual(spirv_check.check(bad), 'block without terminator')
        self.assertEqual(spirv_check.check(b'\0' * 20), 'magic')

    def test_oob_size_query(self):
        from srrecomp.spirv_txq import oob_query_bindings

        def ins(op, *ops):
            return [(len(ops) + 1) << 16 | op, *ops]
        body = (ins(23, 5, 4, 2)          # %5 = ivec2
                + ins(71, 10, 33, 1)      # Decorate %10 Binding 1
                + ins(61, 20, 11, 10)     # %11 = OpLoad %10
                + ins(100, 21, 12, 11)    # %12 = OpImage %11
                + ins(103, 5, 13, 12, 3)  # %13 = OpImageQuerySizeLod ivec2 %12 lod
                + ins(81, 4, 14, 13, 1)   # extract .y: fine
                + ins(81, 4, 15, 13, 2))  # extract .z: out of bounds
        data = struct.pack(f'<{5 + len(body)}I', 0x07230203, 0x10000, 0, 30, 0, *body)
        self.assertEqual(oob_query_bindings(data), {1})
        ok = data[:-20]  # without the out-of-bounds extract
        self.assertEqual(oob_query_bindings(ok), set())

    def test_refuses_encrypted_nca(self):
        raw = b'\0' * 0xC00
        with self.assertRaises(sources.PlaintextRequired):
            sources.Nca(sources.FileSlice(io.BytesIO(raw), 0, len(raw)))


if __name__ == '__main__':
    unittest.main()
