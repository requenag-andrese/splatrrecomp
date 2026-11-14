"""tools/build_runtime.py: the linker's response file with a folder name that holds a space (the builder's temp folder
sits wherever the player put the package: "C:\\Users\\...\\Prueba Builder\\SRBuilder\\out\\builder-temp"), and the
version line of a package, which has no git."""
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
sys.path.insert(0, str(ROOT / 'tools'))

import build_runtime  # noqa: E402

CLANG = sorted((ROOT.parent / '.toolchains').glob('llvm-mingw-*/bin/clang.exe'))


class RspTest(unittest.TestCase):
    def test_each_object_is_quoted_with_forward_slashes(self):
        with tempfile.TemporaryDirectory() as t:
            rsp = os.path.join(t, 'link.rsp')
            build_runtime.write_rsp(rsp, [r'C:\Prueba Builder\obj\a.o', 'C:/x/b.o'])
            self.assertEqual(Path(rsp).read_text(), '"C:/Prueba Builder/obj/a.o"\n"C:/x/b.o"')

    @unittest.skipUnless(CLANG, 'llvm-mingw is not in .toolchains')
    def test_clang_links_objects_from_a_folder_with_a_space(self):
        clang = str(CLANG[-1])
        with tempfile.TemporaryDirectory() as t:
            d = Path(t) / 'Prueba Builder' / 'obj'
            d.mkdir(parents=True)
            (d / 'a.c').write_text('int f(void) { return 0; }\n')
            (d / 'b.c').write_text('int f(void);\nint main(void) { return f(); }\n')
            for n in 'ab':
                subprocess.run([clang, '-c', str(d / f'{n}.c'), '-o', str(d / f'{n}.o')], check=True)
            rsp, exe = d / 'link.rsp', d / 'x.exe'
            build_runtime.write_rsp(str(rsp), [str(d / 'a.o'), str(d / 'b.o')])
            r = subprocess.run([clang, f'@{rsp}', '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr)
            self.assertTrue(exe.is_file())


class PortVersionTest(unittest.TestCase):
    def version_of(self, release):
        with tempfile.TemporaryDirectory() as t:
            Path(t, 'RELEASE').write_text(release + '\n', encoding='ascii')
            return build_runtime.port_version(t)

    def test_a_package_takes_its_release_file(self):
        self.assertEqual(self.version_of('1dc4e2a5d1bc'), '1dc4e2a')
        self.assertEqual(self.version_of('1dc4e2a5d1bc+0630a931'), '1dc4e2a-dirty')
        self.assertEqual(self.version_of('unknown'), 'unknown')

    def test_the_repository_takes_git(self):
        v = build_runtime.port_version()
        self.assertRegex(v, r'^[0-9a-f]{7,}(-dirty)?$')


if __name__ == '__main__':
    unittest.main()
