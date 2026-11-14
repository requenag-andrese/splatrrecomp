"""The tools the builder needs besides this repository (tools.json): which are there, and getting the ones that are not.

Each tool comes from its official source, is checked against the size and SHA-256 the manifest pins, and is unpacked
under the toolchains folder where layout.py looks for it. A tool is there when its probe path exists, so a workspace
set up by hand needs nothing. Downloads go to a folder of their own and stay there: a file already present with the
right hash is not downloaded again.

    python -m builder tools --list                    what is there and what a fetch would download
    python -m builder tools [--only id,id]            download and unpack what is missing
    python -m builder --fetch-tools ...               a build that gets what is missing in its check step

The pinned clones of the vendor folder come the same way (fetch_vendor), and two things are made here: the Vulkan
import library (make_implib) and the shader translator (build_xlate). The embedded Python with its
wheels is builder/package.py's.
"""
import hashlib
import json
import os
import shutil
import subprocess
import tarfile
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

from .runner import BuildError

MANIFEST = Path(__file__).resolve().parent / 'tools.json'
CHUNK = 1 << 20
AGENT = {'User-Agent': 'sr-builder'}


def load():
    return json.loads(MANIFEST.read_text(encoding='utf-8'))


def present(tool, toolchains):
    return (Path(toolchains) / tool['probe']).exists()


def status(toolchains, vendor=None):
    """Every tool of the manifest with 'present', then what is made here (the import library, the shader translator)
    and, with vendor, the pinned clones."""
    m = load()
    out = [dict(id=t['id'], version=t['version'], needed=t['needed'], size=t['size'] + sum(w['size'] for w in t.get('with', [])),
                present=present(t, toolchains)) for t in m['tools']]
    out.append(dict(id='vulkan-implib', version='made here', needed=True, size=0,
                    present=(Path(toolchains) / 'vulkan-implib' / 'libvulkan-1.a').is_file()))
    if vendor is not None:
        for v in m['vendor']['repos']:
            out.append(dict(id=v['id'], version=v['commit'][:12], needed=True, size=v['size'],
                            present=(Path(vendor) / v['probe']).exists()))
        out.append(dict(id='sr-xlate', version='made here', needed=True, size=0,
                        present=(Path(toolchains) / 'sr_xlate' / 'sr-xlate.exe').is_file()))
    return out


def tree_hash(root):
    """One hash for a folder's files: every relative path with its file's SHA-256, in path order. What a vendor clone
    is checked by: the bytes of an archive GitHub makes for a commit may change, its files do not."""
    h = hashlib.sha256()
    root = Path(root)
    for p in sorted((q for q in root.rglob('*') if q.is_file()), key=lambda q: q.relative_to(root).as_posix()):
        h.update(p.relative_to(root).as_posix().encode() + b'\0' + hashlib.sha256(p.read_bytes()).digest())
    return h.hexdigest()


