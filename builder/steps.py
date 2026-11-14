"""The builder's steps, from the player's game file to a portable game folder (docs: README "Builder").

    check     always: the tools, the game file (plaintext NSP of the expected title and version), the keys and firmware
              when the fonts must be made, the disks' free space, a Vulkan 1.3 loader, the game folder
    romfs     the NSP's RomFS level copied to <dest>/romfs/romfs.bin (the game reads it there; the NSP can go)
    exefs     the program's code and metadata to <dest>/exefs (tools/extract_exefs.py)
    lift      the code to C in <temp>/gen (tools/gen_c.py, totk-pc-recomp's generator with the srrecomp extension)
    compile   the C to objects in <temp>/obj (tools/compile_gen.py)
    fonts     the Switch system fonts from the firmware, with the player's keys (tools/firmware_fonts.py; the only step
              that reads keys, whose values stay in that process); transient: it runs when the link does
    link      sr.exe and the libraries it needs into <dest> (tools/build_runtime.py --romfs-copied)
    dotnet    the .NET runtime the shader translator runs on, to <dest>/dotnet (sr.exe uses it, runtime/sr/xlate_env.c)
    shaders   every shader of the game translated and built ahead of play (tools/prepare_shaders.py), packed
              (tools/pack_shader_cache.py)
    shortcut  the desktop and Start menu shortcuts the player asked for
    cleanup   always: the shader seed files go; the temp folder stays for a quicker rebuild unless --clean
    original  always, acts with --delete-original only: the NSP rebuilt in memory from romfs/romfs.bin and
              original/rest.bin (written by the romfs step in the same pass), checked against its SHA-256, then moved
              to the Recycle Bin; `python -m builder restore` writes it again

Steps that write into the game folder replace files through temporary names, so a cancelled step leaves either the
old file or none. The game folder never holds keys; its game data (romfs, exefs, original, shader cache) is the
player's own.
"""
import os
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

from .runner import BuildError, Step
from . import layout as layouts
from . import tools as toolbox

REPO = Path(__file__).resolve().parent.parent
TITLE_ID = 0x0400380020056000      # the game this port recompiles
VERSION = 65536                     # the version tools/gen_c.py's patches and the runtime expect (build_runtime.py)
GIB = 1 << 30


# ---- helpers

def fingerprint(*parts):
    """SHA-256 over strings and the contents of files and directory trees (a missing path counts as missing).
    Adapted from totk-pc-recomp installer/steps.py (MIT)."""
    import hashlib
    h = hashlib.sha256()
    for p in parts:
        if isinstance(p, Path):
            if p.is_dir():
                for f in sorted(p.rglob('*')):
                    if f.is_file() and '__pycache__' not in f.parts:
                        h.update(f.relative_to(p).as_posix().encode() + b'\0' + hashlib.sha256(f.read_bytes()).digest())
            elif p.is_file():
                h.update(p.name.encode() + b'\0' + hashlib.sha256(p.read_bytes()).digest())
            else:
                h.update(b'missing\0' + p.name.encode())
        else:
            h.update(str(p).encode())
        h.update(b'\1')
    return h.hexdigest()


def git_head(path):
    try:
        return subprocess.run(['git', '-C', str(path), 'rev-parse', 'HEAD'], capture_output=True, text=True,
                              timeout=20).stdout.strip() or 'none'
    except (OSError, subprocess.SubprocessError):
        return 'none'


def remove(path):
    if path.is_dir() and not path.is_symlink():
        shutil.rmtree(path)
    elif path.exists() or path.is_symlink():
        path.unlink()


def kill_tree(proc):
    if proc.poll() is None:
        subprocess.run(['taskkill', '/T', '/F', '/PID', str(proc.pid)], capture_output=True)


