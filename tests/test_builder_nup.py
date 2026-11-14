"""The builder's NUP input with a made-up NUP (the layout srrecomp/sources.py Nup reads): no game file is used."""
import hashlib
import struct
import tempfile
import types
import unittest
from pathlib import Path

import zstandard

from builder import steps
from builder.runner import BuildError

REPO = Path(__file__).resolve().parent.parent


def make_nup(path, packages):
    """packages: [(title_id, version, kind 0 base / 1 update, display text, {exefs name: bytes}, {romfs path: (bytes,
    codec)})]. One flat directory per ExeFS and RomFS, plus a root: enough for the reader."""
    pool, dirs, files, children, blob, pkgs = bytearray(), [], [], [], bytearray(), []

    def name(s):
        off = len(pool)
        pool.extend(s.encode())
        return off, len(s.encode())

    def add_dir(entries):          # entries: [(name, data, codec)]; returns the directory's index
        recs = []
        for n, data, codec in sorted(entries, key=lambda e: e[0].encode()):
            stored = zstandard.ZstdCompressor().compress(data) if codec == 1 else data
            recs.append((name(n), len(data), len(blob), len(stored), hashlib.sha256(data).digest(), codec))
            blob.extend(stored)
        first = len(children)
        base = len(files)
        files.extend(recs)
        children.extend(('file', base + k) for k in range(len(recs)))
        dirs.append((name(f'd{len(dirs)}'), first, len(recs)))
        return len(dirs) - 1

    meta = bytearray()
    for tid, ver, kind, text, exefs, romfs in packages:
        e = add_dir([(n, d, 0) for n, d in exefs.items()])
        r = add_dir([(n, d, c) for n, (d, c) in romfs.items()])
        tlv = struct.pack('<HHI', 4, 0, len(text.encode())) + text.encode()
        pkgs.append((tid, 0, e, r, ver, kind, len(meta), len(tlv)))
        meta.extend(tlv)
    blob_off = 0x80
    meta_off = blob_off + len(blob)
    pkg_off = 0x30
    dir_off = pkg_off + len(pkgs) * 0x40
    file_off = dir_off + len(dirs) * 0x18
    child_off = file_off + len(files) * 0x50
    sp_off = child_off + len(children) * 4
    toc = bytearray(sp_off + len(pool))
    toc[:4] = b'TOC\x01'
    struct.pack_into('<IQQQQ', toc, 4, len(pkgs), len(dirs), len(files), sp_off, len(pool))
    for i, (tid, root, e, r, ver, kind, mo, ms) in enumerate(pkgs):
        struct.pack_into('<QIIIIBB', toc, pkg_off + i * 0x40, tid, root, e, r, ver, kind, 0)
        struct.pack_into('<QI', toc, pkg_off + i * 0x40 + 0x20, meta_off + mo, ms)
    for i, ((no, nl), first, count) in enumerate(dirs):
        struct.pack_into('<IIII', toc, dir_off + i * 0x18, no, nl, first, count)
    for i, ((no, nl), size, off, stored, sha, codec) in enumerate(files):
        struct.pack_into('<IIQQQ', toc, file_off + i * 0x50, no, nl, size, blob_off + off, stored)
        toc[file_off + i * 0x50 + 0x20:file_off + i * 0x50 + 0x40] = sha
        toc[file_off + i * 0x50 + 0x40] = codec
    for i, (kind, k) in enumerate(children):
        struct.pack_into('<I', toc, child_off + i * 4, len(dirs) + k)
    toc[sp_off:] = pool
    sb = bytearray(0x80)
    sb[:4] = b'NUP\x01'
    struct.pack_into('<QQ', sb, 0x08, meta_off + len(meta), len(toc))
    sb[0x18:0x38] = hashlib.sha256(toc).digest()
    struct.pack_into('<Q', sb, 0x38, blob_off)
    Path(path).write_bytes(bytes(sb) + bytes(blob) + bytes(meta) + bytes(toc))


class NupTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.lay = types.SimpleNamespace(repo=REPO)
        self.base = {'a/one.bin': (b'base one', 0), 'only-base.txt': (b'x', 0)}
        self.new = {'a/one.bin': (b'updated one' * 50, 1), 'b/two.bin': (b'two' * 7, 0), 'empty': (b'', 0)}

    def tearDown(self):
        self.tmp.cleanup()

    def nup(self, version=steps.VERSION, title=steps.TITLE_ID, name='game.nup'):
        path = self.root / name
        make_nup(path, [(title, 0, 0, '1.0.0', {'main': b'base main'}, self.base),
                        (title, version, 1, '1.1.0', {'main': b'new main'}, self.new)])
        return path

    def test_the_running_program_is_the_newest_update_and_must_be_the_version(self):
        got = steps.read_game(self.nup(), self.lay)
        self.assertEqual((got['kind'], got['version'], got['romfs_offset']), ('nup', '1.1.0', 0))
        self.assertEqual(got['romfs_size'], sum(steps._pad16(len(d)) for d, _ in self.new.values()))
        with self.assertRaises(BuildError) as e:
            steps.read_game(self.nup(version=steps.VERSION + 65536, name='newer.nup'), self.lay)
        self.assertEqual(e.exception.id, 'check.wrong_version')
        with self.assertRaises(BuildError) as e:
            steps.read_game(self.nup(title=0x0100000000001000, name='other.nup'), self.lay)
        self.assertEqual(e.exception.id, 'check.wrong_title')

    def test_a_file_that_is_no_nup_is_unreadable_not_a_crash(self):
        bad = self.root / 'bad.nup'
        bad.write_bytes(b'not a nup at all')
        with self.assertRaises(BuildError) as e:
            steps.read_game(bad, self.lay)
        self.assertEqual(e.exception.id, 'check.game_unreadable')
        with self.assertRaises(BuildError) as e:
            steps.read_game(self.root / 'game.xci', self.lay)
        self.assertEqual(e.exception.id, 'check.game_missing')

    def test_the_copy_holds_the_update_tree_with_its_own_index(self):
        out = self.root / 'romfs'
        seen = []
        size = steps.copy_nup_romfs(self.nup(), out, lambda d, t: seen.append((d, t)))
        image = (out / 'romfs.bin').read_bytes()
        index = (out / 'romfs-index.bin').read_bytes()
        self.assertEqual(len(image), size)
        self.assertEqual(seen[-1], (size, size))
        self.assertEqual(index[:8], b'SRRCRFS1')
        count, strings = struct.unpack_from('<QQ', index, 8)
        files = {}
        for i in range(count):
            off, n, so, sl = struct.unpack_from('<QQII', index, 24 + i * 24)
            files[index[strings + so:strings + so + sl - 1].decode()] = image[off:off + n]
            self.assertEqual(off % 16, 0)
        self.assertEqual(files, {p: d for p, (d, _) in self.new.items()})   # the update's files, decompressed; none of the base's
        self.assertEqual(sorted(p.name for p in out.iterdir()), ['romfs-index.bin', 'romfs.bin'])

    def test_a_damaged_file_inside_stops_the_copy(self):
        path = self.nup()
        raw = bytearray(path.read_bytes())
        at = raw.find(b'twotwotwo')
        raw[at] ^= 0xFF
        path.write_bytes(bytes(raw))
        with self.assertRaises(BuildError) as e:
            steps.copy_nup_romfs(path, self.root / 'romfs', lambda d, t: None)
        self.assertEqual(e.exception.id, 'romfs.bad_file')
        self.assertFalse((self.root / 'romfs' / 'romfs.bin').exists())


if __name__ == '__main__':
    unittest.main()
