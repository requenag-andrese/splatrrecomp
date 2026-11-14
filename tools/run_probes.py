"""Run the runtime's self-tests (sr.exe --selftest and every --probe-*) in a clean setting and say which pass.

usage: python tools/run_probes.py <build dir> [--census <dir>] [--amiibo <dump.bin>] [--only a,b] [--repeat N]
                                 [--logs <dir>]

Each probe runs as its own sr.exe process with:
  SR_CONFIG_FILE    a new, empty settings file: the defaults, whatever the player's sr-config.ini says, and nothing
                    of the player's gets written;
  SR_PROFILE_DIR    a new, empty profile folder: no save of the player's is read or touched;
  SR_PROBE_CENSUS   the shader census of tools/probe_census.py (default ../out/probe-census, when it is there);
  SR_PROBE_AMIIBO   --amiibo: a figure dump for --probe-amiibo, read where it is.
A probe passes on exit 0 with its result line saying passed / ok / PASS, is skipped when it says "skipped" (a feature
this port does not have, game data that is absent), and fails otherwise, or on a FATAL line, or after the timeout. The
two crash probes pass when the crash reporter catches the fault and names its frames. --repeat runs the whole set N
times, so a probe that passes only sometimes shows up. Exit 1 when anything failed.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

PROBES = ['selftest', 'probe-loader', 'probe-cfg', 'probe-dpi', 'probe-kernel', 'probe-ipc', 'probe-services',
          'probe-fs', 'probe-gfx-abi', 'probe-nvn', 'probe-nvn-tex', 'probe-window', 'probe-shader', 'probe-vkmem',
          'probe-vkexec', 'probe-astc', 'probe-vkdevice', 'probe-movie', 'probe-vp9', 'probe-amiibo', 'probe-mods', 'probe-discord',
          'probe-crash-call', 'probe-crash-null']
CRASH = {'probe-crash-call', 'probe-crash-null'}
TIMEOUT = {'probe-window': 180, 'probe-vkexec': 300, 'probe-shader': 300, 'probe-services': 180}
RESULT = re.compile(r'^(?:selftest|probe-[a-z0-9-]+|vk_pipeline selftest)\s*: *(.*)$', re.M)


def classify(name, rc, out):
    """('pass' | 'skip' | 'fail', one line saying why)."""
    if name in CRASH:
        ok = 'FATAL: access violation' in out and 'host frame  0:' in out
        return ('pass' if ok else 'fail'), ('the crash reporter caught it' if ok else f'exit {rc}, no crash report')
    fatal = re.search(r'^FATAL: .*$', out, re.M)
    lines = [m.group(0) for m in RESULT.finditer(out)]
    last = lines[-1] if lines else ''
    if fatal:
        return 'fail', fatal.group(0)[:160]
    if re.search(r': *skipped\b', last):
        return 'skip', last[:160]
    failed_line = re.search(r'FAILED|^\s*FAIL:', out, re.M)
    if rc == 0 and not failed_line and re.search(r'\b(passed|ok|PASS)\b', last):   # vkexec ends on its last part's line
        return 'pass', last[:160]
    return 'fail', (last or f'exit {rc}, no result line')[:160]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('build')
    here = os.path.dirname(os.path.abspath(__file__))
    ap.add_argument('--census', default=os.path.join(here, '..', '..', 'out', 'probe-census'))
    ap.add_argument('--only', help='comma list of probe names (without --)')
    ap.add_argument('--repeat', type=int, default=1)
    ap.add_argument('--amiibo', help='a 540-byte figure dump for --probe-amiibo (read in place; else it is skipped)')
    ap.add_argument('--logs', help='keep each probe\'s output here (default: a temporary folder, deleted)')
    a = ap.parse_args()
    exe = os.path.join(os.path.abspath(a.build), 'sr.exe')
    if not os.path.isfile(exe):
        sys.exit(f'no sr.exe in {a.build}')
    names = a.only.split(',') if a.only else PROBES
    work = tempfile.mkdtemp(prefix='sr-probes-')
    logs = os.path.abspath(a.logs) if a.logs else os.path.join(work, 'logs')
    os.makedirs(logs, exist_ok=True)
    results = {n: [] for n in names}
    try:
        for run in range(a.repeat):
            for name in names:
                profile = os.path.join(work, f'profile-{run}-{name}')
                os.makedirs(profile)
                ini = os.path.join(work, f'config-{run}-{name}.ini')
                open(ini, 'w').close()
                env = dict(os.environ, SR_CONFIG_FILE=ini, SR_PROFILE_DIR=profile)
                if a.amiibo:
                    env['SR_PROBE_AMIIBO'] = os.path.abspath(a.amiibo)
                if os.path.isfile(os.path.join(a.census, 'jobs.json')):
                    env['SR_PROBE_CENSUS'] = os.path.abspath(a.census)
                t0 = time.time()
                try:
                    r = subprocess.run([exe, '--' + name], cwd=os.path.dirname(exe), env=env, capture_output=True,
                                       text=True, errors='replace', timeout=TIMEOUT.get(name, 120))
                    rc, out = r.returncode, r.stdout + r.stderr
                except subprocess.TimeoutExpired as e:
                    rc, out = -1, ((e.stdout or b'').decode('utf-8', 'replace') if isinstance(e.stdout, bytes)
                                   else (e.stdout or '')) + '\nFATAL: timed out'
                with open(os.path.join(logs, f'{name}-{run}.log'), 'w', encoding='utf-8') as f:
                    f.write(out)
                state, why = classify(name, rc, out)
                results[name].append(state)
                print(f'{name:18} {state:4} {time.time() - t0:5.1f}s  {why}', flush=True)
    finally:
        if not a.logs:
            shutil.rmtree(work, ignore_errors=True)
        else:
            shutil.rmtree(os.path.join(work), ignore_errors=True)
    passed = sum(all(s == 'pass' for s in v) for v in results.values())
    skipped = sum(all(s == 'skip' for s in v) for v in results.values())
    flaky = [n for n, v in results.items() if len(set(v)) > 1]
    failed = [n for n, v in results.items() if 'fail' in v]
    print(f'\n{passed} passed, {skipped} skipped, {len(failed)} failed of {len(names)} probes'
          f'{f" x{a.repeat} runs" if a.repeat > 1 else ""}' + (f'; not the same every run: {", ".join(flaky)}' if flaky else ''))
    if failed:
        print('failed: ' + ', '.join(failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