def run_tool(ctx, step, script, args, parse=None, cwd=None, env=None):
    """Run a repository tool with the workspace's Python. Its output goes to <temp>/logs/<step>-<script>.log and, line
    by line, to parse(line). Raises BuildError tool.failed when it exits with an error. Adapted from totk-pc-recomp
    installer/steps.py run_tool (MIT)."""
    logs = ctx.temp / 'logs'
    logs.mkdir(parents=True, exist_ok=True)
    log = logs / f'{step}-{Path(script).stem}.log'
    command = [str(ctx.layout.python), '-I', str(ctx.layout.repo / script), *map(str, args)]
    ctx.reporter.log('run ' + ' '.join(command[2:]))
    full_env = dict(os.environ, PYTHONUNBUFFERED='1', PYTHONIOENCODING='utf-8', **(env or {}))
    with open(log, 'w', encoding='utf-8', newline='\n') as out:
        proc = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, cwd=cwd or ctx.layout.repo,
                                env=full_env, text=True, encoding='utf-8', errors='replace', bufsize=1)
        try:
            for line in proc.stdout:
                out.write(line)
                if parse:
                    parse(line.rstrip())
            code = proc.wait()
        except BaseException:
            kill_tree(proc)
            raise
    ctx.reporter.log(f'{Path(script).name}: exit code {code}; output in {log}')
    if code:
        raise BuildError('tool.failed', tool=Path(script).name, code=code, log=str(log))
    return log


def free_bytes(path):
    p = Path(path)
    while not p.exists():
        p = p.parent
    return shutil.disk_usage(p).free


def same_disk(a, b):
    return Path(a).resolve().drive.lower() == Path(b).resolve().drive.lower()


def vulkan_version():
    """(major, minor) of the Vulkan loader's instance version, None when there is no loader."""
    import ctypes
    try:
        vk = ctypes.WinDLL('vulkan-1.dll')
    except OSError:
        return None
    v = ctypes.c_uint32(0)
    try:
        if vk.vkEnumerateInstanceVersion(ctypes.byref(v)) != 0:
            return None
    except AttributeError:
        return (1, 0)
    return (v.value >> 22) & 0x7f, (v.value >> 12) & 0x3ff


def shared_fonts_in(folder):
    return (Path(folder) / 'shared-fonts' / '0-standard.bfttf').is_file()


def firmware_stamp(folder):
    """Names and sizes of the firmware folder's NCAs: what the fonts are made from, without reading them."""
    f = Path(folder)
    return ';'.join(f'{p.name}:{p.stat().st_size}' for p in sorted(f.rglob('*.nca')) if p.is_file())


REST = 'original'   # <dest>/original: what the game folder lacks of the player's NSP (restore)
REST_MAGIC = b'SRRCNSP1'


def copy_romfs(src, offset, size, dst, rest_dir, report):
    """The RomFS span of the NSP src (size bytes from offset) to dst through dst.tmp, and, in the same pass over the
    whole file, its SHA-256 and the bytes outside the span to <rest_dir>/rest.bin: header SRRCNSP1, u64 offset, u64
    size, u64 file size, 32-byte SHA-256 of the file, then the bytes before the span and the bytes after it. With
    dst it rebuilds the NSP exactly (restore_nsp). Returns the file's SHA-256 hex. report(done, total) per block."""
    import hashlib
    import struct
    total = Path(src).stat().st_size
    tmp = dst.with_name(dst.name + '.tmp')
    dst.parent.mkdir(parents=True, exist_ok=True)
    rest_dir.mkdir(parents=True, exist_ok=True)
    rest_tmp = rest_dir / 'rest.bin.tmp'
    h = hashlib.sha256()
    buf = bytearray(16 << 20)
    view = memoryview(buf)
    done = 0
    with open(src, 'rb', buffering=0) as fi, open(tmp, 'wb', buffering=0) as fo, open(rest_tmp, 'wb') as fr:
        fr.write(REST_MAGIC + struct.pack('<QQQ', offset, size, total) + bytes(32))
        for part, length, out in (('before', offset, fr), ('span', size, fo), ('after', total - offset - size, fr)):
            left = length
            while left:
                n = fi.readinto(view[:min(len(buf), left)])
                if not n:
                    raise BuildError('romfs.short_read', path=str(src))
                h.update(view[:n])
                out.write(view[:n])
                left -= n
                done += n
                report(done, total)
        digest = h.digest()
        fr.seek(len(REST_MAGIC) + 24)
        fr.write(digest)
    os.replace(tmp, dst)
    os.replace(rest_tmp, rest_dir / 'rest.bin')
    return digest.hex()


