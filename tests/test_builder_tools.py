"""builder/tools.py with made-up archives served from file:// addresses: nothing is downloaded and no real tool is used."""
import hashlib
import io
import json
import tempfile
import unittest
import zipfile
from pathlib import Path

from builder import tools
from builder.runner import BuildError


def make_zip(entries):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, 'w') as z:
        for name, data in entries.items():
            z.writestr(name, data)
    return buf.getvalue()


class Quiet:
    def progress(self, *a, **k):
        pass

    def log(self, *a):
        pass


class ToolsTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.src, self.dl, self.tc = self.root / 'src', self.root / 'dl', self.root / 'tc'
        self.src.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def item(self, name, data, **more):
        (self.src / name).write_bytes(data)
        return dict(id=name.split('.')[0], version='1', needed=True, url=(self.src / name).as_uri(), file=name,
                    size=len(data), sha256=hashlib.sha256(data).hexdigest(), **more)

    def test_fetch_keeps_a_good_file_and_refuses_a_wrong_one(self):
        good = self.item('a.zip', make_zip({'top/x.txt': 'x'}))
        path = tools.fetch(good, self.dl)
        self.assertEqual(path.read_bytes(), (self.src / 'a.zip').read_bytes())
        (self.src / 'a.zip').unlink()          # the source is gone: the kept file must do
        self.assertEqual(tools.fetch(good, self.dl), path)
        bad = self.item('b.zip', b'not what the manifest says')
        bad['sha256'] = '0' * 64
        with self.assertRaises(BuildError) as e:
            tools.fetch(bad, self.dl)
        self.assertEqual(e.exception.id, 'tools.hash')
        self.assertEqual(sorted(p.name for p in self.dl.iterdir()), ['a.zip'])   # no b.zip, no .part

    def test_a_missing_source_is_a_download_error(self):
        gone = self.item('c.zip', b'x')
        (self.src / 'c.zip').unlink()
        with self.assertRaises(BuildError) as e:
            tools.fetch(gone, self.dl)
        self.assertEqual(e.exception.id, 'tools.download_failed')

    def test_unpack_strips_the_top_folder_and_leaves_no_staging(self):
        t = self.item('h.zip', make_zip({'Pkg-1/include/a.h': 'a', 'Pkg-1/README': 'r'}), kind='zip', to='headers',
                      strip='Pkg-1', probe='headers/include/a.h')
        tools.unpack(t, tools.fetch(t, self.dl), self.tc)
        self.assertTrue(tools.present(t, self.tc))
        self.assertEqual(sorted(p.name for p in self.tc.iterdir()), ['headers'])

    def test_unpack_into_the_toolchains_folder_itself(self):
        t = self.item('cc.zip', make_zip({'cc-1/bin/cc.exe': 'x'}), kind='zip', to='.', probe='cc-1/bin/cc.exe')
        tools.unpack(t, tools.fetch(t, self.dl), self.tc)
        self.assertTrue(tools.present(t, self.tc))

    def test_an_archive_that_writes_outside_is_refused(self):
        t = self.item('evil.zip', make_zip({'../outside.txt': 'x', 'ok.txt': 'y'}), kind='zip', to='evil', probe='evil/ok.txt')
        with self.assertRaises(BuildError) as e:
            tools.unpack(t, tools.fetch(t, self.dl), self.tc)
        self.assertEqual(e.exception.id, 'tools.unpack_failed')
        self.assertFalse((self.root / 'outside.txt').exists())
        self.assertFalse((self.tc / 'evil').exists())

    def test_a_single_file_tool(self):
        t = self.item('one.h', b'#pragma once\n', kind='file', to='one', probe='one/one.h')
        tools.unpack(t, tools.fetch(t, self.dl), self.tc)
        self.assertEqual((self.tc / 'one' / 'one.h').read_bytes(), b'#pragma once\n')

    PATCH = '''a note before the diff
--- a/src/a.txt
+++ b/src/a.txt
@@ -2,3 +2,4 @@
 two
-three
+THREE
+three and a half
 four
--- /dev/null
+++ b/src/new.txt
@@ -0,0 +1,2 @@
+first
+second
'''

    def test_a_patch_changes_a_file_and_adds_one(self):
        (self.root / 'src').mkdir(exist_ok=True)
        (self.root / 'src' / 'a.txt').write_bytes(b'one\ntwo\nthree\nfour\nfive\n')
        written = tools.apply_patch(self.root, self.PATCH)
        self.assertEqual(written, ['src/a.txt', 'src/new.txt'])
        self.assertEqual((self.root / 'src' / 'a.txt').read_bytes(), b'one\ntwo\nTHREE\nthree and a half\nfour\nfive\n')
        self.assertEqual((self.root / 'src' / 'new.txt').read_bytes(), b'first\nsecond\n')

    def test_a_patch_finds_its_lines_a_little_off_and_keeps_line_ends(self):
        (self.root / 'src').mkdir(exist_ok=True)
        (self.root / 'src' / 'a.txt').write_bytes(b'zero\r\none\r\ntwo\r\nthree\r\nfour\r\n')   # one line more above
        tools.apply_patch(self.root, self.PATCH)
        self.assertEqual((self.root / 'src' / 'a.txt').read_bytes(), b'zero\r\none\r\ntwo\r\nTHREE\r\nthree and a half\r\nfour\r\n')

    def test_a_patch_whose_lines_are_not_there_writes_nothing(self):
        (self.root / 'src').mkdir(exist_ok=True)
        (self.root / 'src' / 'a.txt').write_bytes(b'one\ntwo\nsomething else\nfour\n')
        with self.assertRaises(BuildError) as e:
            tools.apply_patch(self.root, self.PATCH)
        self.assertEqual(e.exception.id, 'tools.patch_failed')
        self.assertEqual((self.root / 'src' / 'a.txt').read_bytes(), b'one\ntwo\nsomething else\nfour\n')
        self.assertFalse((self.root / 'src' / 'new.txt').exists())

    def vendor_item(self, files, tree=None):
        data = make_zip({'repo-abc/' + k: v for k, v in files.items()})
        (self.src / 'v.zip').write_bytes(data)
        check = self.root / 'check'
        for k, v in files.items():
            (check / k).parent.mkdir(parents=True, exist_ok=True)
            (check / k).write_text(v, encoding='utf-8', newline='')
        return dict(id='lib', commit='abc', url=(self.src / 'v.zip').as_uri(), file='v.zip', size=len(data),
                    sha256=hashlib.sha256(data).hexdigest(), tree=tree or tools.tree_hash(check), probe='lib/src/a.txt',
                    patches=[{'file': 'fix.patch', 'at': '.'}])

    def test_a_vendor_clone_is_checked_by_its_files_then_patched(self):
        v = self.vendor_item({'src/a.txt': 'one\ntwo\nthree\nfour\nfive\n', 'README': 'r\n'})
        repo = self.root / 'repo'
        repo.mkdir()
        (repo / 'fix.patch').write_text(self.PATCH, encoding='utf-8')
        tools.fetch_vendor(v, self.dl, self.root / 'vendor', repo)
        lib = self.root / 'vendor' / 'lib'
        self.assertEqual((lib / 'src' / 'a.txt').read_bytes(), b'one\ntwo\nTHREE\nthree and a half\nfour\nfive\n')
        self.assertTrue((lib / 'src' / 'new.txt').is_file())
        self.assertEqual(sorted(p.name for p in (self.root / 'vendor').iterdir()), ['lib'])

    def test_a_vendor_clone_with_other_files_is_refused(self):
        v = self.vendor_item({'src/a.txt': 'tampered\n'}, tree='0' * 64)
        with self.assertRaises(BuildError) as e:
            tools.fetch_vendor(v, self.dl, self.root / 'vendor', self.root)
        self.assertEqual(e.exception.id, 'tools.hash')
        self.assertFalse((self.root / 'vendor' / 'lib').exists())
        self.assertFalse((self.dl / 'v.zip').exists())

    def test_the_package_python_is_checked_unpacked_and_pointed_at_the_app(self):
        from unittest import mock
        from builder import package
        py = self.item('python-embed.zip', make_zip({'python.exe': 'exe', 'python312._pth': 'python312.zip\n.\n', 'python312.zip': 'z'}))
        wheel = self.item('pkg-1-py3-none-any.whl', make_zip({'pkg/__init__.py': 'x = 1\n', 'pkg-1.dist-info/METADATA': 'm'}))
        out = self.root / 'pack'
        out.mkdir()
        with mock.patch.object(tools, 'load', return_value={'python': py, 'wheels': {'files': [wheel]}}):
            self.assertEqual(package.make_python(out, self.dl), 1)
        self.assertTrue((out / 'python' / 'python.exe').is_file())
        self.assertTrue((out / 'python' / 'Lib' / 'site-packages' / 'pkg' / '__init__.py').is_file())
        pth = (out / 'python' / 'python312._pth').read_text(encoding='ascii').split()
        self.assertEqual(pth, ['python312.zip', '.', 'Lib\\site-packages', '..\\app', 'import', 'site'])
        self.assertEqual(sorted(p.name for p in out.iterdir()), ['python'])   # no staging folder left
        wheel['sha256'] = '0' * 64                                            # a wheel that is not the pinned one
        (self.dl / wheel['file']).unlink()
        with mock.patch.object(tools, 'load', return_value={'python': py, 'wheels': {'files': [wheel]}}):
            with self.assertRaises(BuildError) as e:
                package.make_python(out, self.dl)
        self.assertEqual(e.exception.id, 'tools.hash')

    def test_the_package_leaves_the_window_sources_out_but_keeps_the_font_licences(self):
        from builder import package
        names = ['LICENSE', 'builder/cli.py', 'builder/window/MainWindow.xaml', 'builder/window/fonts/BowlbyOne-Regular.ttf',
                 'builder/window/fonts/OFL-BowlbyOne.txt', 'builder/window/fonts/OFL-BarlowCondensed.txt', 'runtime/core/main.c']
        self.assertEqual(package.wanted(names), ['LICENSE', 'builder/cli.py', 'builder/window/fonts/OFL-BowlbyOne.txt',
                                                 'builder/window/fonts/OFL-BarlowCondensed.txt', 'runtime/core/main.c'])
        self.assertIn('LICENSE', package.PARTS)
        self.assertTrue((tools.MANIFEST.parent.parent / 'LICENSE').is_file())

    def test_the_manifest_is_complete(self):
        m = json.loads(tools.MANIFEST.read_text(encoding='utf-8'))
        for t in m['tools']:
            for key in ('id', 'version', 'needed', 'url', 'file', 'size', 'sha256', 'kind', 'to', 'probe', 'licence'):
                self.assertIn(key, t, t.get('id'))
            self.assertTrue(t['url'].startswith('https://'), t['id'])
            self.assertEqual(len(t['sha256']), 64, t['id'])
        repo = tools.MANIFEST.parent.parent
        for v in m['vendor']['repos']:
            for key in ('id', 'repo', 'commit', 'url', 'file', 'size', 'sha256', 'tree', 'probe', 'licence'):
                self.assertIn(key, v, v.get('id'))
            self.assertIn(v['commit'], v['url'], v['id'])
            for p in v.get('patches', []):
                self.assertTrue((repo / p['file']).is_file(), p['file'])
        for w in [m['python']] + m['wheels']['files']:
            for key in ('url', 'file', 'size', 'sha256'):
                self.assertIn(key, w, w.get('file'))
            self.assertTrue(w['url'].startswith('https://'), w['file'])


if __name__ == '__main__':
    unittest.main()