def apply_patch(root, text):
    """A unified diff applied to the files under root, exactly: every context and removed line must be where the hunk
    says (no fuzz, no offset), or nothing of that file is written. Covers what the repository's patches hold: changed
    files and new ones. Returns the paths written. git is not needed for it."""
    root = Path(root)
    lines = text.splitlines()
    written, i = [], 0
    while i < len(lines):
        if not (lines[i].startswith('--- ') and i + 1 < len(lines) and lines[i + 1].startswith('+++ ')):
            i += 1
            continue
        old_name, new_name = lines[i][4:].split('\t')[0].strip(), lines[i + 1][4:].split('\t')[0].strip()
        i += 2
        if new_name == '/dev/null':
            raise BuildError('tools.patch_failed', file=old_name, detail='a removed file is not handled')
        rel = new_name[2:] if new_name[:2] in ('a/', 'b/') else new_name
        target = _safe(root, rel)
        fresh = old_name == '/dev/null'
        if fresh:
            old, eol = [], '\n'
        else:
            if not target.is_file():
                raise BuildError('tools.patch_failed', file=rel, detail='the file is not there')
            raw = target.read_bytes().decode('utf-8')
            eol = '\r\n' if '\r\n' in raw else '\n'
            old = raw.split(eol)
            if old and old[-1] == '':
                old.pop()
        out, pos = [], 0
        while i < len(lines) and lines[i].startswith('@@'):
            head = lines[i].split('@@')[1].split()
            start = int(head[0][1:].split(',')[0])
            i += 1
            body = []
            while i < len(lines) and not lines[i].startswith(('@@', '--- ', 'diff ')):
                line = lines[i]
                if line.startswith('\\'):      # "\ No newline at end of file"
                    i += 1
                    continue
                if line and line[0] not in ' +-':   # text after the last hunk (a mail footer)
                    break
                body.append((line[0], line[1:]) if line else (' ', ''))
                i += 1
            while body and body[-1] == (' ', '') and i >= len(lines):   # blank lines closing the patch file
                body.pop()
            # the hunk's old lines, where the header says or the nearest place after the last hunk (the header's line
            # may be a little off, as git apply and patch also allow); the lines themselves must be exact
            block = [text for kind, text in body if kind in ' -']
            want = start - 1 if start else 0
            at = None
            for delta in range(len(old) + 1):
                for k in (want + delta, want - delta):
                    if pos <= k <= len(old) - len(block) and old[k:k + len(block)] == block:
                        at = k
                        break
                if at is not None:
                    break
            if at is None:
                raise BuildError('tools.patch_failed', file=rel, detail=f'the lines the patch expects near line {start} are not there')
            out += old[pos:at]
            out += [text for kind, text in body if kind in ' +']
            pos = at + len(block)
            if i < len(lines) and not lines[i].startswith(('@@', '--- ', 'diff ')):
                break
        out += old[pos:]
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes((eol.join(out) + eol).encode('utf-8'))
        written.append(rel)
    return written