def restore_nsp(dest, out=None, report=None):
    """The player's NSP again from <dest>/romfs/romfs.bin and <dest>/original/rest.bin, checked against the SHA-256
    recorded when it was split. With out None it only checks (nothing is written). Returns the SHA-256 hex; raises
    BuildError restore.* when a part is missing or the result differs."""
    import hashlib
    import struct
    dest = Path(dest)
    rest, romfs = dest / REST / 'rest.bin', dest / 'romfs' / 'romfs.bin'
    if not rest.is_file() or not romfs.is_file():
        raise BuildError('restore.missing', path=str(dest))
    h = hashlib.sha256()
    buf = bytearray(16 << 20)
    view = memoryview(buf)
    with open(rest, 'rb') as fr:
        head = fr.read(len(REST_MAGIC) + 24 + 32)
        if head[:8] != REST_MAGIC:
            raise BuildError('restore.missing', path=str(rest))
        offset, size, total = struct.unpack_from('<QQQ', head, 8)
        want = head[32:64]
        if romfs.stat().st_size != size:
            raise BuildError('restore.mismatch', path=str(dest))
        tmp = Path(str(out) + '.tmp') if out else None
        fo = open(tmp, 'wb', buffering=0) if tmp else None
        done = 0
        try:
            with open(romfs, 'rb', buffering=0) as fs:
                for src, length in ((fr, offset), (fs, size), (fr, total - offset - size)):
                    left = length
                    while left:
                        n = src.readinto(view[:min(len(buf), left)])
                        if not n:
                            raise BuildError('restore.mismatch', path=str(dest))
                        h.update(view[:n])
                        if fo:
                            fo.write(view[:n])
                        left -= n
                        done += n
                        if report:
                            report(done, total)
        finally:
            if fo:
                fo.close()
    if h.digest() != want:
        if tmp:
            tmp.unlink(missing_ok=True)
        raise BuildError('restore.mismatch', path=str(dest))
    if tmp:
        os.replace(tmp, out)
    return want.hex()


def to_recycle_bin(path):
    """path into the Recycle Bin (the player can bring it back); False when Windows would not."""
    ps = ("Add-Type -AssemblyName Microsoft.VisualBasic; [Microsoft.VisualBasic.FileIO.FileSystem]::DeleteFile("
          "$env:SR_DEL, 'OnlyErrorDialogs', 'SendToRecycleBin')")
    r = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', ps],
                       env=dict(os.environ, SR_DEL=str(path)), capture_output=True, text=True, timeout=600)
    return r.returncode == 0 and not Path(path).exists()


# ---- steps

def _pad16(n):
    return (n + 15) & ~15


def copy_nup_romfs(src, dst_dir, report):
    """The files of the NUP's running program into dst_dir/romfs.bin, one after another on 16-byte bounds, each checked
    against the SHA-256 the NUP records for it (zstd files decompressed), and dst_dir/romfs-index.bin: the same index
    layout the runtime reads (SRRCRFS1: offset, size and path of every file), here with offsets into this romfs.bin.
    Returns the size of romfs.bin. report(done, total) as it goes."""
    import hashlib
    import struct
    for d in (str(REPO),):
        if d not in sys.path:
            sys.path.insert(0, d)
    from srrecomp import sources
    nup = sources.Nup(str(src))
    pkg = nup.package(None, 'exefs')
    files = sorted(nup.tree(pkg['romfs']), key=lambda e: e[0].encode())
    total = sum(_pad16(f['size']) for _, f in files)
    dst_dir.mkdir(parents=True, exist_ok=True)
    tmp = dst_dir / 'romfs.bin.tmp'
    rows, strings, done = bytearray(), bytearray(), 0
    try:
        with open(tmp, 'wb', buffering=1 << 22) as out:
            for path, f in files:
                name = path.encode()
                rows += struct.pack('<QQII', done, f['size'], len(strings), len(name) + 1)
                strings += name + b'\0'
                if f['size']:
                    if f['codec'] == 0:          # stored as it is: copied in blocks, hashed on the way
                        h, left = hashlib.sha256(), f['size']
                        nup.fh.seek(f['off'])
                        while left:
                            block = nup.fh.read(min(left, 8 << 20))
                            if not block:
                                raise BuildError('romfs.short_read', path=str(src))
                            h.update(block)
                            out.write(block)
                            left -= len(block)
                        if h.digest() != f['sha256']:
                            raise BuildError('romfs.bad_file', path=str(src), file=path)
                    else:                        # compressed: sources.Nup.load decompresses and checks the hash
                        try:
                            out.write(nup.load(f).read())
                        except ValueError:
                            raise BuildError('romfs.bad_file', path=str(src), file=path) from None
                    out.write(bytes(_pad16(f['size']) - f['size']))
                done += _pad16(f['size'])
                report(done, total)
    finally:
        nup.fh.close()
    index = struct.pack('<8sQQ', b'SRRCRFS1', len(files), 24 + len(rows)) + bytes(rows) + bytes(strings)
    (dst_dir / 'romfs-index.bin').write_bytes(index)
    os.replace(tmp, dst_dir / 'romfs.bin')
    return total


