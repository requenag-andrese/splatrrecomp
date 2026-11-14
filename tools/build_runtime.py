"""Link the runtime with the compiled generated code into an executable (outside this repository).

usage: python tools/build_runtime.py --gen <gen dir> --obj <generated objects> --exefs <dir> --vendor <dir>
                                     --clang <clang.exe> --out <build dir> [--nsp <.nsp> | --nup <.nup>]
                                     [--profile null|vulkan] [--deps <toolchains dir>] [--jobs N]

Profile `null`: the null graphics back end and no SDL3 / Opus / libvpx / zstd, so it needs nothing beyond llvm-mingw
and the vendored Vulkan core headers. The files that need those libraries are left out and listed.
Profile `vulkan`: the Vulkan back end, the Win32 window and SDL3 gamepad / audio output. Needs, under --deps:
vulkan-headers/ (Khronos Vulkan-Headers), vma/vk_mem_alloc.h, sdl3/SDL3-*/x86_64-w64-mingw32 and
vulkan-implib/libvulkan-1.a. Movies (libvpx, Opus) and libzstd are still left out.

Writes <out>/modules.c (layout, linked function tables, expected ExeFS hashes, default paths: the glue
totk-pc-recomp's tools/build_game.py module_glue writes) and <out>/romfs_config.h + romfs-index.bin (the RomFS of
--nsp or --nup, read in place; see romfs_from_nsp / romfs_from_nup), compiles runtime/core and runtime/sr, and links.
Undefined symbols are reported grouped.
"""
import argparse
import shutil
import collections
import concurrent.futures as cf
import hashlib
import json
import os
import re
import struct
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RT = os.path.join(REPO, 'runtime')
sys.path.insert(0, REPO)

from srrecomp import npdm  # noqa: E402

# SDL front ends of files Windows builds from their Win32 twin, and the movie decoders: never built here.
SDL_TWINS = {f: 'SDL front end (Windows builds the _win twin)' for f in
             ('folder_pick_sdl.c', 'gfx_window_sdl.c', 'input_sdl.c', 'swkbd_sdl.c')}
NO_MOVIES = {'ov_media.c': 'Opus', 'ov_movie.c': 'libvpx'}
# Files of runtime/core that need a library the null profile does not have, or the Vulkan back end.
NEEDS_LIBS = {
    **SDL_TWINS, **NO_MOVIES,
    'audio_mixer.c': 'SDL3', 'input.c': 'SDL3', 'input_win.c': 'Win32 window',
    'gfx_present.c': 'Vulkan', 'vk_interp.c': 'Vulkan', 'gfx_window_win.c': 'Vulkan', 'gfx_window_probe.c': 'Vulkan', 'astc_probe.c': 'Vulkan',
}
PROFILES = {
    'null': {'exclude': NEEDS_LIBS, 'vk': False, 'define': 'SR_NULL_PROFILE'},
    'vulkan': {'exclude': {**SDL_TWINS, **NO_MOVIES}, 'vk': True, 'define': 'SR_VULKAN_PROFILE'},
}
RT_FLAGS = ['-std=gnu11', '-O2', '-g', '-fno-strict-aliasing', '-march=x86-64-v3', '-D_WIN32_WINNT=0x0A00',
            '-Wno-everything']
CXX_FLAGS = ['-std=gnu++17', '-O2', '-g', '-march=x86-64-v3', '-D_WIN32_WINNT=0x0A00', '-Wno-everything']
LINK_FLAGS = ['-fuse-ld=lld', '-Wl,--stack,16777216', '-Wl,--error-limit=0']
LIBS = ['-lpsapi', '-lbcrypt', '-luser32', '-lgdi32', '-ldwmapi', '-limm32', '-lwinmm', '-lole32', '-lshell32', '-luuid']


