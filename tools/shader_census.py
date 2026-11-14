"""How many of a title's shaders the Ampere (sm_86) frontend of shirai91/nx2android PR #88 translates to SPIR-V.

usage: python tools/shader_census.py <plaintext .nsp> --vendor <dir> --xlate <dmsh_xlate.dll> --work <dir> --out <report.json>

1. Walk the RomFS straight from the NSP (nothing is extracted to disk), zstd-decompress each file (with the
   ZsDic dictionaries when the title ships them) and hand it to PR #88's shader_split.scan_blob, which finds
   BNSH / BAHS / NDSR / sead primitive shaders. ExeFS main is scanned as well.
2. Deduplicate by shader_split.skey (control key + code).
3. Build one translator job per unique shader the way PR #88 shader_vk.py job_for / nvn2_job does, without the
   per-game answer table (no_facts), and run dmsh_xlate built from the PR's source.

Labels passed to the PR's scanners are file indices, never paths, so its messages carry no game names. The
report has counts only. Shader code is written to --work (outside the repository) for the translator and
deleted afterwards unless --keep is given.
"""
import argparse
import atexit
import collections
import concurrent.futures as cf
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
sys.path.insert(0, REPO)

from srrecomp import sources, spirv_check, spirv_txq  # noqa: E402
from srrecomp.nso import Nso  # noqa: E402

ZSTD_MAGIC = bytes.fromhex('28b52ffd')
HEX = re.compile(r'(0x)?\b[0-9a-fA-F]{4,}\b')  # strips instruction words / addresses from translator messages
_ss = None
_ctx = {}


def log(msg):
    print(msg, file=sys.stderr, flush=True)


def load_shader_split(vendor):
    """PR #88's shader_split imports a game profile at import time; any profile satisfies it (only its source-group
    names, unused here, come from it)."""
    global _ss
    if _ss is None:
        os.environ.setdefault('DM_GAME', '040074A01BF12000')
        sys.path.insert(0, os.path.join(vendor, 'nx2android-pr88/nxnative/dm/gfx/tools'))
        import shader_split
        shader_split.sarc_members = sarc_members_linear
        _ss = shader_split
    return _ss


def sarc_members_linear(d):
    """Same output as PR #88 shader_split.sarc_members, which reads each member name with
    d[names + off:].split(b'\\0')[0]: a copy and split of the whole remaining buffer per member, quadratic on large
    layout archives (a 72 MB SARC with thousands of members never finished). Here the name ends at the next NUL."""
    hl = struct.unpack_from('<H', d, 4)[0]
    doff = struct.unpack_from('<I', d, 0x0C)[0]
    n = struct.unpack_from('<H', d, hl + 6)[0]
    names = hl + 12 + 16 * n + 8
    for i in range(n):
        h, attr, s, e = struct.unpack_from('<IIII', d, hl + 12 + 16 * i)
        if attr >> 24:
            a = names + (attr & 0xffffff) * 4
            z = d.find(b'\0', a)
            nm = d[a:z if z >= 0 else len(d)].decode('utf-8', 'replace')
        else:
            nm = f'#{i}'
        yield nm, d[doff + s:doff + e]


def _init(nsp, vendor, zdicts):
    ss = load_shader_split(vendor)
    ss.ZDICTS = zdicts
    _ctx['romfs'] = sources.open_romfs(nsp)[1]


def unzstd(ss, d):
    fhd = d[4]
    n, o = (0, 1, 2, 4)[fhd & 3], 6 - (fhd >> 5 & 1)
    did = int.from_bytes(d[o:o + n], 'little')
    z = ss.zstd_mod()
    if not did:
        return z.decompress(d)
    if did not in ss.ZDICTS:
        raise KeyError('zstd dictionary missing')
    if did not in ss._ZD:
        ss._ZD[did] = z.ZstdDict(ss.ZDICTS[did])
    return z.decompress(d, zstd_dict=ss._ZD[did])


def kind_of(label):
    for k in ('#BNSH', '#NDSR', '#SEADPRIM', '#BAHS'):
        if k in label:
            return k[1:]
    return 'other'


def scan_one(job):
    idx, top, off, size = job
    ss = _ss
    out = []
    err = None
    try:
        d = _ctx['romfs'].read(off, size)
        if d[:4] == ZSTD_MAGIC:
            d = unzstd(ss, d)
        ss.scan_blob(d, f'f{idx}', out)
    except SystemExit:
        err = 'scanner_rejected'
    except Exception as e:  # noqa: BLE001
        err = type(e).__name__
    found = {}
    for label, st, ctl, code in out:
        k = ss.skey(ctl, code)
        if k not in found:
            found[k] = (st, bool(ss.nvn2(ctl)), kind_of(label), top, ctl, code)
    return idx, len(out), found, err