def read_game(game, lay, log=None):
    """The game file, checked: a plaintext NSP or a NUP holding this title at this version. Returns what the steps
    need of it: kind ('nsp' or 'nup'), version (the display text), romfs_offset and romfs_size (the NSP's RomFS span;
    for a NUP, 0 and the size its files take in romfs.bin). A BuildError says what is wrong. The check step and
    `check-game` (cli.py: the window's first screen asks before it lets a build start) share it.

    A NUP may hold a base and updates: the program that runs is its newest update, else its base (sources.Nup.package,
    as the console and the other tools take it), and that one must be the version the port expects."""
    if not game.is_file():
        raise BuildError('check.game_missing', path=str(game))
    if game.suffix.lower() not in ('.nsp', '.nup'):
        raise BuildError('check.game_format', name=game.name)
    for d in (str(lay.repo / 'tools'), str(lay.repo)):
        if d not in sys.path:
            sys.path.insert(0, d)
    if game.suffix.lower() == '.nup':
        try:
            from srrecomp import sources
            nup = sources.Nup(str(game))
            pkg = nup.package(None, 'exefs')
            if pkg['romfs'] == 0xFFFFFFFF:
                raise ValueError('its program has no RomFS')
            text = (nup.meta(pkg).get(0x0004) or b'').decode('utf-8', 'replace')
            size = sum(_pad16(f['size']) for _, f in nup.tree(pkg['romfs']))
            nup.fh.close()
        except Exception as e:
            if log:
                log(f'reading {game}: {type(e).__name__}: {e}')
            raise BuildError('check.game_unreadable', name=game.name, detail=str(e)) from None
        if pkg['title_id'] != TITLE_ID:
            raise BuildError('check.wrong_title', name=game.name, title=f'{pkg["title_id"]:016x}')
        if pkg['version'] != VERSION:
            raise BuildError('check.wrong_version', name=game.name, version=f'{text or "?"} ({pkg["version"]})', expected=str(VERSION))
        return dict(kind='nup', version=text or str(pkg['version']), romfs_offset=0, romfs_size=size)
    try:
        from srrecomp import sources
        prog = sources.open_nsp_program(str(game))
        sl = sources.nca_romfs(prog)
        import build_runtime
        version = build_runtime.display_version(str(game))
    except Exception as e:   # an encrypted or damaged file: the readers refuse it
        if log:
            log(f'reading {game}: {type(e).__name__}: {e}')
        raise BuildError('check.game_unreadable', name=game.name, detail=str(e)) from None
    if getattr(prog, 'program_id', TITLE_ID) != TITLE_ID:
        raise BuildError('check.wrong_title', name=game.name, title=f'{prog.program_id:016x}')
    if str(VERSION) not in str(version):
        raise BuildError('check.wrong_version', name=game.name, version=str(version), expected=str(VERSION))
    return dict(kind='nsp', version=str(version), romfs_offset=sl.off, romfs_size=sl.size)


