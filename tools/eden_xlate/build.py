"""Builds eden-xlate (tools/eden_xlate/main.cpp, translator study) against PokeDinner's shader recompiler.

Sources come from the vendor folder, outside the repository: <vendor>/pokedinner (a shallow clone of
https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork) and <vendor>/pokedinner-deps (sirit v1.0.5, fmt 12.1.0,
SPIRV-Headers vulkan-sdk-1.4.341.0 and the boost libraries the recompiler includes, at boost-1.91.0). Writes
<out>/eden-xlate.exe and <out>/eden-xlate.id (the sha256 the runtime keys its cache with: these sources and the clone's
commit), objects under <obj>. Our fixes to those sources are patches/*.patch, applied to the clone by this script.

    python tools/eden_xlate/build.py --vendor ../vendor --clang <llvm-mingw>/bin/clang++.exe --out ../out/build-vk/eden
"""
import argparse, concurrent.futures as cf, glob, hashlib, os, subprocess, sys

ap = argparse.ArgumentParser()
ap.add_argument('--vendor', required=True)
ap.add_argument('--clang', required=True, help='clang++ of llvm-mingw')
ap.add_argument('--out', required=True)
ap.add_argument('--obj', help='object folder (default <out>/obj)')
a = ap.parse_args()

here = os.path.dirname(os.path.abspath(__file__))
P = os.path.join(a.vendor, 'pokedinner', 'src')
D = os.path.join(a.vendor, 'pokedinner-deps')
obj = a.obj or os.path.join(a.out, 'obj')
os.makedirs(obj, exist_ok=True)
os.makedirs(a.out, exist_ok=True)
# our fixes to PokeDinner's sources (patches/*.patch), applied to the clone once; they are part of the identity
patches = sorted(glob.glob(os.path.join(here, 'patches', '*.patch')))
clone = os.path.join(a.vendor, 'pokedinner')
for p in patches:
    if subprocess.run(['git', '-C', clone, 'apply', '--reverse', '--check', p], capture_output=True).returncode == 0:
        continue   # already applied
    r = subprocess.run(['git', '-C', clone, 'apply', p], capture_output=True, text=True)
    if r.returncode:
        sys.exit(f'cannot apply {os.path.basename(p)}:\n{r.stderr}')
    print('applied', os.path.basename(p))
inc = [P, D + '/sirit/include', D + '/sirit/src', D + '/SPIRV-Headers/include', D + '/fmt/include'] + glob.glob(D + '/boost/*/include')
flags = ['-std=c++20', '-O2', '-w', '-DFMT_HEADER_ONLY', '-DNOMINMAX', '-DWIN32_LEAN_AND_MEAN', '-include', 'cstdlib'] + ['-I' + i for i in inc]
srcs = sorted(glob.glob(P + '/shader_recompiler/**/*.cpp', recursive=True))
srcs += sorted(glob.glob(D + '/sirit/src/**/*.cpp', recursive=True))
srcs += [P + '/common/settings.cpp', P + '/common/settings_common.cpp']
srcs += [os.path.join(here, 'main.cpp'), os.path.join(here, 'stubs.cpp')]
# the program, PokeDinner's GLSL and GLASM back ends are not needed: they still compile (the recompiler's own sources)


def obj_of(s):
    return os.path.join(obj, os.path.basename(s) + '.' + hashlib.sha1(os.path.abspath(s).encode()).hexdigest()[:8] + '.o')


def compile_one(s):
    o = obj_of(s)
    if os.path.exists(o) and os.path.getmtime(o) > os.path.getmtime(s):
        return s, 0, ''
    r = subprocess.run([a.clang] + flags + ['-c', s, '-o', o], capture_output=True, text=True)
    return s, r.returncode, r.stderr


bad = 0
with cf.ThreadPoolExecutor(os.cpu_count()) as ex:
    for s, rc, err in ex.map(compile_one, srcs):
        if rc:
            bad += 1
            print('FAIL', s)
            print('\n'.join(l for l in err.splitlines() if 'error' in l)[:2000])
if bad:
    sys.exit(f'{bad} of {len(srcs)} sources failed')
exe = os.path.join(a.out, 'eden-xlate.exe')
r = subprocess.run([a.clang, '-static', '-O2', '-o', exe] + [obj_of(s) for s in srcs], capture_output=True, text=True)
if r.returncode:
    sys.exit('link failed:\n' + r.stderr[:4000])

h = hashlib.sha256(b'eden-xlate 1\n')
for f in ('main.cpp', 'stubs.cpp', 'build.py'):
    h.update(open(os.path.join(here, f), 'rb').read())
for p in patches:
    h.update(open(p, 'rb').read())
head = subprocess.run(['git', '-C', os.path.join(a.vendor, 'pokedinner'), 'rev-parse', 'HEAD'], capture_output=True, text=True)
h.update(head.stdout.strip().encode())
open(os.path.join(a.out, 'eden-xlate.id'), 'w').write(h.hexdigest() + '\n')
print(f'built {exe} ({os.path.getsize(exe) // 1024} KiB), identity {h.hexdigest()[:16]} (pokedinner {head.stdout.strip()[:10]})')
