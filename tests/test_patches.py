"""srrecomp/patches (code patch sets by game version) and how tools/gen_c.py picks the file of the game being built.
The files hold addresses and instruction words, checked against the game when its C is generated; here only their
shape."""
import re
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))

import gen_c  # noqa: E402

PATCHES = ROOT / 'srrecomp' / 'patches'


def nso(module_id):
    """The head of a module file with this id."""
    return b'NSO0' + bytes(0x3c) + module_id + bytes(0x20 - len(module_id))


class PatchFilesTest(unittest.TestCase):
    def test_the_file_of_the_game_is_picked_by_mains_module_id(self):
        with tempfile.TemporaryDirectory() as d:
            exefs, sets = Path(d) / 'exefs', Path(d) / 'sets'
            exefs.mkdir()
            sets.mkdir()
            ident = bytes(range(1, 21))
            (exefs / 'main').write_bytes(nso(ident))
            args = ['--exefs', str(exefs), '--out', 'x']
            self.assertIsNone(gen_c.patches_for(str(sets), args))            # no file for this game: no sets
            (sets / (bytes(20).hex() + '.txt')).write_text('')               # another game's
            self.assertIsNone(gen_c.patches_for(str(sets), args))
            own = sets / (ident.hex() + '.txt')
            own.write_text('')
            self.assertEqual(Path(gen_c.patches_for(str(sets), args)), own)
            (exefs / 'main').write_bytes(b'XXXX' + nso(ident)[4:])           # not a module
            self.assertIsNone(gen_c.patches_for(str(sets), args))
            self.assertIsNone(gen_c.patches_for(str(sets), ['--out', 'x']))

    def test_the_files_are_well_formed(self):
        files = sorted(PATCHES.glob('*'))
        self.assertTrue(files)
        for f in files:
            self.assertRegex(f.name, r'^[0-9a-f]{40}\.txt$')
            sets, sites = {}, set()
            for no, raw in enumerate(f.read_text(encoding='utf-8').splitlines(), 1):
                line = raw.split('#', 1)[0].split()
                where = f'{f.name}:{no}'
                if not line:
                    continue
                if line[0] == 'set':
                    self.assertEqual(len(line), 4, where)
                    self.assertNotIn(line[1], sets, where)
                    self.assertRegex(line[2], r'^SR_PATCH_[A-Z0-9_]+$', where)
                    self.assertEqual(line[3], '0', where)     # the runtime turns a set on, never the file
                    sets[line[1]] = line[2]
                else:
                    self.assertEqual(line[0], 'site', where)
                    self.assertGreaterEqual(len(line), 6, where)
                    self.assertIn(line[1], sets, where)
                    self.assertIn(line[2], ('main', 'subsdk0', 'sdk'), where)
                    self.assertEqual(int(line[3], 16) % 4, 0, where)
                    self.assertNotIn((line[1], line[2], line[3]), sites, where)
                    sites.add((line[1], line[2], line[3]))
                    for w in line[4:]:
                        self.assertRegex(w, r'^[0-9a-f]{8}$', where)
            self.assertEqual(len(set(sets.values())), len(sets), f.name)


if __name__ == '__main__':
    unittest.main()