def c_string(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def module_glue(gen, exefs, romfs='', profile='', game=None, version=None):
    # game: the NSP or NUP the version is read from (romfs is the folder of a copied romfs.bin with --romfs-copied)
    game = romfs if game is None else game
    layout = json.load(open(os.path.join(gen, 'layout.json')))
    linked = [m['name'] for m in layout if os.path.exists(os.path.join(gen, m['name'], f'{m["name"]}_table.c'))]
    out = ['// Module glue written by sr-recomp tools/build_runtime.py (after totk-pc-recomp build_game.py module_glue).',
           '#include "dispatch.h"', '#include "host.h"', '#include "layout.h"', '',
           'const RtLayoutEntry rt_layout[] = {']
    out += [f'    {{"{m["name"]}", MOD_{m["name"].upper()}_BASE, MOD_{m["name"].upper()}_SIZE}},' for m in layout]
    out += ['};', f'const int rt_layout_count = {len(layout)};', '']
    for n in linked:
        out += [f'extern const uint64_t {n}_fn_addrs[];', f'extern const GuestFn {n}_fn_ptrs[];',
                f'extern const uint64_t {n}_fn_count;']
    out += ['', 'int rt_linked_modules(RtModule *out, int max) {', '    int n = 0;']
    for n in linked:
        u = n.upper()
        out.append(f'    if (n < max) out[n++] = (RtModule){{"{n}", MOD_{u}_BASE, MOD_{u}_SIZE, {n}_fn_addrs, '
                   f'{n}_fn_ptrs, {n}_fn_count}};')
    out += ['    return n;', '}', '', 'const RtFileHash rt_exefs_sha256[] = {']
    hashes = {}
    for f in sorted(os.listdir(exefs)):
        with open(os.path.join(exefs, f), 'rb') as fh:
            hashes[f] = hashlib.sha256(fh.read()).hexdigest()
    out += [f'    {{{c_string(f)}, {c_string(h)}}},' for f, h in hashes.items()]
    out += ['};', f'const int rt_exefs_sha256_count = {len(hashes)};', '']
    for name, path in (('exefs', exefs), ('romfs', romfs), ('profile', profile)):
        out.append(f'const char rt_default_{name}[] = {c_string(path.replace(os.sep, "/"))};')
    out += ['const int rt_game_version = 0;', f'const char rt_port_version[] = {c_string(port_version())};', '']
    with open(os.path.join(exefs, 'main.npdm'), 'rb') as fh:
        program_id = npdm.parse(fh.read())['program_id']
    out += ['#include <stdint.h>', f'const uint64_t sr_program_id = 0x{program_id}ull;   // main.npdm ACI0', '']
    shown = version if version is not None else display_version(game)
    out += [f'const char sr_display_version[] = {c_string(shown)};   // control.nacp', '']
    return '\n'.join(out)


def port_version(repo=REPO):
    """The repository's commit for the menu's version line: short hash, '-dirty' with uncommitted changes. A package of
    the builder (builder/package.py) has no git: its RELEASE file ("<12-hex commit>[+<diff fingerprint>]") says it."""
    stamp = os.path.join(repo, 'RELEASE')
    if os.path.isfile(stamp) and not os.path.exists(os.path.join(repo, '.git')):
        with open(stamp, encoding='ascii', errors='replace') as f:
            release = f.read().strip()
        if not release or release == 'unknown':
            return 'unknown'
        head, dirty, _ = release.partition('+')
        return head[:7] + ('-dirty' if dirty else '')
    try:
        r = subprocess.run(['git', '-C', repo, 'describe', '--always', '--dirty'], capture_output=True, text=True)
        return r.stdout.strip() or 'unknown'
    except OSError:
        return 'unknown'


def display_version(nsp):
    """The display version in the NSP's control.nacp (+0x3060, 16 bytes), or the NUP's meta; '' without either."""
    if not nsp:
        return ''
    from srrecomp import sources
    if nsp.lower().endswith('.nup'):
        return sources.Nup(nsp).display_version()
    with open(nsp, 'rb') as fh:
        top = sources.parse_pfs0(sources.FileSlice(fh, 0, os.fstat(fh.fileno()).st_size))
        for name, sl in top.items():
            if not name.endswith('.nca') or name.endswith('.cnmt.nca'):
                continue
            nca = sources.Nca(sl)
            if nca.content_type != 2:   # Control
                continue
            romfs = sources.nca_romfs(nca)
            for path, off, size in sources.romfs_entries(romfs):
                if path == 'control.nacp' and size >= 0x3070:
                    return romfs.read(off + 0x3060, 16).split(b'\0')[0].decode('ascii', 'replace')
    return ''


def game_icon(nsp, dest):
    """sr-recomp: the game's own icon from the NSP's control NCA (icon_AmericanEnglish.dat, else the first icon_*.dat: a
    256x256 JPEG), written to dest as a .ico of PNG frames at 256, 64, 48, 32 and 16 px for sr.exe's icon resource. The
    frames are drawn by Windows' own imaging (System.Drawing through PowerShell): the venv has no image library. It stays
    in the build folder, like the rest of the game. False when there is none (a NUP keeps no icons, other systems)."""
    if not nsp or nsp.lower().endswith('.nup') or os.name != 'nt':
        return False
    from srrecomp import sources
    jpg = None
    with open(nsp, 'rb') as fh:
        top = sources.parse_pfs0(sources.FileSlice(fh, 0, os.fstat(fh.fileno()).st_size))
        for name, sl in top.items():
            if not name.endswith('.nca') or name.endswith('.cnmt.nca'):
                continue
            nca = sources.Nca(sl)
            if nca.content_type != 2:   # Control
                continue
            romfs = sources.nca_romfs(nca)
            icons = {p: (off, size) for p, off, size in sources.romfs_entries(romfs)
                     if p.startswith('icon_') and p.endswith('.dat') and size}
            pick = 'icon_AmericanEnglish.dat' if 'icon_AmericanEnglish.dat' in icons else min(icons, default=None)
            if pick:
                jpg = romfs.read(*icons[pick])
            break
    if not jpg:
        return False
    sizes = (256, 64, 48, 32, 16)
    src = dest + '.jpg'
    with open(src, 'wb') as f:
        f.write(jpg)
    ps = ("Add-Type -AssemblyName System.Drawing; $s = [System.Drawing.Image]::FromFile('{src}'); "
          "foreach ($n in {sizes}) {{ $b = New-Object System.Drawing.Bitmap $n, $n; "
          "$g = [System.Drawing.Graphics]::FromImage($b); "
          "$g.InterpolationMode = [System.Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic; "
          "$g.DrawImage($s, 0, 0, $n, $n); $g.Dispose(); $b.Save('{dest}.' + $n + '.png', "
          "[System.Drawing.Imaging.ImageFormat]::Png); "
          "$d = $b.LockBits((New-Object System.Drawing.Rectangle 0, 0, $n, $n), "
          "[System.Drawing.Imaging.ImageLockMode]::ReadOnly, [System.Drawing.Imaging.PixelFormat]::Format32bppArgb); "
          "$a = New-Object byte[] ($n * $n * 4); [System.Runtime.InteropServices.Marshal]::Copy($d.Scan0, $a, 0, $a.Length); "
          "$b.UnlockBits($d); [System.IO.File]::WriteAllBytes('{dest}.' + $n + '.bgra', $a); $b.Dispose() }}; "
          "$s.Dispose()").format(
              src=src.replace("'", "''"), dest=dest.replace("'", "''"), sizes=','.join(map(str, sizes)))
    r = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', ps], capture_output=True, text=True,
                       errors='replace')
    if r.returncode:
        print(f'game icon: the frames could not be drawn ({(r.stderr.strip().splitlines() or ["?"])[0]})', file=sys.stderr)
        return False
    frames = []
    for n in sizes:
        with open(f'{dest}.{n}.png', 'rb') as f:
            png = f.read()
        with open(f'{dest}.{n}.bgra', 'rb') as f:
            bgra = f.read()
        os.remove(f'{dest}.{n}.png')
        os.remove(f'{dest}.{n}.bgra')
        if n >= 256:
            frames.append((n, png))   # PNG, as Windows Vista and later read the large frame
            continue
        # the small frames as a classic DIB, which every icon reader takes: BITMAPINFOHEADER with twice the height,
        # 32-bit BGRA rows bottom-up, then the AND mask (1 bit a pixel, rows padded to 32 bits; all clear: alpha rules)
        rows = [bgra[y * n * 4:(y + 1) * n * 4] for y in range(n)]
        mask = b'\0' * (((n + 31) // 32) * 4 * n)
        dib = struct.pack('<IiiHHIIiiII', 40, n, 2 * n, 1, 32, 0, n * n * 4 + len(mask), 0, 0, 0, 0)
        frames.append((n, dib + b''.join(reversed(rows)) + mask))
    os.remove(src)
    # ICONDIR, one ICONDIRENTRY per frame (0 means 256), then the frames' data
    head = struct.pack('<HHH', 0, 1, len(frames))
    offset, entries, data = 6 + 16 * len(frames), b'', b''
    for n, png in frames:
        entries += struct.pack('<BBBBHHII', n % 256, n % 256, 0, 0, 1, 32, len(png), offset + len(data))
        data += png
    with open(dest, 'wb') as f:
        f.write(head + entries + data)
    return True


ROMFS_PLACEHOLDER = ('// No RomFS index yet (sr-recomp tools/build_runtime.py).\n#pragma once\n#define ROMFS_MISSING 1\n'
                     '#define ROMFS_IMAGE_SIZE 0ull\n#define ROMFS_IMAGE_SHA256 ""\n#define ROMFS_INDEX_SHA256 ""\n'
                     '#define ROMFS_FILE_COUNT 0ull\n')


def romfs_from_nsp(nsp, out):
    """romfs-index.bin (the SRRCRFS1 layout of totk-pc-recomp tools/romfs_index.py build_index) and romfs_config.h for
    the RomFS level inside a plaintext NSP, read in place by runtime/core/romfs.c (SR_ROMFS_IMAGE_OFFSET). The image
    SHA-256 is left empty: hashing 13 GB at every build is not worth it, and romfs.c only checks it on request."""
    from srrecomp import sources
    sl = sources.nca_romfs(sources.open_nsp_program(nsp))
    files = sorted(((p.encode(), off, size) for p, off, size in sources.romfs_entries(sl)), key=lambda e: e[0])
    rows, strings = bytearray(), bytearray()
    for name, off, size in files:
        rows += struct.pack('<QQII', off, size, len(strings), len(name) + 1)
        strings += name + b'\0'
    index = struct.pack('<8sQQ', b'SRRCRFS1', len(files), 24 + len(rows)) + bytes(rows) + bytes(strings)
    with open(os.path.join(out, 'romfs-index.bin'), 'wb') as f:
        f.write(index)
    return ('// RomFS of the plaintext NSP, read in place (sr-recomp tools/build_runtime.py romfs_from_nsp).\n'
            '#pragma once\n'
            f'#define ROMFS_IMAGE_SIZE {sl.size}ull\n'
            '#define ROMFS_IMAGE_SHA256 ""\n'
            f'#define ROMFS_INDEX_SHA256 "{hashlib.sha256(index).hexdigest()}"\n'
            f'#define ROMFS_FILE_COUNT {len(files)}ull\n'
            f'#define SR_ROMFS_IMAGE_OFFSET {sl.off}ull\n'), len(files)


SIDE_BIT = 1 << 62   # romfs-index.bin offset flag: the file is in the side file, not the image (runtime/core/romfs.c)
SIDE_FILE = 'romfs-side.bin'


def romfs_from_nup(paths, out, overlay='replace'):
    """romfs-index.bin and romfs_config.h for the RomFS of a NUP (vendor/nup SPEC.md v1), read in place. Returns
    (config, file count, the .nup the runtime opens).

    paths: one container, or the files of a split pack (nup pack --split: <name>.nup with the base,
    <name>.update.<version>.nup with a standalone update each); the packages of all of them are pooled.

    The RomFS is the newest update package's tree. overlay 'replace' takes it alone, as the console does with a patch
    NCA (its RomFS is the whole patched RomFS: in Splatoon Raiders 1.1.0 it equals the NSP's file for file);
    'shadow' lays it over the base tree by path, the layering the spec describes, which also keeps files only the base
    has. Without an update the base tree is used.

    Raw files (codec 0) are indexed at their extent in the .nup. zstd files (codec 1) are decompressed once into
    <out>/romfs-side.bin (each checked against its SHA-256) and indexed there with SIDE_BIT set; the side file is
    rebuilt only when the set of zstd files changes (romfs-side.id). Raw extents are not hashed at build time, as with
    the NSP."""
    from srrecomp import sources
    try:
        nup, tree, pick = sources.nup_romfs(paths, overlay)
    except ValueError as e:
        sys.exit(f'--nup: {e} (the runtime reads one file)')
    pkgs = {k: v[1] for k, v in pick.items()}
    path = nup.fh.name
    for f in tree.values():
        if f['codec'] not in (0, 1):
            sys.exit(f'NUP codec {f["codec"]} is not supported')
    image_size = os.path.getsize(path)

    zfiles = sorted((n for n, f in tree.items() if f['codec'] == 1 and f['size']), key=lambda n: n.encode())
    side_off, pos, ident = {}, 0, hashlib.sha256(b'sr-romfs-side 1')
    for n in zfiles:
        f = tree[n]
        side_off[n] = pos
        pos = (pos + f['size'] + 15) & ~15
        ident.update(struct.pack('<Q', f['size']) + f['sha256'])
    side_size, ident = pos, ident.hexdigest()
    side, idf = os.path.join(out, SIDE_FILE), os.path.join(out, 'romfs-side.id')
    fresh = (os.path.isfile(side) and os.path.getsize(side) == side_size and os.path.isfile(idf)
             and open(idf).read().strip() == ident)
    if zfiles and not fresh:
        import zstandard
        dz = zstandard.ZstdDecompressor()
        tmp = side + '.tmp'
        with open(tmp, 'wb') as w, open(path, 'rb') as r:
            for k, n in enumerate(zfiles):
                f = tree[n]
                r.seek(f['off'])
                data = dz.decompress(r.read(f['stored']), max_output_size=f['size'])
                if len(data) != f['size'] or hashlib.sha256(data).digest() != f['sha256']:
                    sys.exit(f'NUP zstd file {k} of {len(zfiles)} does not match its size / SHA-256')
                w.seek(side_off[n])
                w.write(data)
            w.truncate(side_size)
        os.replace(tmp, side)
        with open(idf, 'w') as fh:
            fh.write(ident + '\n')
        print(f'romfs: {len(zfiles)} zstd files decompressed into {SIDE_FILE} ({side_size / 2**30:.2f} GiB)',
              file=sys.stderr)
    elif zfiles:
        print(f'romfs: {SIDE_FILE} is up to date ({len(zfiles)} zstd files)', file=sys.stderr)

    rows, strings = bytearray(), bytearray()
    for n in sorted(tree, key=lambda n: n.encode()):
        f, name = tree[n], n.encode()
        if not f['size']:
            off = 0
        elif f['codec'] == 1:
            off = SIDE_BIT | side_off[n]
        else:
            if f['off'] + f['size'] > image_size:
                sys.exit('a NUP extent lies past the end of the file')
            off = f['off']
        rows += struct.pack('<QQII', off, f['size'], len(strings), len(name) + 1)
        strings += name + b'\0'
    index = struct.pack('<8sQQ', b'SRRCRFS1', len(tree), 24 + len(rows)) + bytes(rows) + bytes(strings)
    with open(os.path.join(out, 'romfs-index.bin'), 'wb') as f:
        f.write(index)
    used = 'update' if pkgs['update'] else 'base'
    if pkgs['update'] and pkgs['base'] and overlay == 'replace':
        only_base = len(set(n for n, _ in pick['base'][0].tree(pkgs['base']['romfs'])) - set(tree))
        print(f'romfs: update tree only ({only_base} files only the base has are left out; --nup-overlay shadow '
              'keeps them)', file=sys.stderr)
    return ('// RomFS of the NUP, read in place (sr-recomp tools/build_runtime.py romfs_from_nup).\n'
            f'// Package: {used} {pkgs[used]["version"]:#x}, overlay {overlay}.\n'
            '#pragma once\n'
            f'#define ROMFS_IMAGE_SIZE {image_size}ull\n'
            '#define ROMFS_IMAGE_SHA256 ""\n'
            f'#define ROMFS_INDEX_SHA256 "{hashlib.sha256(index).hexdigest()}"\n'
            f'#define ROMFS_FILE_COUNT {len(tree)}ull\n'
            '#define SR_ROMFS_IMAGE_OFFSET 0ull\n'
            f'#define SR_ROMFS_SIDE_FILE "{SIDE_FILE}"   // next to romfs-index.bin\n'
            f'#define SR_ROMFS_SIDE_SIZE {side_size}ull\n'
            f'#define SR_ROMFS_SIDE_BIT {SIDE_BIT:#x}ull\n'), len(tree), path


def nvn_extra_names(main_nso):
    """NVN proc names (nvn + capital) among main's .rodata strings that runtime/core/nvn_procs.inc does not list, in a
    stable order. Only API names leave this function; nothing else of the module is written."""
    from srrecomp.nso import Nso
    with open(main_nso, 'rb') as fh:
        n = Nso(fh.read())
    ro = n.image[n.ro[0]:n.ro[0] + n.ro[1]]
    names = {m.decode() for m in re.findall(rb'(?<=\x00)(nvn[A-Z][A-Za-z0-9_]{2,80})(?=\x00)', ro)}
    with open(os.path.join(RT, 'core', 'nvn_procs.inc')) as fh:
        known = set(re.findall(r'NVN_PROC\((\w+)\)', fh.read()))
    known.add('nvnBootstrapLoader')   # ov_gfx.c overrides it by symbol
    return sorted(names - known)


def write_rsp(path, files):
    """The linker's response file: one object per line, quoted, since a folder name may hold spaces
    ("C:/Prueba Builder/..."); forward slashes, which both of clang's response-file readers take as they are."""
    with open(path, 'w', newline='\n') as f:
        f.write('\n'.join('"' + p.replace('\\', '/') + '"' for p in files))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    for k in ('--gen', '--obj', '--exefs', '--vendor', '--clang', '--out'):
        ap.add_argument(k, required=True)
    src = ap.add_mutually_exclusive_group()
    src.add_argument('--nsp', help='plaintext NSP: its RomFS is indexed and read in place (else no RomFS)')
    src.add_argument('--nup', action='append', help='NUP (vendor/nup SPEC.md v1): its newest RomFS is indexed and '
                     'read in place; zstd files are decompressed once into <out>/romfs-side.bin. Repeat it for the '
                     'files of a split pack (base + update containers)')
    ap.add_argument('--romfs-copied', metavar='DIR',
                    help="with --nsp: the NSP's RomFS level was copied to DIR/romfs.bin (builder/, a portable game "
                         "folder): the index is the NSP's, the runtime reads the copy (DIR, or romfs/ next to sr.exe)")
    ap.add_argument('--romfs-indexed', metavar='DIR',
                    help='instead of --nsp or --nup: DIR/romfs.bin with its own DIR/romfs-index.bin (builder/, a game '
                         'folder made from a NUP); the runtime reads the copy (DIR, or romfs/ next to sr.exe)')
    ap.add_argument('--display-version', help='the version sr.exe shows, when no game file is read for it (--romfs-indexed)')
    ap.add_argument('--nup-overlay', choices=('replace', 'shadow'), default='replace',
                    help="update RomFS alone (the console's behaviour, default) or laid over the base by path")
    ap.add_argument('--profile', default='null', choices=sorted(PROFILES))
    ap.add_argument('--deps', help='toolchains folder with the vulkan profile\'s headers and libraries')
    ap.add_argument('--xlate', help='sr_xlate build folder (tools/sr_xlate): copied next to sr.exe as the shader translator')
    ap.add_argument('--dotnet', help='.NET folder the translator runs on (DOTNET_ROOT for it, baked into sr.exe)')
    ap.add_argument('--fonts', help='tools/firmware_fonts.py output: its shared-fonts/ is copied next to sr.exe')
    ap.add_argument('--streamline', help="NVIDIA Streamline SDK folder (2.14): DLSS Frame Generation and Reflex "
                    '(runtime/sr/vk_dlssg.cpp, dlss_fg in sr-config.ini); its headers are compiled in and its signed plugins '
                    'copied next to sr.exe')
    ap.add_argument('--fidelityfx', help="AMD FidelityFX SDK folder (2.4): FSR 3 frame generation on Vulkan "
                    '(runtime/sr/vk_fsrfg.cpp, fsr_fg in sr-config.ini); its headers are compiled in and its signed loader and '
                    'frame generation DLLs copied next to sr.exe')
    ap.add_argument('--dlss', help="NVIDIA's nvngx_dlss.dll (github.com/NVIDIA/DLSS, NVIDIA RTX SDK license): copied "
                    'next to sr.exe, where NGX looks for it (dlaa = 1 in sr-config.ini, runtime/sr/vk_dlss.c)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    if os.path.commonpath([out, REPO]) == REPO:
        sys.exit('refusing to build inside the repository')
    os.makedirs(os.path.join(out, 'rtobj'), exist_ok=True)
    clang = os.path.abspath(a.clang)
    gen = os.path.abspath(a.gen)

    config = ROMFS_PLACEHOLDER
    nsp = os.path.abspath(a.nsp) if a.nsp else ''   # the game file the runtime opens (and reads the version from)
    if a.nup:
        config, nfiles, nsp = romfs_from_nup([os.path.abspath(x) for x in a.nup], out, a.nup_overlay)
        print(f'romfs: {nfiles} files indexed in place from {os.path.basename(nsp)}', file=sys.stderr)
    elif nsp:
        config, nfiles = romfs_from_nsp(nsp, out)
        print(f'romfs: {nfiles} files indexed in place from the NSP', file=sys.stderr)
    romfs = nsp   # what the runtime opens: the NSP itself (read in place) or the folder of a copied romfs.bin
    if a.romfs_copied:
        if not a.nsp:
            sys.exit('--romfs-copied needs --nsp')
        # the same index, offsets from the image's start: without SR_ROMFS_IMAGE_OFFSET romfs.c opens <dir>/romfs.bin
        config = ''.join(l + '\n' for l in config.splitlines() if 'SR_ROMFS_IMAGE_OFFSET' not in l)
        config = config.replace('read in place (sr-recomp tools/build_runtime.py romfs_from_nsp)',
                                'copied to romfs.bin (sr-recomp tools/build_runtime.py --romfs-copied)')
        romfs = os.path.abspath(a.romfs_copied)
        print(f'romfs: read from {os.path.join(romfs, "romfs.bin")}', file=sys.stderr)
    if a.romfs_indexed:
        if a.nsp or a.nup or a.romfs_copied:
            sys.exit('--romfs-indexed goes without --nsp, --nup and --romfs-copied')
        romfs = os.path.abspath(a.romfs_indexed)
        with open(os.path.join(romfs, 'romfs-index.bin'), 'rb') as f:
            index = f.read()
        if index[:8] != b'SRRCRFS1':
            sys.exit(f'--romfs-indexed: {os.path.join(romfs, "romfs-index.bin")} is not an index')
        nfiles = struct.unpack_from('<Q', index, 8)[0]
        with open(os.path.join(out, 'romfs-index.bin'), 'wb') as f:
            f.write(index)
        config = ('// RomFS files copied to romfs.bin with their own index (sr-recomp tools/build_runtime.py --romfs-indexed).\n'
                  '#pragma once\n'
                  f'#define ROMFS_IMAGE_SIZE {os.path.getsize(os.path.join(romfs, "romfs.bin"))}ull\n'
                  '#define ROMFS_IMAGE_SHA256 ""\n'
                  f'#define ROMFS_INDEX_SHA256 "{hashlib.sha256(index).hexdigest()}"\n'
                  f'#define ROMFS_FILE_COUNT {nfiles}ull\n')
        print(f'romfs: {nfiles} files read from {os.path.join(romfs, "romfs.bin")}', file=sys.stderr)
    with open(os.path.join(out, 'modules.c'), 'w', newline='\n') as f:
        f.write(module_glue(gen, os.path.abspath(a.exefs), romfs, os.path.join(out, 'profile'), nsp,
                            a.display_version))   # saves, account data
        dotnet = os.path.abspath(a.dotnet).replace(os.sep, '/') if a.dotnet else ''
        f.write(f'const char sr_dotnet_root[] = "{dotnet}";   // DOTNET_ROOT for the shader translator (sr/xlate_env.c)\n')
    with open(os.path.join(out, 'romfs_config.h'), 'w', newline='\n') as f:
        f.write(config)
    extra = nvn_extra_names(os.path.join(os.path.abspath(a.exefs), 'main'))
    with open(os.path.join(out, 'nvn_procs_extra.inc'), 'w', newline='\n') as f:
        f.write('// NVN names in main beyond runtime/core/nvn_procs.inc (sr-recomp tools/build_runtime.py).\n')
        f.write(''.join(f'NVN_PROC({n})\n' for n in extra))
    print(f'nvn: {len(extra)} proc names beyond the TotK list get logged stubs', file=sys.stderr)

    prof = PROFILES[a.profile]
    libs = list(LIBS)
    extra_dlls = []
    common = ['-I', os.path.join(RT, 'core'), '-I', os.path.join(RT, 'compat', 'null-zstd'), '-I', gen, '-I', out,
              f'-D{prof["define"]}', '-DSR_NVN_EXTRA']
    if prof['vk']:
        if not a.deps:
            sys.exit('the vulkan profile needs --deps')
        deps = os.path.abspath(a.deps)
        sdl = next(os.path.join(deps, 'sdl3', d, 'x86_64-w64-mingw32') for d in os.listdir(os.path.join(deps, 'sdl3'))
                   if d.startswith('SDL3-'))
        inc = [*common, '-I', os.path.join(deps, 'vulkan-headers', 'include'), '-I', os.path.join(deps, 'vma'),
               '-I', os.path.join(sdl, 'include')]
        libs = ['-L', os.path.join(sdl, 'lib'), '-L', os.path.join(deps, 'vulkan-implib'), '-lvulkan-1', '-lSDL3', *libs]
        extra_dlls.append(os.path.join(sdl, 'bin', 'SDL3.dll'))
        vpx = os.path.join(deps, 'libvpx', 'ucrt64')   # VP9 movies (runtime/sr/ov_codec_video.c), linked statically
        if os.path.isfile(os.path.join(vpx, 'lib', 'libvpx.a')):
            inc += ['-I', os.path.join(vpx, 'include'), '-DSR_HAVE_LIBVPX']
            libs = [os.path.join(vpx, 'lib', 'libvpx.a'), '-Wl,-Bstatic', '-lpthread', '-Wl,-Bdynamic', *libs]   # libvpx threads: winpthreads, static
            print('libvpx: VP9 movies decoded on the host', file=sys.stderr)
    else:
        vk = os.path.join(os.path.abspath(a.vendor), 'nx2android-pr88/nxnative/dm/third_party/vulkan-headers/include')
        inc = [*common, '-I', os.path.join(RT, 'compat'), '-I', os.path.join(RT, 'compat', 'null-vma'), '-I', vk]
    core = os.path.join(RT, 'core')
    srcs = [os.path.join(core, f) for f in sorted(os.listdir(core))
            if (f.endswith('.c') or (prof['vk'] and f.endswith('.cpp'))) and f not in prof['exclude']
            and (prof['vk'] or not f.startswith('vk_'))]
    srcs += [os.path.join(core, 'platform', f) for f in ('win32.c', 'common.c')]
    srcs += [os.path.join(RT, 'sr', f) for f in sorted(os.listdir(os.path.join(RT, 'sr')))
             if f.endswith('.c') or (prof['vk'] and f.endswith('.cpp'))]
    if prof['vk']:   # the in-game menu (runtime/core/ui, totk-pc-recomp's) and Dear ImGui, which draws it
        imgui = os.path.join(RT, 'third_party', 'imgui')
        inc += ['-I', imgui]
        srcs += [os.path.join(core, 'ui', f) for f in sorted(os.listdir(os.path.join(core, 'ui')))
                 if f.endswith('.cpp') and f != 'menu_sdl.cpp']
        srcs += [os.path.join(imgui, f) for f in sorted(os.listdir(imgui)) if f.endswith('.cpp')]
        srcs.append(os.path.join(imgui, 'backends', 'imgui_impl_vulkan.cpp'))
    sl_dir = os.path.abspath(a.streamline) if a.streamline and prof['vk'] else ''
    if sl_dir:   # DLSS Frame Generation (runtime/sr/vk_dlssg.cpp); without it that file only passes calls through
        inc += ['-I', os.path.join(sl_dir, 'include'), '-DSR_HAVE_STREAMLINE']
        libs += ['-lwintrust', '-lcrypt32']
        print('streamline: DLSS Frame Generation compiled in', file=sys.stderr)
    fx_dir = os.path.abspath(a.fidelityfx) if a.fidelityfx and prof['vk'] else ''
    if fx_dir:   # FSR 3 frame generation (runtime/sr/vk_fsrfg.cpp); without it that file only answers "off"
        kit = os.path.join(fx_dir, 'Kits', 'FidelityFX')
        inc += ['-I', os.path.join(kit, 'api', 'include'), '-I', os.path.join(kit, 'framegeneration', 'include'),
                '-DSR_HAVE_FSRFG']
        if '-lwintrust' not in libs: libs += ['-lwintrust', '-lcrypt32']
        print('fidelityfx: FSR 3 frame generation compiled in', file=sys.stderr)
    srcs.append(os.path.join(out, 'modules.c'))
    left = dict(prof['exclude'])
    print(f'profile {a.profile}; left out ({len(left)}{"" if prof["vk"] else " + vk_*"}): '
          + ', '.join(f'{k} [{v}]' for k, v in sorted(left.items())), file=sys.stderr)
    clangxx = os.path.join(os.path.dirname(clang), 'clang++.exe' if clang.endswith('.exe') else 'clang++')

    def cc(src):
        name = os.path.relpath(src, RT).replace(os.sep, '_').replace('/', '_') if src.startswith(RT) else 'modules.c'
        obj = os.path.join(out, 'rtobj', os.path.splitext(name)[0] + '.o')
        cpp = src.endswith('.cpp')
        r = subprocess.run([clangxx if cpp else clang, *(CXX_FLAGS if cpp else RT_FLAGS), *inc, '-c', src, '-o', obj],
                           capture_output=True, text=True, errors='replace')
        return src, obj, r.returncode, r.stderr

    errs = []
    objs = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for src, obj, rc, err in ex.map(cc, srcs):
            if rc:
                errs.append((os.path.basename(src), err.strip().splitlines()[0] if err.strip() else 'error'))
            else:
                objs.append(obj)
    # the application manifest (runtime/sr.rc: UTF-8 code page, per-monitor DPI awareness v2; --probe-dpi checks it)
    windres = os.path.join(os.path.dirname(clang), 'x86_64-w64-mingw32-windres.exe')
    if os.path.isfile(windres):
        res = os.path.join(out, 'rtobj', 'sr_rc.o')
        r = subprocess.run([windres, '-I', RT, os.path.join(RT, 'sr.rc'), '-O', 'coff', '-o', res], capture_output=True,
                           text=True, errors='replace')
        if r.returncode:
            errs.append(('sr.rc', (r.stderr.strip().splitlines() or ['windres failed'])[0]))
        else:
            objs.append(res)
        # sr-recomp: the game's icon as icon resource 1 (gfx_window_win.c loads it for the window), from the dump; the
        # .ico and its script stay in the build folder
        ico = os.path.join(out, 'rtobj', 'sr_icon.ico')
        try:
            have_icon = game_icon(nsp, ico)
        except Exception as e:   # a dump the reader does not know: the build goes on without an icon
            print(f'game icon: not read ({e})', file=sys.stderr)
            have_icon = False
        if have_icon:
            rc = os.path.join(out, 'rtobj', 'sr_icon.rc')
            with open(rc, 'w') as f:
                f.write('1 ICON "%s"\n' % ico.replace('\\', '/'))
            ires = os.path.join(out, 'rtobj', 'sr_icon_rc.o')
            r = subprocess.run([windres, rc, '-O', 'coff', '-o', ires], capture_output=True, text=True, errors='replace')
            if r.returncode:
                print(f'game icon: windres failed ({(r.stderr.strip().splitlines() or ["?"])[0]})', file=sys.stderr)
            else:
                objs.append(ires)
                print('game icon: linked into sr.exe from the dump', file=sys.stderr)
    else:
        print('windres not found next to clang: sr.exe gets no manifest (no DPI awareness, ANSI code page)',
              file=sys.stderr)
    print(f'runtime: {len(objs)} of {len(srcs)} files compiled', file=sys.stderr)
    for s, e in errs:
        print(f'  FAILED {s}: {e[:200]}', file=sys.stderr)
    if errs:
        return 1

    gen_objs = sorted(os.path.join(a.obj, f) for f in os.listdir(a.obj) if f.endswith('.o'))
    rsp = os.path.join(out, 'link.rsp')
    write_rsp(rsp, objs + gen_objs)
    exe = os.path.join(out, 'sr.exe')
    linker = clangxx if prof['vk'] else clang   # the vulkan profile has C++ (VMA, the folder dialog): libc++, static
    static_cxx = ['-static-libstdc++', '-static-libgcc'] if prof['vk'] else []
    r = subprocess.run([linker, *LINK_FLAGS, *static_cxx, f'@{rsp}', '-o', exe, *libs], capture_output=True, text=True,
                       errors='replace')
    for dll in extra_dlls:
        with open(dll, 'rb') as src, open(os.path.join(out, os.path.basename(dll)), 'wb') as dst:
            dst.write(src.read())
    if a.xlate:   # runtime/core/gfx_shader.c starts <exe dir>/sr-xlate.exe --serve
        xl = os.path.abspath(a.xlate)
        names = [n for n in os.listdir(xl) if os.path.isfile(os.path.join(xl, n))]
        for n in names:
            shutil.copyfile(os.path.join(xl, n), os.path.join(out, n))
        # The translator's identity (in every shader cache key, gfx_shader.c xlate_identity): sha256 over its files.
        # The exe is only the .NET launcher, so its own hash would not change when the translator does.
        h = hashlib.sha256()
        for n in sorted(names):
            if n.endswith('.id'):
                continue
            h.update(n.encode() + b'\0')
            with open(os.path.join(xl, n), 'rb') as f:
                h.update(f.read())
        with open(os.path.join(out, 'sr-xlate.id'), 'w', newline='\n') as f:
            f.write(h.hexdigest() + '\n')
        print(f'xlate: {len(names)} translator files copied next to sr.exe, identity {h.hexdigest()[:16]}', file=sys.stderr)
    if a.fonts:   # nn::pl fonts from the user's firmware (ov_shared_font_dir looks next to the executable)
        src_dir = os.path.join(os.path.abspath(a.fonts), 'shared-fonts')
        names = sorted(n for n in os.listdir(src_dir) if n.endswith('.bfttf'))
        os.makedirs(os.path.join(out, 'shared-fonts'), exist_ok=True)
        for n in names:
            shutil.copyfile(os.path.join(src_dir, n), os.path.join(out, 'shared-fonts', n))
        print(f'fonts: {len(names)} copied to shared-fonts/', file=sys.stderr)
    if sl_dir:   # Streamline loads its plugins from the folder of sr.exe (sl_fg_preinit checks their signer)
        sl_files = ['sl.interposer.dll', 'sl.common.dll', 'sl.dlss_g.dll', 'sl.reflex.dll', 'sl.pcl.dll',
                    'nvngx_dlssg.dll', 'NvLowLatencyVk.dll']
        for n in sl_files:
            shutil.copyfile(os.path.join(sl_dir, 'bin', 'x64', n), os.path.join(out, n))
        print(f'streamline: {len(sl_files)} signed files copied next to sr.exe', file=sys.stderr)
    if fx_dir:   # the FidelityFX loader finds its effect DLLs next to itself (vk_fsrfg.cpp checks their signer)
        fx_files = ['amd_fidelityfx_loader_vk.dll', 'amd_fidelityfx_framegeneration_vk.dll']
        for n in fx_files:
            shutil.copyfile(os.path.join(fx_dir, 'Kits', 'FidelityFX', 'signedbin', n), os.path.join(out, n))
        print(f'fidelityfx: {len(fx_files)} signed files copied next to sr.exe', file=sys.stderr)
    if a.dlss:   # NGX loads the DLSS model from the folder of the executable
        shutil.copyfile(os.path.abspath(a.dlss), os.path.join(out, 'nvngx_dlss.dll'))
        print('dlss: nvngx_dlss.dll copied next to sr.exe', file=sys.stderr)
    undef = collections.Counter()
    for ln in r.stderr.splitlines():
        m = re.search(r'undefined symbol: (\S+)', ln)
        if m:
            undef[m.group(1)] += 1
    if r.returncode:
        print(f'link failed: {len(undef)} undefined symbols' if undef else 'link failed:', file=sys.stderr)
        for s, n in sorted(undef.items()):
            print(f'  {s}', file=sys.stderr)
        if not undef:
            print(r.stderr[-3000:], file=sys.stderr)
        return 1
    print(f'linked {exe} ({os.path.getsize(exe) / 2**20:.0f} MB)', file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