class Check(Step):
    id = 'check'
    always = True

    def run(self, ctx):
        lay, o, rep = ctx.layout, ctx.options, ctx.reporter
        # the tools of the manifest (tools.json) that are not there: downloaded now with --fetch-tools, else named
        gone = [t for t in toolbox.status(lay.toolchains, lay.vendor) if t['needed'] and not t['present']]
        if gone and getattr(o, 'fetch_tools', False):
            toolbox.ensure(lay.toolchains, lay.downloads, rep, self.id, vendor=lay.vendor, repo=lay.repo)
            lay = ctx.layout = layouts.find(lay.workspace, toolchains=lay.toolchains, vendor=lay.vendor,
                                            temp=lay.temp, downloads=lay.downloads)
        elif gone:
            raise BuildError('check.tool_fetch', what=gone[0]['id'], mb=round(sum(t['size'] for t in gone) / 1e6))
        if lay.missing:
            what, path = lay.missing[0]
            raise BuildError('check.tool_missing', what=what, path=str(path))
        dest = ctx.dest
        if dest.exists() and any(dest.iterdir()) and ctx.state['steps'] == {} and not (dest / 'sr-build.json').exists():
            raise BuildError('check.dest_not_empty', path=str(dest))
        # the game file: a plaintext NSP of this title and version
        game = o.game or (Path(ctx.state['inputs']['game']['path']) if ctx.state['inputs'].get('game') else None)
        if not game:
            raise BuildError('check.need_game')
        game = Path(game).resolve()
        original = game   # where the player's NSP is (or was); game may become a copy rebuilt in the temp folder
        restored_copy = False
        if not game.is_file() and (dest / REST / 'rest.bin').is_file() and not o.game:
            # the original went to the Recycle Bin (--delete-original): rebuilt from the folder into the temp folder
            # for this run (the link and the shaders read the NSP), checked, and removed by the cleanup step
            restored = ctx.temp / 'restored' / game.name
            if not restored.is_file():
                rep.warning('check.restoring', name=game.name)
                restored.parent.mkdir(parents=True, exist_ok=True)
                restore_nsp(dest, restored, lambda d, t: rep.progress(self.id, 'restore', d, t, 'bytes'))
                mt = ctx.state['inputs']['game'].get('mtime_ns')
                if mt:   # the original's time, so the steps' keys (name, size, time) stay what they were
                    os.utime(restored, ns=(mt, mt))
            game, restored_copy = restored, True
        got = read_game(game, lay, rep.log)
        st = game.stat()
        ctx.info.update(game=game, kind=got['kind'], version=got['version'], romfs_offset=got['romfs_offset'],
                        romfs_size=got['romfs_size'], restored=restored_copy,
                        game_id=f'{game.name}|{st.st_size}|{st.st_mtime_ns}|{TITLE_ID:016x}|{VERSION}')
        rec = ctx.state['inputs'].setdefault('game', {})
        rec.update(name=original.name, path=str(original), size=st.st_size, mtime_ns=st.st_mtime_ns)
        # fonts: from the firmware with the keys, unless the folder has them
        have_fonts = shared_fonts_in(dest) or shared_fonts_in(ctx.temp / 'fonts')
        if o.keys or o.firmware:
            if not (o.keys and Path(o.keys).is_file()):
                raise BuildError('check.keys_missing', path=str(o.keys or ''))
            if not (o.firmware and Path(o.firmware).is_dir() and firmware_stamp(o.firmware)):
                raise BuildError('check.firmware_missing', path=str(o.firmware or ''))
        elif not have_fonts:
            raise BuildError('check.need_keys')
        # space: the RomFS copy, the program and its libraries, the shader cache; the temp folder's C and objects
        need = (0 if (dest / 'romfs' / 'romfs.bin').is_file() else got['romfs_size']) + 6 * GIB
        temp_need = 3 * GIB
        if same_disk(dest, ctx.temp):
            need += temp_need
        free = free_bytes(dest)
        rep.emit('space', dest=str(dest), need=need, free=free, temp=str(ctx.temp), temp_need=temp_need,
                 temp_free=free_bytes(ctx.temp), same_disk=same_disk(dest, ctx.temp))
        if free < need:
            raise BuildError('check.no_space', path=str(dest), need_gib=round(need / GIB, 1), free_gib=round(free / GIB, 1))
        if not same_disk(dest, ctx.temp) and free_bytes(ctx.temp) < temp_need:
            raise BuildError('check.no_space', path=str(ctx.temp), need_gib=round(temp_need / GIB, 1),
                             free_gib=round(free_bytes(ctx.temp) / GIB, 1))
        vk = vulkan_version()
        if vk is None or vk < (1, 3):
            rep.warning('check.vulkan', version='none' if vk is None else f'{vk[0]}.{vk[1]}')
        if not lay.dlss.is_file():
            rep.warning('check.no_dlss', path=str(lay.dlss))
        ctx.temp.mkdir(parents=True, exist_ok=True)
        return {}


