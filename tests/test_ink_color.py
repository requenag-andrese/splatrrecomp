"""runtime/sr/ink_color.c: the ink colours written into a team colour table. The tables here are made up (the game's own
never enters the repository): the same shape, other names' values. Needs llvm-mingw in .toolchains (skipped without)."""
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path

import zstandard

ROOT = Path(__file__).resolve().parent.parent
CLANG = sorted((ROOT.parent / '.toolchains').glob('llvm-mingw-*/bin/clang.exe'))


class Byml:
    """A little-endian BYML writer: dictionaries, lists, floats, strings and booleans. Equal containers are written
    once and shared, as the game's tool does, unless share is off."""

    def __init__(self, root, share=True):
        self.keys, self.strs = sorted(self._names(root, set())), sorted(self._values(root, set()))
        self.share, self.done, self.out = share, {}, bytearray(16)
        key_at, str_at = self._table(self.keys), self._table(self.strs)
        root_at = self._node(root)
        self.out[0:16] = b'YB' + struct.pack('<HIII', 7, key_at, str_at, root_at)

    def _names(self, v, acc):
        for k, x in (v.items() if isinstance(v, dict) else enumerate(v) if isinstance(v, list) else ()):
            if isinstance(v, dict):
                acc.add(k)
            self._names(x, acc)
        return acc

    def _values(self, v, acc):
        if isinstance(v, str):
            acc.add(v)
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
        if isinstance(v, float):
            return 0xD2, struct.unpack('<I', struct.pack('<f', v))[0]
        if isinstance(v, str):
            return 0xA0, self.strs.index(v)
        return (0xC1 if isinstance(v, dict) else 0xC0), self._node(v)

    def _node(self, v):
        mark = repr(v)
        if self.share and mark in self.done:
            return self.done[mark]
        if isinstance(v, dict):
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


def parse(data):
    """The table back as Python values (the reader the writer above is checked with)."""
    keys_at, strs_at, root = struct.unpack_from('<III', data, 4)

    def names(at):
        n = int.from_bytes(data[at + 1:at + 4], 'little')
        offs = struct.unpack_from(f'<{n}I', data, at + 4)
        return [data[at + o:data.index(b'\0', at + o)].decode() for o in offs]

    keys, strs = names(keys_at), names(strs_at)

    def value(t, raw):
        if t == 0xD2:
            return struct.unpack('<f', struct.pack('<I', raw))[0]
        return {0xA0: lambda: strs[raw], 0xD0: lambda: bool(raw)}.get(t, lambda: node(raw))()

    def node(at):
        n = int.from_bytes(data[at + 1:at + 4], 'little')
        if data[at] == 0xC1:
            return {keys[int.from_bytes(data[at + 4 + 8 * i:at + 7 + 8 * i], 'little')]:
                    value(data[at + 7 + 8 * i], struct.unpack_from('<I', data, at + 8 + 8 * i)[0]) for i in range(n)}
        base = at + 4 + (n + 3 & ~3)
        return [value(data[at + 4 + i], struct.unpack_from('<I', data, base + 4 * i)[0]) for i in range(n)]

    return node(root)


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