def fetch_vendor(v, downloads, vendor, repo, progress=None):
    """A pinned clone as the archive GitHub makes of its commit: unpacked, checked by its files (tree_hash), patched
    with the repository's patches, then moved to vendor/<id>."""
    vendor = Path(vendor)
    path = Path(downloads) / v['file']
    known = path.is_file() and path.stat().st_size == v['size'] and sha256_of(path) == v['sha256']
    if not known:   # downloaded without the archive's own hash as the test: the files decide below
        path.parent.mkdir(parents=True, exist_ok=True)
        part = path.with_name(path.name + '.part')
        try:
            req = urllib.request.Request(v['url'], headers=AGENT)
            done = 0
            with urllib.request.urlopen(req, timeout=60) as r, open(part, 'wb') as f:
                for block in iter(lambda: r.read(CHUNK), b''):
                    f.write(block)
                    done += len(block)
                    if progress:
                        progress(done, v['size'])
        except (urllib.error.URLError, OSError, TimeoutError) as e:
            part.unlink(missing_ok=True)
            raise BuildError('tools.download_failed', file=v['file'], url=v['url'], detail=str(getattr(e, 'reason', e))) from None
        os.replace(part, path)
    stage = vendor / f'.incoming-{v["id"]}'
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    try:
        with zipfile.ZipFile(path) as z:
            for i in z.infolist():
                _safe(stage, i.filename)
                z.extract(i, stage)
        tops = [p for p in stage.iterdir()]
        if len(tops) != 1 or not tops[0].is_dir():
            raise BuildError('tools.unpack_failed', file=v['file'], detail='not one top folder inside')
        src = tops[0]
        if tree_hash(src) != v['tree']:
            path.unlink(missing_ok=True)
            raise BuildError('tools.hash', file=v['file'], url=v['url'])
        for p in v.get('patches', []):
            apply_patch(src / p['at'], (Path(repo) / p['file']).read_text(encoding='utf-8'))
        dest = vendor / v['id']
        if dest.exists():
            shutil.rmtree(dest)
        shutil.move(str(src), str(dest))
    except (OSError, zipfile.BadZipFile) as e:
        raise BuildError('tools.unpack_failed', file=v['file'], detail=str(e)) from None
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def build_xlate(toolchains, vendor, repo, downloads, log=None):
    """The shader translator: tools/sr_xlate built with the toolchains' .NET against the patched nx2android clone, into
    toolchains/sr_xlate (README "Shader translator"). NuGet brings the packages Ryujinx's projects name, into the
    downloads folder."""
    tc = Path(toolchains)
    dotnet = tc / 'dotnet' / 'dotnet.exe'
    ryu = Path(vendor) / 'nx2android-pr88' / 'nxnative' / 'dm' / 'third_party' / 'ryujinx'
    for what, p in (('.NET', dotnet), ('vendor: nx2android PR #88', ryu)):
        if not p.exists():
            raise BuildError('check.tool_missing', what=what, path=str(p))
    out = tc / 'sr_xlate'
    stage = tc / '.incoming-sr_xlate'
    if stage.exists():
        shutil.rmtree(stage)
    env = dict(os.environ, DOTNET_ROOT=str(dotnet.parent), DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1',
               DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1', NUGET_PACKAGES=str(Path(downloads) / 'nuget-packages'),
               MSBUILDDISABLENODEREUSE='1')
    cmd = [str(dotnet), 'build', '-c', 'Release', str(Path(repo) / 'tools' / 'sr_xlate' / 'sr_xlate.csproj'),
           '-o', str(stage), f'-p:RyuSrc={ryu}', '-nologo', '-v', 'q',
           # no compiler or MSBuild server left running: they kept the tools folder open after the build
           '--disable-build-servers', '-p:UseSharedCompilation=false']
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, encoding='utf-8', errors='replace')
    if log:
        log('sr-xlate build: ' + ' '.join(cmd) + f'\nexit {r.returncode}\n' + (r.stdout or '')[-3000:] + (r.stderr or '')[-1000:])
    if r.returncode or not (stage / 'sr-xlate.exe').is_file():
        shutil.rmtree(stage, ignore_errors=True)
        tail = ' '.join(((r.stdout or '') + (r.stderr or '')).split())[-300:]
        raise BuildError('tool.failed', tool='dotnet build (sr-xlate)', code=r.returncode, log=tail)
    if out.exists():
        shutil.rmtree(out)
    os.replace(stage, out)


def sha256_of(path, progress=None):
    h, done, total = hashlib.sha256(), 0, path.stat().st_size
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(CHUNK), b''):
            h.update(block)
            done += len(block)
            if progress:
                progress(done, total)
    return h.hexdigest()


def fetch(item, downloads, progress=None, check=None):
    """item's file in downloads, checked: the one already there when its hash is right, else downloaded. A file whose
    size or SHA-256 is not the manifest's is removed and never used."""
    path = Path(downloads) / item['file']
    if path.is_file() and path.stat().st_size == item['size'] and sha256_of(path, check) == item['sha256']:
        return path
    path.parent.mkdir(parents=True, exist_ok=True)
    part = path.with_name(path.name + '.part')
    h, done = hashlib.sha256(), 0
    try:
        req = urllib.request.Request(item['url'], headers=AGENT)
        with urllib.request.urlopen(req, timeout=60) as r, open(part, 'wb') as f:
            for block in iter(lambda: r.read(CHUNK), b''):
                f.write(block)
                h.update(block)
                done += len(block)
                if progress:
                    progress(done, item['size'])
    except (urllib.error.URLError, OSError, TimeoutError) as e:
        part.unlink(missing_ok=True)
        raise BuildError('tools.download_failed', file=item['file'], url=item['url'], detail=str(getattr(e, 'reason', e))) from None
    if done != item['size'] or h.hexdigest() != item['sha256']:
        part.unlink(missing_ok=True)
        raise BuildError('tools.hash', file=item['file'], url=item['url'])
    os.replace(part, path)
    return path