def build_job(ss, key, st, ctl, code, work):
    """PR #88 android/tools/shader_vk.py job_for + nvn2_job (MIT), for a shader with no answer-table row."""
    j = {'key': key, 'stage': st, 'tex': {}, 'img': {}, 'attr': {}, 'fout': {}, 'debug': False, 'reduced': True,
         'sb_align': 256, 'subgroup': 128, 'gather_bias': 8, 'gs_passthrough': False, 'depth_runtime': True,
         'physical_gmem': True, 'no_facts': True}
    if ss.nvn2(ctl):
        n, c1a, c1e = struct.unpack_from('<I4xII', ctl, 0x38)
        c1 = code[c1a:c1e]
        code = code[:n]
        j['arch'] = 'sm86'
        if st == 'cs':
            j['nvn2_cs'] = [ctl[0xb1], ctl[0xb2], ctl[0xb3], struct.unpack_from('<H', ctl, 0xba)[0], ctl[0xa9] * 4]
    else:
        c1o, c1t = struct.unpack_from('<2I', ctl, 0x700)
        c1 = code[c1o:c1t]
        if st != 'cs':
            code = code[0x30:]
        elif code[:4] == bytes.fromhex('79563412'):
            code = code[0x100:]
    if st == 'cs':
        j['local'], j['shared'] = ss.cs_layout(ctl)
    p = os.path.join(work, key + '.bin')
    with open(p, 'wb') as f:
        f.write(code)
    j['code'] = p
    j['c1'] = c1.hex()
    return j


def run_xlate(dotnet, dll, jobs, work, i):
    jp = os.path.join(work, f'job_{i}.json')
    with open(jp, 'w') as f:
        json.dump(jobs, f)
    r = subprocess.run([dotnet, dll, jp, os.path.join(work, 'spv')], capture_output=True, text=True, errors='replace')
    return r.stderr


NOTE_SLOT = re.compile(r'texture handle ([0-9A-Fa-f]+) \(constant buffer (-?\d+), slot (\d+)\)')


def infer_txq_array(a, xjobs, spv_files, results, run_val):
    """Second pass for modules spirv-val rejected because a size query reads a component the (default 2D) texture
    type lacks: those textures are given the type '2darray' in the job and translated again. The type is inferred,
    not known: a 3D texture would also explain the query."""
    by_key = {j['key']: j for j in xjobs}
    retry = []
    for p, res in zip(spv_files, results):
        if not res or 'Vector access is out of bounds' not in res:
            continue
        key = os.path.basename(p)[:-4]
        with open(p, 'rb') as f:
            binds = spirv_txq.oob_query_bindings(f.read())
        with open(p[:-4] + '.res.json') as f:
            info = json.load(f)
        handle_slot = {int(h, 16): int(s) for h, _, s in (NOTE_SLOT.search(n).groups() for n in info.get('notes', [])
                                                          if NOTE_SLOT.search(n))}
        slots = {handle_slot[t['handle']] for t in info.get('textures', [])
                 if t['binding'] in binds and t['handle'] in handle_slot}
        if not slots:
            continue
        j = dict(by_key[key])
        j['tex'] = {**j.get('tex', {}), **{str(s): '2darray' for s in slots}}
        retry.append(j)
    if not retry:
        return {'attempted': 0}
    log(f'retranslating {len(retry)} shaders with inferred array textures ...')
    for p in [os.path.join(a.work, 'spv', j['key'] + '.spv') for j in retry]:
        os.remove(p)
    errs = run_xlate(a.dotnet, a.xlate, retry, a.work, 'txq')
    paths = [os.path.join(a.work, 'spv', j['key'] + '.spv') for j in retry]
    translated = [p for p in paths if os.path.exists(p)]
    after = [run_val(p) for p in translated]
    return {'attempted': len(retry), 'translated': len(translated),
            'now_valid': after.count(None), 'still_invalid': len(after) - after.count(None),
            'translator_errors': sum(1 for ln in errs.splitlines() if ln.startswith('[RED] ')),
            'type_used': '2darray (inferred from a size query reading component 2)'}