class Romfs(Step):
    id = 'romfs'

    def key(self, ctx):
        return fingerprint(ctx.info['game_id'], ctx.info['kind'], ctx.info['romfs_offset'], ctx.info['romfs_size'])

    def current(self, ctx, record):
        f = ctx.dest / 'romfs' / 'romfs.bin'
        if not (f.is_file() and f.stat().st_size == ctx.info['romfs_size']):
            return False
        if ctx.info['kind'] == 'nup':   # its own index beside it; nothing to rebuild a NUP from
            return (ctx.dest / 'romfs' / 'romfs-index.bin').is_file()
        return (ctx.dest / REST / 'rest.bin').is_file()

    def run(self, ctx):
        size = ctx.info['romfs_size']
        if ctx.info['kind'] == 'nup':
            remove(ctx.dest / REST)             # an NSP's rest from an earlier build of this folder
            copy_nup_romfs(ctx.info['game'], ctx.dest / 'romfs',
                           lambda done, total: ctx.reporter.progress(self.id, 'copy', done, total, 'bytes'))
            ctx.state['inputs']['game'].pop('sha256', None)
            return {'size': size, 'kind': 'nup'}
        remove(ctx.dest / 'romfs' / 'romfs-index.bin')   # a NUP's index from an earlier build of this folder
        sha = copy_romfs(ctx.info['game'], ctx.info['romfs_offset'], size, ctx.dest / 'romfs' / 'romfs.bin',
                         ctx.dest / REST, lambda done, total: ctx.reporter.progress(self.id, 'copy', done, total, 'bytes'))
        ctx.state['inputs']['game']['sha256'] = sha
        return {'size': size, 'sha256': sha}


class Exefs(Step):
    id = 'exefs'

    def key(self, ctx):
        r = ctx.layout.repo
        return fingerprint(ctx.info['game_id'], r / 'tools' / 'extract_exefs.py', r / 'srrecomp')

    def current(self, ctx, record):
        return (ctx.dest / 'exefs' / 'main.npdm').is_file() and (ctx.dest / 'exefs' / 'main').is_file()

    def run(self, ctx):
        out = ctx.dest / 'exefs'
        remove(out)
        ctx.reporter.progress(self.id, 'extract', force=True)
        run_tool(ctx, self.id, 'tools/extract_exefs.py', [ctx.info['game'], '--out', out])
        return {}


class Lift(Step):
    id = 'lift'

    def key(self, ctx):
        r, v = ctx.layout.repo, ctx.layout.vendor
        return fingerprint(ctx.keys['exefs'], r / 'tools' / 'gen_c.py', r / 'srrecomp',
                           git_head(v / 'totk-pc-recomp'), git_head(v / 'LASRecomp'))

    def current(self, ctx, record):
        return (ctx.temp / 'gen' / 'layout.json').is_file()

    def run(self, ctx):
        out = ctx.temp / 'gen'
        ctx.reporter.progress(self.id, 'lift', force=True)
        run_tool(ctx, self.id, 'tools/gen_c.py', ['--vendor', ctx.layout.vendor, '--', '--exefs', ctx.dest / 'exefs',
                                                  '--out', out, '--modules', 'main,subsdk0,sdk'])
        return {}


class Compile(Step):
    id = 'compile'

    def key(self, ctx):
        return fingerprint(ctx.keys['lift'], ctx.layout.repo / 'tools' / 'compile_gen.py', ctx.layout.clang.parent.parent.name)

    def current(self, ctx, record):
        obj = ctx.temp / 'obj'
        return obj.is_dir() and any(obj.iterdir())

    def run(self, ctx):
        ctx.reporter.progress(self.id, 'compile', force=True)
        run_tool(ctx, self.id, 'tools/compile_gen.py', ['--gen', ctx.temp / 'gen', '--vendor', ctx.layout.vendor,
                                                        '--clang', ctx.layout.clang, '--obj', ctx.temp / 'obj',
                                                        '--out', ctx.temp / 'compile_gen.json'])
        return {}


class Fonts(Step):
    id = 'fonts'
    transient = True   # <temp>/fonts feeds the link only

    def key(self, ctx):
        o = ctx.options
        if o.firmware:
            return fingerprint(firmware_stamp(o.firmware), ctx.layout.repo / 'tools' / 'firmware_fonts.py')
        rec = ctx.record(self.id)   # no firmware this run: the fonts made before stand, under the key they had
        return rec['key'] if rec and (shared_fonts_in(ctx.temp / 'fonts') or shared_fonts_in(ctx.dest)) else 'kept'

    def current(self, ctx, record):
        return shared_fonts_in(ctx.temp / 'fonts')

    def run(self, ctx):
        out = ctx.temp / 'fonts'
        o = ctx.options
        if not (o.keys and o.firmware):   # fonts made before: in the temp folder, else the game folder's own
            if shared_fonts_in(out):
                return {}
            if not shared_fonts_in(ctx.dest):
                raise BuildError('check.need_keys')
            remove(out)
            shutil.copytree(ctx.dest / 'shared-fonts', out / 'shared-fonts')
            return {}
        ctx.reporter.progress(self.id, 'extract', force=True)
        run_tool(ctx, self.id, 'tools/firmware_fonts.py', ['--vendor', ctx.layout.vendor, '--keys', Path(o.keys),
                                                           '--firmware', Path(o.firmware), '--out', out])
        if not shared_fonts_in(out):
            raise BuildError('fonts.none', firmware=str(o.firmware))
        return {}


