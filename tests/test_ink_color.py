"""runtime/sr/ink_color.c: the ink colours written into a team colour table. The tables here are made up (the game's own
never enters the repository): the same shape, other names' values. Needs llvm-mingw in .toolchains (skipped without)."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

import zstandard

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))

from byml_tables import Byml, parse  # noqa: E402

CLANG = sorted((ROOT.parent / '.toolchains').glob('llvm-mingw-*/bin/clang.exe'))


def colour(r, g, b):
    return {'A': 1.0, 'R': r, 'G': g, 'B': b}


def team(r, g, b, **more):
    return {'TeamColor': colour(r, g, b), 'InkRimIntensity': 0.5, **more}


def table(extra=None):
    rows = [{'__RowId': f'Work/Gyml/Hunt{n}.game__gfx__parameter__TeamColorDataSet.gyml', 'IsSetUIColor': False,
             'Alpha': team(0.2 * i, 0.2, 0.4), 'Bravo': team(0.8, 0.6, 0.2), 'Charlie': team(0.0, 0.0, 1.0)}
            for i, n in enumerate(('One', 'Two', 'Three'), 1)]
    rows.append({'__RowId': 'Work/Gyml/Support_One.game__gfx__parameter__TeamColorDataSet.gyml', 'IsSetUIColor': False,
                 'Alpha': team(0.8, 0.2, 0.8), 'Bravo': team(1.0, 0.6, 0.2), 'Charlie': team(0.0, 0.0, 1.0)})
    if extra:
        extra(rows)
    return rows


@unittest.skipUnless(CLANG, 'llvm-mingw is not in .toolchains')
class InkColorTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.dir.name) / 'ink_harness.exe'
        subprocess.run([str(CLANG[-1]), '-O1', '-I', str(ROOT / 'runtime' / 'core'), str(ROOT / 'tests' / 'ink_color_harness.c'),
                        str(ROOT / 'runtime' / 'sr' / 'ink_color.c'), '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.dir.cleanup()

    def run_patch(self, rows, player, enemy, share=True):
        """(floats set, the served table as Python values or None, the unpacked size before)"""
        src, out = Path(self.dir.name) / 'in.byml', Path(self.dir.name) / 'out.zs'
        raw = bytes(Byml(rows, share).out)
        src.write_bytes(raw)
        out.unlink(missing_ok=True)
        r = subprocess.run([str(self.exe), str(src), str(out), player or '-', enemy or '-'], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        if not out.exists():
            return int(r.stdout), None, len(raw)
        back = zstandard.ZstdDecompressor().decompress(out.read_bytes())   # a frame any zstd reads, its size stated
        self.assertEqual(len(back), len(raw))
        return int(r.stdout), parse(back), len(raw)

    def rgb(self, team_dict):
        c = team_dict['TeamColor']
        return tuple(round(c[k] * 255) for k in 'RGB')

    def test_the_writer_and_reader_agree(self):
        rows = table()
        self.assertEqual(parse(bytes(Byml(rows).out))[0]['__RowId'], rows[0]['__RowId'])
        self.assertAlmostEqual(parse(bytes(Byml(rows).out))[3]['Bravo']['TeamColor']['G'], 0.6, places=6)

    def test_the_players_colour_goes_to_alpha_of_the_hunt_sets_only(self):
        set_, rows, _ = self.run_patch(table(), 'e02020', None)
        self.assertEqual(set_, 9)
        for r in rows[:3]:
            self.assertEqual(self.rgb(r['Alpha']), (0xe0, 0x20, 0x20))
            self.assertEqual(self.rgb(r['Bravo']), (204, 153, 51))          # the enemies keep theirs
            self.assertAlmostEqual(r['Alpha']['InkRimIntensity'], 0.5)      # and nothing else moves
            self.assertAlmostEqual(r['Alpha']['TeamColor']['A'], 1.0)
        self.assertEqual(self.rgb(rows[3]['Alpha']), (204, 51, 204))        # a set that is not the player's

    def test_the_enemies_colour_goes_to_bravo_of_every_set(self):
        set_, rows, _ = self.run_patch(table(), None, '#2060FF')
        self.assertGreater(set_, 0)
        for r in rows:
            self.assertEqual(self.rgb(r['Bravo']), (0x20, 0x60, 0xff))
        self.assertEqual(self.rgb(rows[0]['Alpha']), (51, 51, 102))

    def test_both_colours_with_and_without_shared_containers(self):
        for share in (True, False):
            set_, rows, _ = self.run_patch(table(), 'ff8800', '2060ff', share)
            self.assertGreater(set_, 0)
            self.assertEqual([self.rgb(r['Alpha']) for r in rows[:3]], [(0xff, 0x88, 0x00)] * 3)
            self.assertEqual([self.rgb(r['Bravo']) for r in rows], [(0x20, 0x60, 0xff)] * 4)
            self.assertEqual(self.rgb(rows[0]['Charlie']), (0, 0, 255))

    def test_a_colour_shared_with_another_entry_stops_the_patch(self):
        def share_with_other(rows):   # another field holds the very colour of a set's Alpha: one container for both
            rows[0]['Alpha']['NightTeamColor'] = dict(rows[0]['Alpha']['TeamColor'])
        set_, rows, _ = self.run_patch(table(share_with_other), 'e02020', None)
        self.assertEqual((set_, rows), (0, None))

    def test_a_setting_that_is_no_colour_changes_nothing(self):
        for bad in ('red', '12345', '#12345g'):
            self.assertEqual(self.run_patch(table(), bad, None)[:2], (0, None))

    def test_a_table_of_another_shape_is_left_alone(self):
        self.assertEqual(self.run_patch([{'__RowId': 'Work/Gyml/HuntOne.x.gyml', 'Alpha': {'Other': 1.0}}], 'e02020', None)[:2],
                         (0, None))


if __name__ == '__main__':
    unittest.main()
