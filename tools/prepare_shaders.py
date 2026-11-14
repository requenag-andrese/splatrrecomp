"""Translate every shader of the game ahead of play, so a run does not translate them as it meets them.

Reads every shader of the NSP's (or NUP's) RomFS and ExeFS main with PR #88's scanners (tools/shader_census.py scan_nsp), writes
them as the seed totk-pc-recomp's runtime prepares from (runtime/core/gfx_shader.c "preparation from record files",
layouts of totk-pc-recomp tools/prepare_shaders.py, MIT) next to sr.exe, and runs `sr.exe --prepare-shaders`: the
shaders are translated on every core into the build's shader cache (shader-cache/) and each one's stage library or
compute pipeline is built once into the GPU driver's disk cache.

    seed-programs.bin  "SRRCSPRG": stage, length, c1 length, runtime code hash, control section, code, c1
    seed-answers.bin   "SRRCSANS": stage, code hash, input id (zero: the runtime computes it), answers, priority

Each shader is seeded with its default answers (every texture slot 2D, float vertex inputs and colour outputs), the
ones a draw gives when its bound textures are 2D. A draw that binds another texture type to a slot the shader asks
about, or integer vertex inputs, still translates that variant when it meets it.

NVN2 control sections are seeded as the runtime sees them: the NVN1-layout host copy of runtime/sr/nvn2_control.c
(convert), byte for byte, since the code hash covers the whole 0x880-byte section. Shaders the game builds or loads
from elsewhere than RomFS and main are not seen here and are translated in play as before.

    python tools/prepare_shaders.py <.nsp | .nup> --vendor ../vendor --build ../out/build-vk [--no-run]

The seed files are game-derived and stay in the build directory (outside the repository); delete them afterwards.
"""
import argparse
import hashlib
import os
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shader_census  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CONTROL = 0x880
NVN2_READ = 0xc0                                 # runtime/sr/nvn2_control.c
GFX_STAGE = {'vs': 0, 'ps': 4, 'cs': 5}          # runtime/core/gfx.h GFX_STAGE_*
NVN2_GFX = {0: 0, 1: 4, 5: 5}                    # NVN stage at control +0x56 -> GFX stage (other stages are not translated)
TEX_2D = 4                                       # gfx_shader.c tex_names: none, 1d, 1darray, buffer, 2d, ...
ANS_REC = 4 + 32 + 32 + 147 + 1


def nvn2_host_control(g):
    """runtime/sr/nvn2_control.c convert: the NVN2 bytes at +0, the NVN1 fields the back end reads filled from them."""
    o = bytearray(CONTROL)
    o[:NVN2_READ] = g[:NVN2_READ]
    instr, c1a, c1e = struct.unpack_from('<I4xII', g, 0x38)
    total = max(c1e, instr)
    c1_off = c1a if c1e > c1a and c1e == total else total
    struct.pack_into('<II', o, 0x700, c1_off, total)
    struct.pack_into('<I', o, 0x714, g[0x56])
    if g[0x56] == 5:
        o[0x748:0x754] = g[0x58:0x64]
        struct.pack_into('<I', o, 0x754, (struct.unpack_from('<I', g, 0x64)[0] + 15) // 16 * 16)
    return bytes(o)


def default_answers(stage, control):
    """GfxShaderAnswers in gfx_shader.c pack_answers layout (147 bytes): every texture slot 2D, float inputs/outputs."""
    tex = bytes([TEX_2D]) * 32
    local = [0, 0, 0]   # vk_exec_draw.c answers_for: zero for graphics stages
    if stage == 5:      # compute: the control section's thread group, 0 read as 1
        local = [v or 1 for v in struct.unpack_from('<III', control, 0x748)]
    packed = tex + bytes(32) + bytes(32) + bytes(32) + bytes(8) + struct.pack('<HHHIB', *local, 0, 0)
    assert len(packed) == 147
    return packed


def code_hash(stage, control, code, length, c1):
    """gfx_shader.c build_code_hash."""
    return hashlib.sha256(struct.pack('<I', stage) + control + struct.pack('<Q', length) + code[:length] +
                          struct.pack('<I', len(c1)) + c1).digest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('nsp')
    ap.add_argument('--vendor', required=True)
    ap.add_argument('--build', required=True, help='build directory holding the vulkan-profile sr.exe')
    ap.add_argument('--workers', type=int, default=max(1, (os.cpu_count() or 4) - 1))
    ap.add_argument('--no-run', action='store_true', help='write the seed files only')
    a = ap.parse_args()
    out = os.path.abspath(a.build)
    if os.path.commonpath([out, REPO]) == REPO:
        sys.exit('refusing to write game-derived data inside the repository')
    t0 = time.monotonic()
    _, uniq, *_ = shader_census.scan_nsp(a.nsp, a.vendor, a.workers)
    progs, recs, skipped = {}, [], {'stage': 0, 'layout': 0}
    for key in sorted(uniq):
        st, v2, kind, top, ctl, code = uniq[key]
        if v2:
            stage = NVN2_GFX.get(ctl[0x56])
            control = nvn2_host_control(ctl) if stage is not None else None
        else:
            stage = GFX_STAGE.get(st)
            control = bytes(ctl[:CONTROL]).ljust(CONTROL, b'\0') if stage is not None else None
        if stage is None:
            skipped['stage'] += 1
            continue
        c1_off, length = struct.unpack_from('<II', control, 0x700)
        if length > len(code) or c1_off > length or (not v2 and stage != 5 and length <= 0x30):
            skipped['layout'] += 1
            continue
        c1 = bytes(code[c1_off:length])
        h = code_hash(stage, control, code, length, c1)
        if h in progs:
            continue
        progs[h] = struct.pack('<III', stage, length, len(c1)) + h + control + bytes(code[:length]) + c1
        rec = struct.pack('<I', stage) + h + bytes(32) + default_answers(stage, control) + bytes([2])
        assert len(rec) == ANS_REC
        recs.append(rec)
    with open(os.path.join(out, 'seed-programs.bin'), 'wb') as f:
        f.write(b'SRRCSPRG' + struct.pack('<II', 1, 0))
        for body in progs.values():
            f.write(body)
    with open(os.path.join(out, 'seed-answers.bin'), 'wb') as f:
        f.write(b'SRRCSANS' + struct.pack('<II', 1, ANS_REC))
        for rec in recs:
            f.write(rec)
    by = {s: sum(1 for r in recs if struct.unpack_from('<I', r)[0] == s) for s in (0, 4, 5)}
    print(f'seed: {len(recs)} shaders (vertex {by[0]}, fragment {by[4]}, compute {by[5]}) of {len(uniq)} found, '
          f'skipped {skipped}; written in {time.monotonic() - t0:.1f} s', flush=True)
    if a.no_run:
        return 0
    r = subprocess.run([os.path.join(out, 'sr.exe'), '--prepare-shaders'], cwd=out)
    print(f'sr.exe --prepare-shaders: rc={r.returncode}; total {time.monotonic() - t0:.1f} s', flush=True)
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