# build_runtime.py's intermediates: kept in the temp folder between builds, not in the game folder
LINK_SCRATCH = ('rtobj', 'link.rsp', 'modules.c', 'nvn_procs_extra.inc', 'romfs_config.h')


class Link(Step):
    id = 'link'

    def key(self, ctx):
        r, lay = ctx.layout.repo, ctx.layout
        return fingerprint(ctx.keys['compile'], ctx.keys['fonts'], ctx.info['game_id'], r / 'runtime',
                           r / 'tools' / 'build_runtime.py', lay.xlate / 'sr-xlate.id', lay.dlss.name if lay.dlss.is_file() else '')

    def current(self, ctx, record):
        return (ctx.dest / 'sr.exe').is_file()

    def run(self, ctx):
        lay, dest, keep = ctx.layout, ctx.dest, ctx.temp / 'link'
        if (keep / 'rtobj').is_dir() and not (dest / 'rtobj').exists():   # the last build's runtime objects
            shutil.move(str(keep / 'rtobj'), str(dest / 'rtobj'))
        args = ['--gen', ctx.temp / 'gen', '--obj', ctx.temp / 'obj', '--exefs', dest / 'exefs', '--vendor', lay.vendor,
                '--clang', lay.clang, '--out', dest, '--profile', 'vulkan', '--deps', lay.toolchains,
                '--xlate', lay.xlate, '--dotnet', lay.dotnet, '--fonts', ctx.temp / 'fonts',
                '--streamline', lay.streamline, '--fidelityfx', lay.fidelityfx]
        if ctx.info['kind'] == 'nup':   # the index made with the copy; the version as the NUP states it
            args += ['--romfs-indexed', dest / 'romfs', '--display-version', ctx.info['version']]
        else:
            args += ['--nsp', ctx.info['game'], '--romfs-copied', dest / 'romfs']
        if lay.dlss.is_file():
            args += ['--dlss', lay.dlss]
        ctx.reporter.progress(self.id, 'link', force=True)
        run_tool(ctx, self.id, 'tools/build_runtime.py', args)
        keep.mkdir(parents=True, exist_ok=True)
        for name in LINK_SCRATCH:
            if (dest / name).exists():
                remove(keep / name)
                shutil.move(str(dest / name), str(keep / name))
        return {}


class Dotnet(Step):
    id = 'dotnet'

    def key(self, ctx):
        d = ctx.layout.dotnet
        return fingerprint(*sorted(p.name for p in (d / 'shared' / 'Microsoft.NETCore.App').glob('*')),
                           *sorted(p.name for p in (d / 'host' / 'fxr').glob('*')))

    def current(self, ctx, record):
        return (ctx.dest / 'dotnet' / 'dotnet.exe').is_file()

    def run(self, ctx):
        src, out = ctx.layout.dotnet, ctx.dest / 'dotnet'
        tmp = out.with_name('dotnet.tmp')
        remove(tmp)
        tmp.mkdir(parents=True)
        for f in ('dotnet.exe', 'LICENSE.txt', 'ThirdPartyNotices.txt'):
            if (src / f).is_file():
                shutil.copy2(src / f, tmp / f)
        shutil.copytree(src / 'host', tmp / 'host')
        shutil.copytree(src / 'shared' / 'Microsoft.NETCore.App', tmp / 'shared' / 'Microsoft.NETCore.App')
        remove(out)
        os.replace(tmp, out)
        return {}


class Shaders(Step):
    id = 'shaders'

    def key(self, ctx):
        return fingerprint(ctx.keys['link'], ctx.layout.repo / 'tools' / 'prepare_shaders.py',
                           ctx.layout.repo / 'tools' / 'pack_shader_cache.py')

    def current(self, ctx, record):
        return (ctx.dest / 'shader-cache' / 'shader-cache.pack').is_file()

    def run(self, ctx):
        dest = ctx.dest
        ctx.reporter.progress(self.id, 'translate', force=True)
        run_tool(ctx, self.id, 'tools/prepare_shaders.py', [ctx.info['game'], '--vendor', ctx.layout.vendor,
                                                            '--build', dest])
        for f in ('seed-programs.bin', 'seed-answers.bin'):
            remove(dest / f)
        ctx.reporter.progress(self.id, 'pack', force=True)
        run_tool(ctx, self.id, 'tools/pack_shader_cache.py', [dest / 'shader-cache'])
        return {}


