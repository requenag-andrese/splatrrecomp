"""runtime/sr/loadout_live.c: which fields of the game's loadout object the settings own and what is written when the
game fills the object or a setting changes. The object is an array of fields here (tests/loadout_live_harness.c); the
part that finds it in the running game is checked in the game."""
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))

from byml_tables import murmur3  # noqa: E402

CLANG = sorted((ROOT.parent / '.toolchains').glob('llvm-mingw-*/bin/clang.exe'))

# The fields in loadout_live.c's order.
TANK, WEAPON, WEAPON_LV = 0, 1, 2
PARTS = 9


def weapon_power(j):
    return 3 + 2 * j        # type; the level follows


def relic_power(j):
    return 9 + 2 * j


def gadget(slot, what='type', k=0):
    return 19 + slot * (4 + PARTS) + {'type': 0, 'empty': 1, 'count': 2, 'extra': 3, 'part': 4 + k}[what]


NONE = 0xFFFFFFFF
# The player's own loadout as the game fills it: a Power Tank, weapon 1 at level 5 with one power, one relic power,
# nothing on L, gadget 1 with two parts on R, nothing on A, Showstopper 4.
OWN = {TANK: 2, WEAPON: 1, WEAPON_LV: 5, weapon_power(0): 1, weapon_power(0) + 1: 1, weapon_power(1): NONE,
       weapon_power(1) + 1: 1, weapon_power(2): NONE, weapon_power(2) + 1: 1, relic_power(0): 0, relic_power(0) + 1: 2,
       gadget(0): NONE, gadget(0, 'empty'): 1, gadget(1): 1, gadget(1, 'count'): 2, gadget(1, 'extra'): 2,
       gadget(1, 'part', 0): 111, gadget(1, 'part', 1): 222, gadget(2): NONE, gadget(2, 'empty'): 1, gadget(3): 4}
for _j in range(1, 5):
    OWN[relic_power(_j)], OWN[relic_power(_j) + 1] = NONE, 1


def fill(values):
    return 'set:' + ','.join(f'{i}={v}' for i, v in values.items())


