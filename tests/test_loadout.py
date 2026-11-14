"""runtime/sr/byml.c (tables read and written again) and loadout.c (a loadout written into the raid table). The tables
are made up, in the shape of the game's. Needs llvm-mingw in .toolchains (skipped without)."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))

from byml_tables import Byml, HashMap, U, murmur3, parse, sorted_keys  # noqa: E402

CLANG = sorted((ROOT.parent / '.toolchains').glob('llvm-mingw-*/bin/clang.exe'))
NONE = U(murmur3('Invalid'))


def part(name):
    return f'Work/Gyml/GadgetParts/{name}.spl__GadgetPartsInfo.gyml'


def gadget(kind=None, parts=()):
    return {'Type': U(murmur3(kind)) if kind else NONE, 'Parts': [part(p) for p in parts]}


def raid(name, weapon=None, level=1, a=None, left=None, right=None, style='Agility', kind='Normal'):
    return {'__RowId': f'Work/Gyml/{name}.spl__HuntDungeonInfo.gyml', 'DungeonType': U(murmur3(kind)), 'Floor': 3,
            'IsInfiniteRespawn': False, 'Lv': HashMap({1: murmur3('Easy'), 20: murmur3('Hard')}),
            'ChallengeWeapon': {'Type': U(murmur3(weapon)) if weapon else NONE, 'Lv': level, 'Affix': [NONE, NONE, NONE],
                                'AffixLv': [1, 1, 1]},
            'ChallengeGadgetA': a or gadget(), 'ChallengeGadgetL': left or gadget(), 'ChallengeGadgetR': right or gadget(),
            'ChallengeStyle': U(murmur3(style)), 'ChallengePlayerLevel': -1,
            'ClearReward': [{'Type': U(murmur3('Coin')), 'Num': 100}]}


def table():
    """Two ordinary raids, a boss raid whose row has a weapon of its own (the game has one such) and a challenge that
    fixes its loadout, with parts enough that a loadout written over it leaves the table no larger (as in the game's
    table, where the challenges' own lists go when all raids are written)."""
    own = [gadget('Bomb', [f'Bomb_{i:02}_SomeEffectOfTheGames_{k:02}' for i in range(1, 7)]) for k in (1, 2, 3)]
    return [raid('Raid_A'), raid('Raid_B'), raid('Boss_C', weapon='Brush_X', kind='Boss'),
            raid('C_Test', weapon='Brush_X', level=12, a=own[0], left=own[1], right=own[2], style='Strength',
                 kind='ChallengeEx')]


@unittest.skipUnless(CLANG, 'llvm-mingw is not in .toolchains')
class LoadoutTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.dir.name) / 'loadout_harness.exe'
        sr = ROOT / 'runtime' / 'sr'
        subprocess.run([str(CLANG[-1]), '-O1', '-I', str(sr), '-I', str(ROOT / 'runtime' / 'core'),
                        str(ROOT / 'tests' / 'loadout_harness.c'), str(sr / 'byml.c'), str(sr / 'loadout.c'), '-o', str(cls.exe)],
                       check=True)

    @classmethod
    def tearDownClass(cls):
        cls.dir.cleanup()

    def run_harness(self, mode, rows, *settings, share=True):
        """(the harness's answer, the table it wrote as Python values or None, its bytes, the input's size)"""
        src, out = Path(self.dir.name) / 'in.byml', Path(self.dir.name) / 'out.byml'
        raw = bytes(Byml(rows, share).out)
        src.write_bytes(raw)
        out.unlink(missing_ok=True)
        r = subprocess.run([str(self.exe), mode, str(src), str(out), *settings], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        data = out.read_bytes() if out.exists() else None
        return r.stdout.strip(), parse(data) if data else None, data, len(raw)

    def test_the_ids_are_murmur3_of_the_names(self):
        for name in ('Invalid', 'Shooter_A', 'Agility', 'a', 'ab', 'abc', 'abcd', ''):
            r = subprocess.run([str(self.exe), 'hash', name], capture_output=True, text=True)
            self.assertEqual(int(r.stdout), murmur3(name), name)

    def test_a_table_read_and_written_again_says_the_same(self):
        for share in (True, False):
            rows = table()
            answer, back, data, size = self.run_harness('roundtrip', rows, share=share)
            self.assertTrue(answer.startswith('built'))
            self.assertEqual(back, parse(bytes(Byml(rows, share).out)))
            self.assertTrue(sorted_keys(data))
            self.assertLessEqual(len(data), size)   # equal containers are written once, shared or not in the input

    def test_the_loadout_goes_into_every_raid(self):
        answer, rows, data, size = self.run_harness(
            'loadout', table(), 'SR_LOADOUT=1', 'SR_LOADOUT_WEAPON=Roller_Z', 'SR_LOADOUT_WEAPON_LV=50',
            'SR_LOADOUT_AFFIX=Fast:3, Strong:2', 'SR_LOADOUT_GADGET_A=Hook', 'SR_LOADOUT_PARTS_A=Hook_01_Up_16,Hook_02_Far_05',
            'SR_LOADOUT_GADGET_L=none', 'SR_LOADOUT_STYLE=Technique', 'SR_LOADOUT_PLAYER_LV=30')
        self.assertTrue(answer.startswith('built'), answer)
        self.assertTrue(sorted_keys(data))
        self.assertLessEqual(len(data), size)
        for r in rows:
            w = r['ChallengeWeapon']
            self.assertEqual((w['Type'], w['Lv']), (murmur3('Roller_Z'), 50))
            self.assertEqual(w['Affix'], [murmur3('Fast'), murmur3('Strong'), NONE])
            self.assertEqual(w['AffixLv'], [3, 2, 1])
            self.assertIsInstance(w['Type'], U)          # the value types stay the table's own
            self.assertNotIsInstance(w['Lv'], U)
            self.assertEqual(r['ChallengeGadgetA'], {'Type': murmur3('Hook'), 'Parts': [part('Hook_01_Up_16'), part('Hook_02_Far_05')]})
            self.assertEqual(r['ChallengeGadgetL'], {'Type': NONE, 'Parts': []})
            self.assertEqual((r['ChallengeStyle'], r['ChallengePlayerLevel']), (murmur3('Technique'), 30))
            self.assertEqual(r['ClearReward'], [{'Type': murmur3('Coin'), 'Num': 100}])     # the rest of the raid stays
            self.assertEqual(r['Lv'], HashMap({1: murmur3('Easy'), 20: murmur3('Hard')}))
        self.assertEqual(rows[3]['ChallengeGadgetR']['Type'], murmur3('Bomb'))   # a slot the settings do not name
        self.assertEqual(len(rows[3]['ChallengeGadgetR']['Parts']), 6)
        self.assertEqual([r['__RowId'] for r in rows], [r['__RowId'] for r in table()])

    def test_free_leaves_the_games_challenge_raids(self):
        """By the raid's type, which is what the game goes by: a raid of another type gets the loadout even when its
        row has a weapon of its own."""
        for kind in ('Challenge', 'ChallengeEx'):
            rows = table()
            rows[3]['DungeonType'] = U(murmur3(kind))
            answer, rows, _, _ = self.run_harness('loadout', rows, 'SR_LOADOUT=1', 'SR_LOADOUT_WEAPON=Roller_Z',
                                                  'SR_LOADOUT_RAIDS=free')
            self.assertTrue(answer.startswith('built'), answer)
            self.assertEqual([r['ChallengeWeapon']['Type'] for r in rows], [murmur3('Roller_Z')] * 3 + [murmur3('Brush_X')])
            self.assertEqual(rows[3]['ChallengeWeapon']['Lv'], 12)

    def test_a_hash_can_be_given_as_a_number(self):
        _, rows, _, _ = self.run_harness('loadout', table(), 'SR_LOADOUT=1', 'SR_LOADOUT_WEAPON=#12345678', 'SR_LOADOUT_AFFIX=#77:4')
        self.assertEqual(rows[0]['ChallengeWeapon']['Type'], 12345678)
        self.assertEqual((rows[0]['ChallengeWeapon']['Affix'][0], rows[0]['ChallengeWeapon']['AffixLv'][0]), (77, 4))

    def test_no_setting_no_table(self):
        self.assertEqual(self.run_harness('loadout', table(), 'SR_LOADOUT=1')[:2], ('none', None))
        self.assertEqual(self.run_harness('loadout', table(), 'SR_LOADOUT=1', 'SR_LOADOUT_PARTS_A=Hook_01_Up_16')[:2], ('none', None))

    def test_a_loadout_that_makes_the_table_larger_is_refused(self):
        rows = [raid('Raid_A')]   # nothing to share and nothing dropped: sixteen long part names cannot fit
        parts = ','.join(f'Hook_{i:02}_AVeryLongPartNameToFillTheTable_{i:02}' for i in range(16))
        answer, back, _, _ = self.run_harness('loadout', rows, 'SR_LOADOUT=1', 'SR_LOADOUT_GADGET_A=Hook', f'SR_LOADOUT_PARTS_A={parts}')
        self.assertEqual((answer, back), ('none', None))

    def test_bytes_that_are_no_table_are_left_alone(self):
        src, out = Path(self.dir.name) / 'bad.bin', Path(self.dir.name) / 'bad.out'
        for bad in (b'', b'YB', b'XX' + bytes(30), b'YB\x07\x00' + bytes([0xFF]) * 12):
            src.write_bytes(bad)
            r = subprocess.run([str(self.exe), 'roundtrip', str(src), str(out)], capture_output=True, text=True)
            self.assertEqual((r.returncode, r.stdout.strip()), (0, 'unreadable'), bad)


if __name__ == '__main__':
    unittest.main()
