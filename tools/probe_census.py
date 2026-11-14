"""A shader census for the runtime probes, made from SR's own shaders.

usage: python tools/probe_census.py <shader cache dir> --out <dir> [--count N]

--probe-shader, --probe-vkexec and the pipeline self-test translate and draw census shaders (runtime/core
gfx_shader_probe.c probe_census_dir). totk-pc-recomp gave them TotK's census; this writes one from the inputs the game
stored in its shader cache while it played (<cache>/in/<id>.in, magic "TKSI", runtime/core/gfx_shader.c in_store), so
the probes run Splatoon Raiders' shaders:

  <out>/jobs.json       one entry per shader: key (the input id), stage (vs / ps / cs), c1 (hex), local (compute),
                        source (a group: one vertex shader and the fragment shaders the draw test pairs with it)
  <out>/code/<key>.code the code as the game handed it to NVN, whole (header included)
  <out>/code/<key>.ctl  its NVN2 control section (0x880 bytes), <key>.ans the translator answers the game's state gave;
                        the probes translate exactly these (runtime/core/gfx_shader_probe.c probe_census_inputs)

The runtime finds it through SR_PROBE_CENSUS=<out> (tools/run_probes.py sets it). The census is shader code of the
game: keep --out outside the repository (the default is ../out/probe-census).
"""
import argparse
import json
import os
import random
import struct
import sys

IN_MAGIC = 0x49534B54
IN_VERSION = 1
CONTROL = 0x880
STAGES = {0: 'vs', 4: 'ps', 5: 'cs'}   # gfx.h GFX_STAGE_VS / FS / CS; the other stages are left out


def read_input(path):
    """(stage, code[length], c1, local) of a stored input, or None when it is another version or malformed."""
    with open(path, 'rb') as f:
        b = f.read()
    if len(b) < 24:
        return None
    magic, version, stage, length, c1_len, answers = struct.unpack_from('<6I', b)
    if magic != IN_MAGIC or version != IN_VERSION or len(b) != 24 + answers + CONTROL + length + c1_len:
        return None
    control = b[24 + answers:24 + answers + CONTROL]
    code = b[24 + answers + CONTROL:24 + answers + CONTROL + length]
    c1 = b[24 + answers + CONTROL + length:]
    if not c1:   # stored with c1 inside the code (gfx_shader_get_async): code[c1 offset .. length], control+0x700;
        # most NVN2 shaders have none (offset == length)
        off, end = struct.unpack_from('<2I', control, 0x700)
        if off < end <= len(code):
            c1 = code[off:end]
    local = list(struct.unpack_from('<3I', control, 0x748)) if stage == 5 else [1, 1, 1]
    return stage, code, c1, local, control, b[24:24 + answers]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('cache')
    ap.add_argument('--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'out',
                                                  'probe-census'))
    ap.add_argument('--count', type=int, default=3000, help='shaders kept, spread over the three stages')
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    if os.path.commonpath([out, repo]) == repo:
        sys.exit('refusing to write game shader code inside the repository')
    in_dir = os.path.join(a.cache, 'in')
    names = sorted(n for n in os.listdir(in_dir) if n.endswith('.in') and len(n) == 67)
    by_stage = {'vs': [], 'ps': [], 'cs': []}
    bad = 0
    for n in names:
        r = read_input(os.path.join(in_dir, n))
        if not r:
            bad += 1
            continue
        stage, code, c1, local, control, ans = r
        if stage not in STAGES:
            continue
        if c1 and code[len(code) - len(c1):] != c1:   # c1, when there is one, ends the code
            bad += 1
            continue
        by_stage[STAGES[stage]].append((n[:64], code, c1, local, control, ans))
    rng = random.Random(1)   # the same census from the same cache
    share = {'vs': a.count * 3 // 8, 'ps': a.count * 3 // 8, 'cs': a.count // 4}
    keep = {s: sorted(rng.sample(v, min(len(v), share[s]))) for s, v in by_stage.items()}
    os.makedirs(os.path.join(out, 'code'), exist_ok=True)
    jobs = []
    # groups for the draw test: one vertex shader with up to four fragment shaders (the stored inputs do not say which
    # shaders the game drew together, so the pairs are arbitrary and the test tries many until one draws)
    vs, ps = keep['vs'], keep['ps']

    def write(key, code, control, ans):
        for ext, data in (('.code', code), ('.ctl', control), ('.ans', ans)):
            with open(os.path.join(out, 'code', key + ext), 'wb') as f:
                f.write(data)

    for i, (key, code, c1, local, control, ans) in enumerate(vs):
        write(key, code, control, ans)
        jobs.append({'job': {'key': key, 'stage': 'vs', 'c1': c1.hex(), 'local': local}, 'source': f'group{i}',
                     'origin': 'sr shader cache'})
    for i, (key, code, c1, local, control, ans) in enumerate(ps):
        write(key, code, control, ans)
        jobs.append({'job': {'key': key, 'stage': 'ps', 'c1': c1.hex(), 'local': local},
                     'source': f'group{i % max(1, len(vs))}', 'origin': 'sr shader cache'})
    for key, code, c1, local, control, ans in keep['cs']:
        write(key, code, control, ans)
        jobs.append({'job': {'key': key, 'stage': 'cs', 'c1': c1.hex(), 'local': local}, 'source': 'compute',
                     'origin': 'sr shader cache'})
    with open(os.path.join(out, 'jobs.json'), 'w', newline='\n') as f:
        f.write('[\n' + ',\n'.join(json.dumps(j) for j in jobs) + '\n]\n')
    print(f'probe census: {len(vs)} vertex, {len(ps)} fragment, {len(keep["cs"])} compute shaders of '
          f'{len(names)} stored inputs ({bad} unreadable) -> {out}', file=sys.stderr)


if __name__ == '__main__':
    main()