@unittest.skipUnless(CLANG, 'llvm-mingw is not in .toolchains')
class LoadoutLiveTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.dir = tempfile.TemporaryDirectory()
        cls.exe = Path(cls.dir.name) / 'loadout_live_harness.exe'
        sr = ROOT / 'runtime' / 'sr'
        subprocess.run([str(CLANG[-1]), '-O1', '-I', str(sr), '-I', str(ROOT / 'runtime' / 'core'),
                        str(ROOT / 'tests' / 'loadout_live_harness.c'), str(sr / 'loadout_live.c'), str(sr / 'loadout.c'),
                        str(sr / 'byml.c'), '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.dir.cleanup()

    def run_harness(self, *args):
        """The harness's answers, one per look / offset / hash: 'none' or {field: value written}; and its log."""
        r = subprocess.run([str(self.exe), *args], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        out = []
        for line in r.stdout.split('\n'):
            if line.startswith('write'):
                out.append({int(p.split('=')[0]): int(p.split('=')[1]) for p in line[6:].split(',') if p})
            elif line:
                out.append(line)
        return out, r.stderr

    def test_the_fields_are_where_the_game_keeps_them(self):
        fields = {TANK: 0x10c, WEAPON: 0x7f8, WEAPON_LV: 0x7fc, weapon_power(0): 0x890, weapon_power(0) + 1: 0x894,
                  weapon_power(1): 0x910, weapon_power(2) + 1: 0x994, relic_power(0): 0x598, relic_power(1): 0x610,
                  relic_power(4) + 1: 0x77c, gadget(0): 0xa40, gadget(0, 'empty'): 0xa80, gadget(0, 'count'): 0xa58,
                  gadget(0, 'part', 0): 0xa5c, gadget(0, 'part', 1): 0xa60, gadget(0, 'extra'): 0xd48, gadget(1): 0xaf8,
                  gadget(1, 'count'): 0xb10, gadget(1, 'part', 0): 0xb14, gadget(1, 'extra'): 0xdf0, gadget(2): 0xbb0,
                  gadget(2, 'empty'): 0xbf0, gadget(3): 0xc68}
        out, _ = self.run_harness(*(f'offset:{i}' for i in fields))
        self.assertEqual([int(o) for o in out], list(fields.values()))
        offsets, _ = self.run_harness(*(f'offset:{i}' for i in range(gadget(3, 'part', PARTS - 1) + 1)))
        self.assertEqual(len(set(offsets)), len(offsets))               # no two fields share a place

    def test_nothing_is_written_without_the_switch_or_without_keys(self):
        self.assertEqual(self.run_harness(fill(OWN), 'look', 'look')[0], ['none', 'none'])
        self.assertEqual(self.run_harness('SR_LOADOUT_WEAPON=Roller_Z', fill(OWN), 'look')[0], ['none'])
        self.assertEqual(self.run_harness('SR_LOADOUT=1', fill(OWN), 'look')[0], ['none'])

    def test_the_settings_go_over_the_games_own(self):
        out, _ = self.run_harness(
            'SR_LOADOUT=1', 'SR_LOADOUT_WEAPON=Roller_Z', 'SR_LOADOUT_WEAPON_LV=50', 'SR_LOADOUT_TANK=Speed',
            'SR_LOADOUT_WEAPON_POWERS=Wpn_ToFrozen:3, none', 'SR_LOADOUT_POWERS=Cmn_MultiJump:3,Cmn_A',
            'SR_LOADOUT_GADGET_L=Boomerang', 'SR_LOADOUT_PARTS_L=Boom_11_Multi_02, Boom_01_SizeUpXZ_16',
            'SR_LOADOUT_GADGET_A=Hook', 'SR_LOADOUT_BUDDY=Frye', fill(OWN), 'look', 'look')
        self.assertEqual(out[1], 'none')
        self.assertEqual(out[0], {
            TANK: 1, WEAPON: 2, WEAPON_LV: 50, weapon_power(0): 2, weapon_power(0) + 1: 3,
            relic_power(0): 1, relic_power(0) + 1: 3, relic_power(1): 0,
            gadget(0): 1, gadget(0, 'empty'): 0, gadget(0, 'count'): 2, gadget(0, 'extra'): 2,
            gadget(0, 'part', 0): murmur3('Boom_11_Multi_02'), gadget(0, 'part', 1): murmur3('Boom_01_SizeUpXZ_16'),
            gadget(2): 2, gadget(2, 'empty'): 0, gadget(3): 3})

    def test_own_in_a_power_list_leaves_that_slot_as_the_game_has_it(self):
        out, _ = self.run_harness('SR_LOADOUT=1', 'SR_LOADOUT_POWERS=own, none, Cmn_A:2', 'SR_LOADOUT_WEAPON_POWERS=own,own,Cmn_A',
                                  fill(OWN), 'look')
        self.assertEqual(out, [{relic_power(2): 0, relic_power(2) + 1: 2, weapon_power(2): 0}])   # slot 1 was empty

    def test_the_tank_and_the_buddy_take_the_games_words_and_its_internal_ones(self):
        for name, tank in (('Default', 0), ('Speed', 1), ('Agility', 1), ('Power', 2), ('Tactical', 3), ('Technique', 3)):
            out, _ = self.run_harness('SR_LOADOUT=1', f'SR_LOADOUT_TANK={name}', fill({**OWN, TANK: 9}), 'look')
            self.assertEqual(out, [{TANK: tank}], name)
        for name, special in (('Shiver', 5), ('Fuka', 5), ('Frye', 3), ('BigMan', 4), ('Mantaro', 4)):
            out, _ = self.run_harness('SR_LOADOUT=1', f'SR_LOADOUT_BUDDY={name}', fill({**OWN, gadget(3): 9}), 'look')
            self.assertEqual(out, [{gadget(3): special}], name)

    def test_a_name_the_game_does_not_have_changes_nothing(self):
        out, log = self.run_harness('SR_LOADOUT=1', 'SR_LOADOUT_WEAPON=Sword_Q', 'SR_LOADOUT_TANK=Huge',
                                    'SR_LOADOUT_GADGET_L=Yoyo', 'SR_LOADOUT_BUDDY=Callie', 'SR_LOADOUT_POWERS=Fly:3',
                                    fill(OWN), 'look')
        self.assertEqual(out, ['none'])
        for word in ('Sword_Q', 'Huge', 'Yoyo', 'Callie', 'Fly'):
            self.assertIn(word, log)

    def test_another_gadget_drops_the_parts_and_the_same_gadget_keeps_them(self):
        out, _ = self.run_harness('SR_LOADOUT=1', 'SR_LOADOUT_GADGET_R=Hook', fill(OWN), 'look')
        self.assertEqual(out, [{gadget(1): 2, gadget(1, 'count'): 0, gadget(1, 'extra'): 0, gadget(1, 'part', 0): 0,
                                gadget(1, 'part', 1): 0}])
        self.assertEqual(self.run_harness('SR_LOADOUT=1', 'SR_LOADOUT_GADGET_R=Boomerang', fill(OWN), 'look')[0], ['none'])
        out, _ = self.run_harness('SR_LOADOUT=1', 'SR_LOADOUT_PARTS_R=#7', fill(OWN), 'look')     # parts for the own gadget
        self.assertEqual(out, [{gadget(1, 'count'): 1, gadget(1, 'extra'): 1, gadget(1, 'part', 0): 7, gadget(1, 'part', 1): 0}])
        out, _ = self.run_harness('SR_LOADOUT=1', 'SR_LOADOUT_GADGET_R=none', fill(OWN), 'look')
        self.assertEqual(out[0][gadget(1)], NONE)
        self.assertEqual(out[0][gadget(1, 'empty')], 1)

    def test_no_more_parts_than_a_gadget_holds(self):
        parts = ','.join(f'#{n}' for n in range(1, 14))
        out, _ = self.run_harness('SR_LOADOUT=1', f'SR_LOADOUT_PARTS_R={parts}', fill(OWN), 'look')
        self.assertEqual(out[0][gadget(1, 'count')], PARTS)
        self.assertEqual(out[0][gadget(1, 'part', PARTS - 1)], PARTS)

    def test_it_is_written_again_when_the_game_fills_the_object(self):
        """A raid starts: the game puts the player's own back. One look to see it, a second to be sure the fill is
        over, then the settings go over it again."""
        out, _ = self.run_harness('SR_LOADOUT=1', 'SR_LOADOUT_WEAPON=Roller_Z', fill(OWN), 'look', 'look',
                                  fill(OWN), 'look', 'look', 'look')
        self.assertEqual(out, [{WEAPON: 2}, 'none', 'none', {WEAPON: 2}, 'none'])

    def test_a_fill_caught_half_way_waits(self):
        out, _ = self.run_harness('SR_LOADOUT=1', 'SR_LOADOUT_WEAPON=Roller_Z', fill(OWN), 'look',
                                  fill({WEAPON: 0}), 'look', fill({WEAPON: 0, WEAPON_LV: 9}), 'look', 'look', 'look')
        self.assertEqual(out, [{WEAPON: 2}, 'none', 'none', {WEAPON: 2}, 'none'])

    def test_what_the_player_changes_in_the_game_stays_where_no_key_owns_it(self):
        out, _ = self.run_harness('SR_LOADOUT=1', 'SR_LOADOUT_WEAPON=Roller_Z', fill(OWN), 'look',
                                  fill({**OWN, TANK: 3}), 'look', 'look', fill({TANK: 9}), 'look')
        self.assertEqual(out[:3], [{WEAPON: 2}, 'none', {WEAPON: 2}])    # the tank the game chose is kept
        self.assertEqual(out[3], 'none')                                 # ... and so is the next one, after a look

    def test_a_setting_changed_while_playing_is_written_at_once(self):
        out, _ = self.run_harness('SR_LOADOUT=1', 'SR_LOADOUT_WEAPON=Roller_Z', fill(OWN), 'look',
                                  'SR_LOADOUT_WEAPON=Shooter_A', 'look', 'SR_LOADOUT_TANK=Tactical', 'look', 'look')
        self.assertEqual(out, [{WEAPON: 2}, {WEAPON: 0}, {TANK: 3}, 'none'])

    def test_switching_it_off_gives_the_players_own_back(self):
        out, _ = self.run_harness('SR_LOADOUT=1', 'SR_LOADOUT_WEAPON=Roller_Z', 'SR_LOADOUT_GADGET_R=Hook', fill(OWN),
                                  'look', 'SR_LOADOUT=0', 'look', 'look')
        self.assertEqual(out[1], {WEAPON: 1, gadget(1): 1, gadget(1, 'count'): 2, gadget(1, 'extra'): 2,
                                  gadget(1, 'part', 0): 111, gadget(1, 'part', 1): 222})
        self.assertEqual(out[2], 'none')

    def test_a_setting_changed_as_the_game_fills_the_object_keeps_the_games_new_values(self):
        out, _ = self.run_harness('SR_LOADOUT=1', 'SR_LOADOUT_WEAPON=Roller_Z', fill(OWN), 'look',
                                  fill({**OWN, TANK: 3, WEAPON_LV: 8}), 'SR_LOADOUT_WEAPON=Shooter_A', 'look', 'look')
        self.assertEqual(out[1:], ['none', {WEAPON: 0}])                 # tank 3 and level 8 are the game's and stay


if __name__ == '__main__':
    unittest.main()
