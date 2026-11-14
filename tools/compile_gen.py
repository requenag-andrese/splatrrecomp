"""Compile every generated chunk (tools/gen_c.py output) to an object file, with the flags totk-pc-recomp's
tools/build_game.py uses for generated code. Compile only: nothing is linked.

usage: python tools/compile_gen.py --gen <dir> --vendor <dir> --clang <clang.exe> --obj <dir> --out <report.json>
                                   [--jobs N] [--modules main,subsdk0,sdk]

The report holds counts, timings and compiler messages with file names, line numbers and identifiers removed.
"""
import argparse
import collections
import concurrent.futures as cf
import json
import os
import re
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GEN_FLAGS = ['-O1', '-fno-strict-aliasing', '-Wno-return-type', '-Wno-unused-label', '-fvisibility=hidden',
             '-march=x86-64-v3']
MSG = re.compile(r'^(?:[A-Za-z]:)?[^:]*:\d+:\d+: (error|warning|fatal error): (.*)$')


def normalise(msg):
    msg = re.sub(r"'[^']*'", "'_'", msg)
    msg = re.sub(r'0x[0-9a-fA-F]+|\b\d+\b', '#', msg)
    return msg[:160]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--gen', required=True)
    ap.add_argument('--vendor', required=True)
    ap.add_argument('--clang', required=True)
    ap.add_argument('--obj', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--modules', default='main,subsdk0,sdk')
    a = ap.parse_args()
    for p in (a.obj, a.out):
        if os.path.commonpath([os.path.abspath(p), REPO]) == REPO:
            sys.exit('refusing to write build output inside the repository')
    clang = os.path.abspath(a.clang)
    inc = ['-I', os.path.abspath(os.path.join(a.vendor, 'totk-pc-recomp', 'runtime')), '-I', os.path.abspath(a.gen),
           '-include', os.path.join(REPO, 'runtime', 'srrecomp_ext.h')]

    srcs = []
    for m in a.modules.split(','):
        d = os.path.join(a.gen, m)
        srcs += [(m, os.path.join(d, f)) for f in sorted(os.listdir(d)) if f.endswith('.c')]
    srcs.sort(key=lambda s: -os.path.getsize(s[1]))  # biggest first
    os.makedirs(a.obj, exist_ok=True)

    def build(item):
        mod, src = item
        obj = os.path.join(a.obj, os.path.basename(src)[:-2] + '.o')
        t = time.time()
        r = subprocess.run([clang, *GEN_FLAGS, *inc, '-c', src, '-o', obj], capture_output=True, text=True,
                           errors='replace')
        msgs = [(m.group(1), normalise(m.group(2))) for m in map(MSG.match, r.stderr.splitlines()) if m]
        size = os.path.getsize(obj) if r.returncode == 0 and os.path.exists(obj) else 0
        return mod, r.returncode, time.time() - t, msgs, size, os.path.getsize(src)

    t0 = time.time()
    per = collections.defaultdict(lambda: collections.Counter())
    errors, warnings = collections.Counter(), collections.Counter()
    slowest = []
    done = 0
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for mod, rc, secs, msgs, osize, csize in ex.map(build, srcs):
            c = per[mod]
            c['chunks'] += 1
            c['ok' if rc == 0 else 'failed'] += 1
            c['obj_bytes'] += osize
            c['c_bytes'] += csize
            c['cpu_seconds'] += secs
            slowest.append(round(secs, 1))
            for kind, m in msgs:
                (errors if 'error' in kind else warnings)[m] += 1
            done += 1
            if done % 25 == 0 or done == len(srcs):
                print(f'compiled {done}/{len(srcs)} chunks, {time.time() - t0:.0f}s', file=sys.stderr, flush=True)
    report = {
        'tool': 'sr-recomp compile_gen',
        'compiler': subprocess.run([clang, '--version'], capture_output=True, text=True).stdout.splitlines()[0],
        'flags': GEN_FLAGS,
        'wall_seconds': round(time.time() - t0, 1),
        'modules': {m: {k: round(v, 1) if isinstance(v, float) else v for k, v in c.items()} for m, c in per.items()},
        'errors_top': dict(errors.most_common(30)),
        'warnings_top': dict(warnings.most_common(15)),
        'slowest_chunk_seconds': sorted(slowest, reverse=True)[:5],
    }
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=1)
    tot = collections.Counter()
    for c in per.values():
        tot.update(c)
    print(f'{tot["ok"]} of {tot["chunks"]} chunks compiled, {tot["failed"]} failed, '
          f'{tot["obj_bytes"] / 2**20:.0f} MB of objects, {report["wall_seconds"]}s; report: {a.out}', file=sys.stderr)


if __name__ == '__main__':
    main()
