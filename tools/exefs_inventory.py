"""Inventory of an ExeFS: modules, SDK, symbols, SVCs and lifter coverage.

usage: python tools/exefs_inventory.py <exefs dir | plaintext .nsp | .nup> --vendor <dir> --out <report.json>

The report holds aggregates only (counts, ISA mnemonics, SDK/middleware identifiers, hashes). It never contains
game bytes, raw symbol lists from game modules, strings from game code, or assets. --out must lie outside this
repository.
"""
import argparse
import hashlib
import json
import os
import re
import sys
import time
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, REPO)

from srrecomp import lifters, npdm, sources, svc_names  # noqa: E402
from srrecomp.nso import Nso  # noqa: E402
from srrecomp.symbols import namespace  # noqa: E402

SDK_MODULES = {'rtld', 'sdk'}
MW = re.compile(rb'SDK MW\+([A-Za-z0-9_.\-]{1,64})\+([A-Za-z0-9_.\-]{1,96})')
SDK = re.compile(rb'NintendoSdk_nnSdk-((?:[A-Za-z]+_)?[0-9_]+)-(Release|Develop|Debug)')

LIFTERS = {
    'totk-c': ('totk-pc-recomp/recomp', 'c'),
    'totk-asm': ('totk-pc-recomp/recomp', 'asm'),
    'las-c': ('LASRecomp/tools', 'c'),
    'totk-c+ext': ('totk-pc-recomp/recomp', 'c'),
    'totk-asm+ext': ('totk-pc-recomp/recomp', 'asm'),
}


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def svc_scan(text):
    import numpy as np
    w = np.frombuffer(text[:len(text) // 4 * 4], dtype='<u4')
    hits = w[(w & 0xFFE0001F) == 0xD4000001]
    return Counter(((hits >> 5) & 0xFFFF).tolist())


def module_report(name, data, lifter_set, dec):
    nso = Nso(data)
    r = {
        'file_size': len(data),
        'sha256': hashlib.sha256(data).hexdigest(),
        'nso_version': nso.version,
        'module_id': nso.module_id.rstrip(b'\0').hex(),
        'segments': {'text': nso.text[1], 'rodata': nso.ro[1], 'data': nso.data[1], 'bss': nso.bss},
        'segment_compressed': nso.compressed,
        'segment_hash_ok': nso.hash_ok,
        'has_mod0': nso.mod0 is not None,
        'dt_needed': nso.needed,
        'functions_eh_frame': nso.eh_fde_count,
    }
    ro = nso.image[nso.ro[0]:nso.ro[0] + nso.ro[1]]
    r['sdk_versions'] = sorted({m.group(1).decode() + '-' + m.group(2).decode() for m in SDK.finditer(ro)})
    r['middleware'] = sorted({f'{a.decode()}+{b.decode()}' for a, b in MW.findall(ro)})

    syms = list(nso.symbols())
    defined = [s for s in syms if s[3]]
    imports = [s for s in syms if not s[3]]
    r['symbols'] = {
        'dynsym_total': len(syms),
        'defined': len(defined),
        'defined_functions': sum(1 for s in defined if s[2] == 2),
        'defined_objects': sum(1 for s in defined if s[2] == 1),
        'imports': len(imports),
        'weak': sum(1 for s in syms if s[1] == 2),
        'imports_by_namespace': dict(Counter(namespace(s[0]) for s in imports).most_common(60)),
    }
    if name in SDK_MODULES:
        r['symbols']['exports_by_namespace'] = dict(Counter(namespace(s[0]) for s in defined).most_common(80))

    text = nso.text_words()
    svcs = svc_scan(text)
    r['svc_sites'] = {f'{i:#04x} {svc_names.name(i)}': c for i, c in sorted(svcs.items())}

    uw, counts, total = lifters.unique_words(text)
    r['text_words'] = total
    r['text_unique_words'] = len(uw)

    # Only the words inside functions the unwind tables describe: excludes inter-function padding and data.
    import numpy as np
    words = np.frombuffer(text[:total * 4], dtype='<u4')
    mask = np.zeros(total, dtype=bool)
    ranges = nso.function_ranges()
    t0 = nso.text[0]
    for s, e in ranges:
        a, b = max(0, (s - t0) // 4), min(total, (e - t0 + 3) // 4)
        if a < b:
            mask[a:b] = True
    r['eh_functions'] = len(ranges)
    r['text_words_in_functions'] = int(mask.sum())
    fu, fc = np.unique(words[mask], return_counts=True)
    fu, fc = fu.tolist(), fc.tolist()

    r['lifter_coverage'] = {}
    r['lifter_coverage_in_functions'] = {}
    for key, (lifter, unsupported) in lifter_set.items():
        t = time.time()
        r['lifter_coverage'][key] = lifters.coverage(lifter, unsupported, dec, uw, counts)
        r['lifter_coverage_in_functions'][key] = lifters.coverage(lifter, unsupported, dec, fu, fc)
        log(f'  {name}: {key} sweep {r["lifter_coverage"][key]["covered_pct"]}%, in functions '
            f'{r["lifter_coverage_in_functions"][key]["covered_pct"]}% ({time.time() - t:.0f}s)')
    return r, svcs


def merge_coverage(mods, key, field='lifter_coverage'):
    tot = Counter()
    mn = Counter()
    uniq = Counter()
    for m in mods.values():
        c = m.get(field, {}).get(key)
        if not c:
            continue
        for f in ('decoded_instructions', 'covered', 'uncovered'):
            tot[f] += c[f]
        for e in c['uncovered_mnemonics']:
            mn[e['mnemonic']] += e['occurrences']
            uniq[e['mnemonic']] += e['unique_encodings']
    return {**tot, 'covered_pct': round(100 * tot['covered'] / tot['decoded_instructions'], 4)
            if tot['decoded_instructions'] else None,
            'uncovered_mnemonics_top': [{'mnemonic': k, 'occurrences': v, 'unique_encodings': uniq[k]}
                                        for k, v in mn.most_common(40)]}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input')
    ap.add_argument('--vendor', required=True, help='directory holding the pinned vendor clones')
    ap.add_argument('--out', required=True)
    ap.add_argument('--lifters', default=','.join(LIFTERS), help='comma list of ' + ','.join(LIFTERS))
    ap.add_argument('--nup-package', choices=('base', 'update'),
                    help='NUP package to read (default: the newest update, else the base)')
    a = ap.parse_args()

    out = os.path.abspath(a.out)
    if os.path.commonpath([out, REPO]) == REPO:
        sys.exit('refusing to write a report inside the repository; pass --out outside it')

    files, meta = sources.open_exefs(a.input, a.nup_package)
    lifter_set = {}
    for k in filter(None, a.lifters.split(',')):
        d, simd = LIFTERS[k]
        lifter_set[k] = lifters.load_lifter(os.path.join(a.vendor, d), simd, ext=k.endswith('+ext'))
    dec = lifters.Decoder()

    report = {'tool': 'sr-recomp exefs_inventory', 'container': meta, 'exefs_files': {}, 'modules': {}}
    for name, sl in files.items():
        report['exefs_files'][name] = sl.size
    if 'main.npdm' in files:
        report['npdm'] = npdm.parse(files['main.npdm'].read())
        report['npdm']['sha256'] = hashlib.sha256(files['main.npdm'].read()).hexdigest()

    all_svcs = Counter()
    order = sorted((n for n in files if n != 'main.npdm'), key=lambda n: (n != 'rtld', n != 'main', n))
    for name in order:
        data = files[name].read()
        if data[:4] != b'NSO0':
            report['modules'][name] = {'error': 'not NSO0', 'magic_hex': data[:4].hex()}
            continue
        log(f'module {name}: {len(data)} bytes')
        rep, svcs = module_report(name, data, lifter_set, dec)
        report['modules'][name] = rep
        all_svcs.update(svcs)

    report['summary'] = {
        'modules': list(report['modules']),
        'sdk_versions': sorted({v for m in report['modules'].values() for v in m.get('sdk_versions', [])}),
        'svc_ids_used': {f'{i:#04x} {svc_names.name(i)}': c for i, c in sorted(all_svcs.items())},
        'svc_ids_unknown': [f'{i:#04x}' for i in sorted(all_svcs) if i not in svc_names.SVC],
        'lifter_coverage': {k: merge_coverage(report['modules'], k) for k in lifter_set},
        'lifter_coverage_in_functions': {k: merge_coverage(report['modules'], k, 'lifter_coverage_in_functions')
                                         for k in lifter_set},
    }
    allowed = set(report.get('npdm', {}).get('allowed_svcs', []))
    if allowed:
        report['summary']['svc_used_not_allowed_by_npdm'] = [f'{i:#04x}' for i in sorted(all_svcs) if i not in allowed]

    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=1)
    s = report['summary']
    log(f'modules: {", ".join(s["modules"])}')
    log(f'sdk: {", ".join(s["sdk_versions"]) or "?"}')
    log(f'svc ids used: {len(s["svc_ids_used"])} (unknown to table: {len(s["svc_ids_unknown"])})')
    for k, c in s['lifter_coverage'].items():
        f = s['lifter_coverage_in_functions'][k]
        log(f'lifter {k}: sweep {c["covered_pct"]}% ({c["uncovered"]} uncovered of {c["decoded_instructions"]}); '
            f'in functions {f["covered_pct"]}% ({f["uncovered"]} uncovered of {f["decoded_instructions"]})')
    log(f'report: {out}')


if __name__ == '__main__':
    main()