def desktop_folder():
    """The desktop as Windows names it (OneDrive may move it), else USERPROFILE/Desktop."""
    try:
        r = subprocess.run(['powershell', '-NoProfile', '-Command', "[Environment]::GetFolderPath('Desktop')"],
                           capture_output=True, text=True, timeout=30)
        if r.stdout.strip():
            return Path(r.stdout.strip())
    except (OSError, subprocess.SubprocessError):
        pass
    return Path(os.environ.get('USERPROFILE', '~')).expanduser() / 'Desktop'


class Shortcut(Step):
    id = 'shortcut'

    def key(self, ctx):
        return fingerprint(str(ctx.dest), ctx.options.desktop_shortcut, ctx.options.start_menu)

    def current(self, ctx, record):
        return all(Path(p).is_file() for p in record.get('paths', []))

    def run(self, ctx):
        made = []
        targets = []
        if ctx.options.desktop_shortcut:
            targets.append(desktop_folder() / 'SR Recomp.lnk')
        if ctx.options.start_menu:
            targets.append(Path(os.environ['APPDATA']) / 'Microsoft' / 'Windows' / 'Start Menu' / 'Programs' / 'SR Recomp.lnk')
        exe = ctx.dest / 'sr.exe'
        for lnk in targets:
            lnk.parent.mkdir(parents=True, exist_ok=True)
            ps = ("$s = (New-Object -ComObject WScript.Shell).CreateShortcut($env:SR_LNK); "
                  "$s.TargetPath = $env:SR_EXE; $s.WorkingDirectory = $env:SR_DIR; $s.IconLocation = $env:SR_EXE + ',0'; "
                  "$s.Description = 'SR Recomp'; $s.Save()")
            env = dict(os.environ, SR_LNK=str(lnk), SR_EXE=str(exe), SR_DIR=str(ctx.dest))
            r = subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-Command', ps], env=env,
                               capture_output=True, text=True, timeout=60)
            if r.returncode or not lnk.is_file():
                ctx.reporter.warning('shortcut.failed', path=str(lnk))
                continue
            made.append(str(lnk))
        return {'paths': made}


class Cleanup(Step):
    id = 'cleanup'
    always = True

    def run(self, ctx):
        for f in ('seed-programs.bin', 'seed-answers.bin'):
            remove(ctx.dest / f)
        remove(ctx.temp / 'restored')   # a copy of the original rebuilt for this run (check step)
        if ctx.options.clean:   # the C, the objects and the runtime objects: a rebuild then starts from the lift
            for name in ('gen', 'obj', 'link', 'fonts'):
                remove(ctx.temp / name)
        return {}


class Original(Step):
    """--delete-original: once everything else is done, the player's NSP rebuilt in memory from the game folder and
    checked against its SHA-256, then moved to the Recycle Bin. `restore` makes it again (cli.py)."""
    id = 'original'
    always = True

    def run(self, ctx):
        if not ctx.options.delete_original:
            return {}
        game = ctx.info['game']
        if ctx.info['kind'] == 'nup':   # its compressed files cannot be made again byte for byte from the folder
            ctx.reporter.warning('original.nup_kept', name=game.name)
            return {}
        want = ctx.state['inputs']['game'].get('sha256')
        if ctx.info.get('restored') or not game.exists():   # gone already: this run used a copy rebuilt in temp
            return {}
        if not want:
            raise BuildError('original.no_hash')
        got = restore_nsp(ctx.dest, None, lambda d, t: ctx.reporter.progress(self.id, 'verify', d, t, 'bytes'))
        if got != want:
            raise BuildError('restore.mismatch', path=str(ctx.dest))
        st = game.stat()
        if f'{game.name}|{st.st_size}|{st.st_mtime_ns}' not in ctx.info['game_id']:
            raise BuildError('original.changed', path=str(game))
        ctx.reporter.progress(self.id, 'recycle', force=True)
        if not to_recycle_bin(game):
            raise BuildError('original.not_recycled', path=str(game))
        ctx.state['inputs']['game']['recycled'] = True
        ctx.save()
        return {}


def all_steps():
    return [Check(), Romfs(), Exefs(), Lift(), Compile(), Fonts(), Link(), Dotnet(), Shaders(), Shortcut(), Cleanup(),
            Original()]
