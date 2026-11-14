# Runs spirv-val (Vulkan 1.3, scalar block layout as eden-xlate's modules need) on every .spv of a folder and groups
# the errors.
#   python -I validate.py <folder>
import os, sys, subprocess, re, collections, concurrent.futures as cf
# spirv-val of the workspace's SPIRV-Tools (<workspace>/.toolchains/spirv-tools/install/bin), or SPIRV_VAL
VAL = os.environ.get('SPIRV_VAL') or os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', '..', '..',
                                                  '.toolchains', 'spirv-tools', 'install', 'bin', 'spirv-val.exe')
d = sys.argv[1]
files = sorted(f for f in os.listdir(d) if f.endswith('.spv'))

def one(f):
    r = subprocess.run([VAL, '--target-env', 'vulkan1.3', '--scalar-block-layout', os.path.join(d, f)], capture_output=True, text=True)
    return f, r.returncode, (r.stderr or r.stdout).strip().splitlines()[:1]

bad = collections.Counter()
ex = {}
n = 0
with cf.ThreadPoolExecutor(os.cpu_count()) as pool:
    for f, rc, msg in pool.map(one, files):
        n += 1
        if rc:
            key = re.sub(r'[0-9]+', '#', msg[0] if msg else '?')[:150]
            if not bad[key]: ex[key] = f
            bad[key] += 1
print(n, 'modules,', sum(bad.values()), 'invalid')
for k, c in bad.most_common(15):
    print(' %6d  %s  (e.g. %s)' % (c, k, ex[k]))