def _safe(base, name):
    """base/name, refused when the archive's name would leave base."""
    target = (base / name).resolve()
    if base.resolve() not in target.parents and target != base.resolve():
        raise BuildError('tools.unpack_failed', file=name, detail='a path outside the folder')
    return target


def _move_into(src, dst):
    """src's entries into dst (made if missing); an entry already there is replaced."""
    dst.mkdir(parents=True, exist_ok=True)
    for entry in src.iterdir():
        target = dst / entry.name
        if target.is_dir():
            shutil.rmtree(target)
        elif target.exists():
            target.unlink()
        shutil.move(str(entry), str(target))


def unpack(tool, path, toolchains, progress=None):
    """The downloaded file into its place under toolchains, through a folder of its own so a stop midway leaves no
    half tool where the probe looks."""
    tc = Path(toolchains)
    dest = tc if tool['to'] == '.' else tc / tool['to']
    stage = tc / f'.incoming-{tool["id"]}'
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    try:
        kind = tool['kind']
        if kind == 'file':
            shutil.copyfile(path, stage / tool['file'])
        elif kind == 'zip':
            with zipfile.ZipFile(path) as z:
                infos = z.infolist()
                total, done = sum(i.file_size for i in infos), 0
                for i in infos:
                    _safe(stage, i.filename)
                    z.extract(i, stage)
                    done += i.file_size
                    if progress:
                        progress(done, total)
        elif kind == 'tar.zst':
            try:
                import zstandard
            except ImportError:
                raise BuildError('tools.unpack_failed', file=tool['file'], detail='the zstandard package is missing') from None
            with open(path, 'rb') as f, zstandard.ZstdDecompressor().stream_reader(f) as zr, \
                    tarfile.open(fileobj=zr, mode='r|') as tar:
                for m in tar:
                    if m.name.startswith('.') or not (m.isfile() or m.isdir()):
                        continue   # the package's own records, and links
                    target = _safe(stage, m.name)
                    if m.isdir():
                        target.mkdir(parents=True, exist_ok=True)
                        continue
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with tar.extractfile(m) as src, open(target, 'wb') as out:
                        shutil.copyfileobj(src, out)
        else:
            raise BuildError('tools.unpack_failed', file=tool['file'], detail=f'unknown kind {kind}')
        src = stage / tool['strip'] if tool.get('strip') else stage
        if not src.is_dir():
            raise BuildError('tools.unpack_failed', file=tool['file'], detail=f'no folder {tool.get("strip")} inside')
        _move_into(src, dest)
    except (OSError, zipfile.BadZipFile, tarfile.TarError) as e:
        raise BuildError('tools.unpack_failed', file=tool['file'], detail=str(e)) from None
    finally:
        shutil.rmtree(stage, ignore_errors=True)


