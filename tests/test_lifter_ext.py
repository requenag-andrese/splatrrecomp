"""Hand-assembled encodings, checked against capstone's decoding, run through the extension over the vendored lifter.
Skipped when ../vendor is not present."""
import os
import sys
import unittest

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VENDOR = os.path.join(os.path.dirname(ROOT), 'vendor', 'totk-pc-recomp', 'recomp')
sys.path.insert(0, ROOT)

from srrecomp import lifters  # noqa: E402

CASES = [
    # word, capstone mnemonic, expected Insn kind, substring of the C (or None)
    (0xD65F0FFF, 'retab', 'ret', None),
    (0xD65F0BFF, 'retaa', 'ret', None),
    (0xD71F0C22, 'brab', 'br', 'c->x[1]'),          # brab x1, x2
    (0xD63F083F, 'blraaz', 'blr', 'c->x[1]'),       # blraaz x1
    (0x38BFC020, 'ldaprb', 'op', 'lda8(c->x[1])'),  # ldaprb w0, [x1]
    (0x78BFC020, 'ldaprh', 'op', 'lda16(c->x[1])'),
    (0xB8BFC3E0, 'ldapr', 'op', 'lda32(c->sp)'),    # ldapr w0, [sp]
    (0xF8BFC020, 'ldapr', 'op', 'lda64(c->x[1])'),
    (0x9AC23020, 'pacga', 'op', 'rt_pacga(c->x[1], c->x[2])'),
    (0xDAC11420, 'autib', 'op', 'never signed'),
    (0xDAC143E0, 'xpaci', 'op', 'never signed'),
    (0xDAC10020, 'pacia', 'op', 'never signed'),
    (0x6EF8F820, 'fneg', 'op', 'c->v[0] = c->v[1] ^'),
    (0x0EF8F820, 'fabs', 'op', 'c->v[0] = (uint64_t)c->v[1] & ~'),
]


@unittest.skipUnless(os.path.isdir(VENDOR), 'vendor clone not present')
class T(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ext, cls.unsupported = lifters.load_lifter(VENDOR, 'c', ext=True)
        cls.base, _ = lifters.load_lifter(VENDOR, 'c')
        cls.dec = lifters.Decoder()

    def test_cases(self):
        for w, mn, kind, code in CASES:
            with self.subTest(word=f'{w:08x}'):
                self.assertEqual(self.dec.mnemonic(w), mn)
                r = self.ext.lift(w, 0x80004000)
                self.assertEqual(r.kind, kind)
                if code:
                    self.assertIn(code, r.code if kind == 'op' else r.target)

    def test_other_words_unchanged(self):
        for w in (0xD65F03C0, 0xD61F0020, 0xC8DFFC20, 0x910003FD, 0xD503233F):  # ret, br x1, ldar, mov x29,sp, paciasp
            a, b = self.ext.lift(w, 0x80004000), self.base.lift(w, 0x80004000)
            self.assertEqual((a.kind, a.code, a.target), (b.kind, b.code, b.target))


if __name__ == '__main__':
    unittest.main()