def scan_nsp(nsp, vendor, workers):
    """Every unique shader of the NSP's RomFS and ExeFS main, with PR #88's scanners: {key: (stage, nvn2, kind, top
    directory, control section, code area)} and the scan counters (instances, files_with, errors)."""
    ss = load_shader_split(vendor)
    entries, romfs = sources.open_romfs(nsp)   # plaintext NSP or NUP
    log(f'romfs: {len(entries)} files')
    zdicts = {}
    for path, off, size in entries:
        if path.endswith('ZsDic.pack.zs'):
            d = ss.zstd_mod().decompress(romfs.read(off, size))
            zdicts = {struct.unpack_from('<I', m, 4)[0]: m for _, m in ss.sarc_members(d) if m[:4] == bytes.fromhex('37a430ec')}
    log(f'zstd dictionaries: {len(zdicts)}')

    jobs = [(i, p.split('/', 1)[0] if '/' in p else '(root)', off, size) for i, (p, off, size) in enumerate(entries)]
    jobs.sort(key=lambda j: -j[3])  # big files first for better load balance
    uniq = {}
    instances = 0
    files_with = collections.Counter()
    errors = collections.Counter()
    t = time.time()
    with cf.ProcessPoolExecutor(workers, initializer=_init, initargs=(nsp, vendor, zdicts)) as ex:
        for n, (idx, cnt, found, err) in enumerate(ex.map(scan_one, jobs, chunksize=4)):
            instances += cnt
            if err:
                errors[err] += 1
            if found:
                files_with['files'] += 1
            for k, v in found.items():
                uniq.setdefault(k, v)
            if n % 5000 == 0:
                log(f'  scanned {n}/{len(jobs)} files, {len(uniq)} unique shaders ({time.time() - t:.0f}s)')
    # ExeFS main
    files, _ = sources.open_exefs(nsp)
    main_out = []
    try:
        ss.scan_blob(Nso(files['main'].read()).image, 'main', main_out)
    except SystemExit:
        errors['main_scanner_rejected'] += 1
    for label, st, ctl, code in main_out:
        uniq.setdefault(ss.skey(ctl, code), (st, bool(ss.nvn2(ctl)), kind_of(label), '(exefs main)', ctl, code))
    instances += len(main_out)
    log(f'scan done: {instances} shader stage instances, {len(uniq)} unique ({time.time() - t:.0f}s)')

    errors['_romfs_files'] = len(entries)
    errors['_zstd_dictionaries'] = len(zdicts)
    return ss, uniq, instances, files_with, errors


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('nsp')
    ap.add_argument('--vendor', required=True)
    ap.add_argument('--xlate', required=True, help='dmsh_xlate.dll built from PR #88 source')
    ap.add_argument('--dotnet', default='dotnet')
    ap.add_argument('--work', required=True)
    ap.add_argument('--out', required=True)
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 4) - 1))
    ap.add_argument('--keep', action='store_true', help='keep extracted code and SPIR-V in --work')
    ap.add_argument('--spirv-val', help='path to spirv-val (SPIRV-Tools); every translated module is validated')
    ap.add_argument('--target-env', default='vulkan1.2')
    ap.add_argument('--infer-txq-array', action='store_true',
                    help='with --spirv-val: retranslate textures whose size query reads a missing component as 2darray')
    a = ap.parse_args()
    # CreateProcess does not resolve relative executable paths with forward slashes
    a.xlate = os.path.abspath(a.xlate)
    if a.spirv_val:
        a.spirv_val = os.path.abspath(a.spirv_val)
    if os.path.sep in a.dotnet or '/' in a.dotnet:
        a.dotnet = os.path.abspath(a.dotnet)
    for p in (a.work, a.out):
        if os.path.commonpath([os.path.abspath(p), REPO]) == REPO:
            sys.exit('refusing to write game-derived data inside the repository')

    ss, uniq, instances, files_with, errors = scan_nsp(a.nsp, a.vendor, a.jobs)
    n_entries, n_zdicts = errors.pop('_romfs_files'), errors.pop('_zstd_dictionaries')
    os.makedirs(a.work, exist_ok=True)
    os.makedirs(os.path.join(a.work, 'spv'), exist_ok=True)
    if not a.keep:  # extracted shader code must not outlive the run, failed or not
        atexit.register(shutil.rmtree, a.work, ignore_errors=True)
    xjobs, bad_ctl = [], 0
    for k, (st, v2, kind, top, ctl, code) in uniq.items():
        try:
            xjobs.append(build_job(ss, k, st, ctl, code, a.work))
        except Exception:  # noqa: BLE001
            bad_ctl += 1
    chunks = [xjobs[i::a.jobs] for i in range(a.jobs) if xjobs[i::a.jobs]]
    log(f'translating {len(xjobs)} shaders in {len(chunks)} batches ...')
    t = time.time()
    with cf.ThreadPoolExecutor(len(chunks)) as ex:
        stderr = list(ex.map(lambda ic: run_xlate(a.dotnet, a.xlate, ic[1], a.work, ic[0]), enumerate(chunks)))
    log(f'translation done ({time.time() - t:.0f}s)')

    fail_by_type = collections.Counter()
    fail_msgs = collections.Counter()
    failed_keys = set()
    for s in stderr:
        for ln in s.splitlines():
            if ln.startswith('[RED] '):
                key, _, rest = ln[6:].partition(': ')
                failed_keys.add(key)
                typ, _, msg = rest.partition(': ')
                fail_by_type[typ] += 1
                fail_msgs[HEX.sub('#', f'{typ}: {msg}')[:140]] += 1
    notes = collections.Counter()
    structural_bad = collections.Counter()
    spv_files = []
    spv_buckets = []
    untyped_tex = 0
    ok = collections.Counter()
    tot = collections.Counter()
    by_kind = collections.Counter()
    by_top = collections.Counter()
    by_top_ok = collections.Counter()
    for j in xjobs:
        st, v2, kind, top, _, _ = uniq[j['key']]
        bucket = ('nvn2' if v2 else 'maxwell') + ':' + st
        tot[bucket] += 1
        by_kind[kind] += 1
        by_top[top] += 1
        sp = os.path.join(a.work, 'spv', j['key'] + '.spv')
        if os.path.exists(sp) and j['key'] not in failed_keys:
            ok[bucket] += 1
            by_top_ok[top] += 1
            spv_files.append(sp)
            spv_buckets.append(bucket)
            with open(sp, 'rb') as f:
                why = spirv_check.check(f.read())
            if why:
                structural_bad[why] += 1
            rp = os.path.join(a.work, 'spv', j['key'] + '.res.json')
            if os.path.exists(rp):
                ns = json.load(open(rp)).get('notes', [])
                if any('has no type in the answer table' in x for x in ns):
                    untyped_tex += 1
                for nline in ns:
                    notes[HEX.sub('#', nline.split('+')[0])[:80]] += 1
    total, total_ok = sum(tot.values()), sum(ok.values())

    val = None
    if a.spirv_val:
        log(f'validating {len(spv_files)} modules with spirv-val ...')

        def run_val(p):
            r = subprocess.run([a.spirv_val, '--target-env', a.target_env, p], capture_output=True, text=True,
                               errors='replace')
            if r.returncode == 0:
                return None
            first = (r.stdout + r.stderr).strip().splitlines()[:1]
            msg = re.sub(r'%\w+', '%id', first[0] if first else 'error')
            msg = re.sub(r'line \d+', 'line #', msg)
            msg = re.sub(r'VUID-([\w-]+)-\d+', r'VUID-\1', msg)
            return HEX.sub('#', msg)[:140]

        with cf.ThreadPoolExecutor(a.jobs) as ex:
            results = list(ex.map(run_val, spv_files))
        val_msgs = collections.Counter(r for r in results if r)
        bad_by_stage = collections.Counter(b for b, r in zip(spv_buckets, results) if r)
        val = {'tool': os.path.basename(a.spirv_val), 'target_env': a.target_env, 'checked': len(results),
               'valid': results.count(None), 'invalid': len(results) - results.count(None),
               'invalid_by_arch_stage': dict(bad_by_stage),
               'messages_top': dict(val_msgs.most_common(25))}

        if a.infer_txq_array:
            val['txq_array_inference'] = infer_txq_array(a, xjobs, spv_files, results, run_val)
    report = {
        'tool': 'sr-recomp shader_census',
        'translator': {'source': 'shirai91/nx2android PR #88 @ 3089bb229510075db9b048403192f21a860071fe',
                       'flags': 'TargetApi.Vulkan, Spirv, Ampere for sm86, no per-game answer table'},
        'romfs_files': n_entries,
        'zstd_dictionaries': n_zdicts,
        'files_with_shaders': files_with['files'],
        'scan_errors': dict(errors),
        'stage_instances': instances,
        'unique_shaders': len(uniq),
        'job_build_failures': bad_ctl,
        'translated_ok': total_ok,
        'translated_failed': total - total_ok,
        'translated_pct': round(100 * total_ok / total, 3) if total else None,
        'by_arch_stage': {b: {'total': tot[b], 'ok': ok[b]} for b in sorted(tot)},
        'by_container_kind': dict(by_kind),
        'by_romfs_top_dir': {d: {'total': by_top[d], 'ok': by_top_ok[d]} for d in sorted(by_top)},
        'failures_by_exception': dict(fail_by_type.most_common()),
        'failure_messages_top': dict(fail_msgs.most_common(25)),
        'translator_notes_top': dict(notes.most_common(15)),
        'shaders_with_untyped_textures': untyped_tex,
        'structural_check': {'checked': len(spv_files), 'failed': sum(structural_bad.values()),
                             'reasons': dict(structural_bad)},
        'spirv_val': val,
    }
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, 'w', encoding='utf-8') as f:
        json.dump(report, f, indent=1)
    log(f'{total_ok} of {total} unique shaders translated ({report["translated_pct"]}%); report: {a.out}')


if __name__ == '__main__':
    main()