def make_implib(toolchains, clang, log=None):
    """vulkan-implib/libvulkan-1.a from the exports of the system's vulkan-1.dll, with llvm-mingw's own tools
    (THIRD_PARTY.md: no file is downloaded for it)."""
    out = Path(toolchains) / 'vulkan-implib'
    dll = Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'vulkan-1.dll'
    binp = Path(clang).parent
    readobj, dlltool = binp / 'llvm-readobj.exe', binp / 'llvm-dlltool.exe'
    if not dll.is_file():
        raise BuildError('tools.no_vulkan', path=str(dll))
    for tool in (readobj, dlltool):
        if not tool.is_file():
            raise BuildError('check.tool_missing', what=tool.name, path=str(tool))
    r = subprocess.run([str(readobj), '--coff-exports', str(dll)], capture_output=True, text=True)
    names = sorted({line.split(':', 1)[1].strip() for line in r.stdout.splitlines() if line.strip().startswith('Name:')} - {''})
    if r.returncode or not names:
        raise BuildError('tool.failed', tool=readobj.name, code=r.returncode, log=(r.stderr or 'no exports listed').strip()[:300])
    out.mkdir(parents=True, exist_ok=True)
    (out / 'names.txt').write_text('\n'.join(names) + '\n', encoding='ascii')
    (out / 'vulkan-1.def').write_text('LIBRARY vulkan-1.dll\nEXPORTS\n' + '\n'.join(names) + '\n', encoding='ascii')
    r = subprocess.run([str(dlltool), '-m', 'i386:x86-64', '-d', str(out / 'vulkan-1.def'), '-D', 'vulkan-1.dll',
                        '-l', str(out / 'libvulkan-1.a')], capture_output=True, text=True)
    if r.returncode or not (out / 'libvulkan-1.a').is_file():
        raise BuildError('tool.failed', tool=dlltool.name, code=r.returncode, log=(r.stderr or '').strip()[:300])
    if log:
        log(f'vulkan-implib: {len(names)} exports of {dll}')


def ensure(toolchains, downloads, reporter, step='check', only=None, optional=True, vendor=None, repo=None):
    """Download and unpack every manifest tool that is not there (only: those ids; optional=False: the needed ones).
    With vendor: the pinned clones too, and the shader translator built from them. Progress goes out as events of
    step: download, verify, unpack, implib and xlate, with the tool's name as detail."""
    repo = Path(repo) if repo else MANIFEST.parent.parent
    tc = Path(toolchains)
    tc.mkdir(parents=True, exist_ok=True)
    got = []
    for tool in load()['tools']:
        if only and tool['id'] not in only:
            continue
        if not only and not optional and not tool['needed']:
            continue
        if present(tool, tc):
            continue
        name = f'{tool["id"]} {tool["version"].split(",")[0].split(" (")[0]}'
        reporter.log(f'tools: getting {tool["id"]} from {tool["url"]}')
        path = fetch(tool, downloads, lambda d, t: reporter.progress(step, 'download', d, t, 'bytes', detail=name),
                     lambda d, t: reporter.progress(step, 'verify', d, t, 'bytes', detail=name))
        extra = [fetch(w, downloads) for w in tool.get('with', [])]
        unpack(tool, path, tc, lambda d, t: reporter.progress(step, 'unpack', d, t, 'bytes', detail=name))
        for w, p in zip(tool.get('with', []), extra):
            shutil.copyfile(p, tc / tool['to'] / w['file'])
        if not present(tool, tc):
            raise BuildError('tools.unpack_failed', file=tool['file'], detail=f'{tool["probe"]} is not there after unpacking')
        got.append(tool['id'])
    lib = tc / 'vulkan-implib' / 'libvulkan-1.a'
    if not lib.is_file() and (not only or 'vulkan-implib' in only):
        clangs = sorted(tc.glob('llvm-mingw-*/bin/clang.exe'))
        if clangs:
            reporter.progress(step, 'implib', force=True)
            make_implib(tc, clangs[-1], reporter.log)
            got.append('vulkan-implib')
    if vendor is not None:
        for v in load()['vendor']['repos']:
            if (only and v['id'] not in only) or (Path(vendor) / v['probe']).exists():
                continue
            name = f'{v["id"]} {v["commit"][:12]}'
            reporter.log(f'tools: getting {v["id"]} from {v["url"]}')
            reporter.progress(step, 'download', 0, v['size'], 'bytes', detail=name, force=True)
            fetch_vendor(v, downloads, vendor, repo, lambda d, t: reporter.progress(step, 'download', d, t, 'bytes', detail=name))
            got.append(v['id'])
        if not (tc / 'sr_xlate' / 'sr-xlate.exe').is_file() and (not only or 'sr-xlate' in only):
            reporter.progress(step, 'xlate', force=True)
            build_xlate(tc, vendor, repo, downloads, reporter.log)
            got.append('sr-xlate')
    return got
