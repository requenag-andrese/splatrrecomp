#!/usr/bin/env python3
"""Reads the slow-frame dumps of SR_PROFILE=<path> SR_PROFILE_SLOW=<ms> (runtime/core/prof_win.c): what the main, render
and replay threads were doing during each slow frame.

usage: prof_slow.py <SR_PROFILE path> <sr.exe> <llvm-nm> [--min-frame N] [--top 4]

Each line of a thread's report: share of the frame's samples, then the innermost function inside sr.exe <- the module
the thread was in when that is another one (a driver, the system). Prints every dump, then all dumps added up.
"""
import argparse
import bisect
import collections
import glob
import os
import re
import subprocess


def load_symbols(nm, exe):
    out = subprocess.run([os.path.abspath(nm), exe], capture_output=True, text=True, errors='replace').stdout
    syms = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] in 'tT':
            syms.setdefault(int(parts[0], 16), parts[2])
    addrs = sorted(syms)
    return addrs, [syms[a] for a in addrs], (addrs[0] & ~0xffffff) if addrs else 0x140000000


class Dump:
    def __init__(self, path):
        self.frame = self.ms = 0.0
        self.base = 0
        self.tids = {}
        self.modules = []
        self.samples = collections.defaultdict(list)
        self.stalls = []
        for line in open(path, encoding='utf-8', errors='replace'):
            p = line.split()
            if not p:
                continue
            if p[0] == 'base':
                self.base = int(p[1], 16)
            elif p[0] == 'frame':
                self.frame, self.ms = int(p[1]), float(p[3])
            elif p[0] == 'main':
                self.tids = {int(p[1]): 'main', int(p[3]): 'render', int(p[5]): 'replay'}
            elif p[0] == 'module':
                self.modules.append((int(p[1], 16), int(p[2], 16), os.path.basename(' '.join(p[3:]))))
            elif p[0] == 'tstack':
                self.samples[int(p[1])].append((float(p[2]), [int(x, 16) for x in p[3:]]))
            elif p[0] == 'tstall':
                self.stalls.append((int(p[1]), float(p[2]), float(p[3]), [int(x, 16) for x in p[4:]]))
        self.modules.sort()
        self.starts = [m[0] for m in self.modules]

    def module_of(self, rip):
        i = bisect.bisect_right(self.starts, rip) - 1
        if i >= 0 and rip < self.modules[i][0] + self.modules[i][1]:
            return self.modules[i][2]
        return '?'


def label(dump, chain, exe_name, addrs, names, image):
    """innermost sr.exe function, and the module the thread was in if not sr.exe"""
    if not chain:
        return '(no stack)'
    leaf = dump.module_of(chain[0])
    for rip in chain:
        if dump.module_of(rip) == exe_name:
            i = bisect.bisect_right(addrs, rip - dump.base + image) - 1
            fn = names[i] if i >= 0 else hex(rip)
            return fn if leaf == exe_name else f'{fn} <- {leaf}'
    return leaf


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('profile')
    ap.add_argument('exe')
    ap.add_argument('nm')
    ap.add_argument('--min-frame', type=int, default=0)
    ap.add_argument('--top', type=int, default=4)
    a = ap.parse_args()
    addrs, names, image = load_symbols(a.nm, a.exe)
    exe_name = os.path.basename(a.exe)
    total = {'main': collections.Counter(), 'render': collections.Counter(), 'replay': collections.Counter()}
    paths = sorted(glob.glob(a.profile + '.f*'), key=lambda s: int(s.rsplit('.f', 1)[1]))
    used = 0
    for path in paths:
        d = Dump(path)
        if d.frame < a.min_frame:
            continue
        used += 1
        print(f'frame {d.frame}: {d.ms:.1f} ms')
        for tid, role in d.tids.items():
            got = d.samples.get(tid, [])
            if not got:
                continue
            c = collections.Counter(label(d, s[1], exe_name, addrs, names, image) for s in got)
            total[role].update(c)
            print(f'  {role} ({len(got)} samples):')
            for what, n in c.most_common(a.top):
                print(f'    {100 * n / len(got):3.0f}% {what}')
        for tid, at, ms, chain in d.stalls:
            print(f'  stall: thread {d.tids.get(tid, tid)} at {at:.1f} ms took {ms:.1f} ms to stop in '
                  f'{label(d, chain, exe_name, addrs, names, image)}')
    print(f'\nall {used} dumps added up:')
    for role, c in total.items():
        n = sum(c.values())
        if not n:
            continue
        print(f'  {role} ({n} samples):')
        for what, k in c.most_common(a.top * 3):
            print(f'    {100 * k / n:3.0f}% {what}')


if __name__ == '__main__':
    main()
