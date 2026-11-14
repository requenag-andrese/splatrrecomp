"""The builder as one folder that needs nothing installed (phase 3, step 2).

    python -m builder package --out DIR [--downloads DIR] [--no-window]

    DIR/
      SrBuilder.exe      the window, published with its own .NET inside
      app/               this repository's sources the builder runs: builder/, tools/, srrecomp/, runtime/ (sr.exe is
                         compiled on the player's PC, so the runtime's sources travel with it), and RELEASE
      README.txt         what it is and where its licences are
      python/            Python (tools.json "python", the embeddable build) with the wheels of "wheels" unpacked in
                         Lib/site-packages; python312._pth names them and ../app
      .toolchains/ vendor/ out/    made on the first build by the tools step (tools.py), not by this command

The folder is a workspace as layout.py understands one: app/ is the repository, its parent the workspace. No game
file, key or firmware is ever read here, and nothing of NVIDIA's is put in: those come from their source on the
player's PC. Building a package needs the workspace's .NET SDK (the window) and git (the list of files).
"""
import json
import os
import shutil
import subprocess
import time
import zipfile
from pathlib import Path

from . import tools as toolbox
from .runner import BuildError

MARK = 'sr-builder-package.json'
PARTS = ('builder', 'tools', 'srrecomp', 'runtime', 'requirements.txt', 'README.md', 'THIRD_PARTY.md', 'LICENSE')
LEFT_OUT = ('builder/window/',)          # the window's sources: the package has it built
KEPT = ('builder/window/fonts/OFL-',)    # ...but the licence texts of the fonts inside SrBuilder.exe go with it (OFL)
NOTE = '''SR Builder

Open SrBuilder.exe. It needs nothing installed: the first build downloads its tools into this folder.

Licence: GNU General Public License, version 3 (app/LICENSE). The sources are in app/.
Third-party parts and their licences: app/THIRD_PARTY.md.
The fonts inside SrBuilder.exe (Bowlby One, Barlow Condensed) are under the SIL Open Font License 1.1:
app/builder/window/fonts/OFL-*.txt.
Python (python/LICENSE.txt) and its packages (python/Lib/site-packages/*.dist-info) carry their own.
Nothing of the game, of console keys, of firmware or of NVIDIA is in this folder.
'''
PTH = 'python312.zip\n.\nLib\\site-packages\n..\\app\nimport site\n'


def tracked(repo):
    """The repository's files that go into app/, as git lists them: the tracked ones and the new ones not yet added,
    never what .gitignore names (build output, caches, game data)."""
    r = subprocess.run(['git', '-C', str(repo), 'ls-files', '-z', '--cached', '--others', '--exclude-standard', '--', *PARTS],
                       capture_output=True)
    if r.returncode:
        raise BuildError('package.no_git', detail=r.stderr.decode(errors='replace').strip()[:200])
    names = [n for n in r.stdout.decode('utf-8').split('\0') if n]
    return wanted(names)


def wanted(names):
    """Of the listed files, the ones the package carries: everything but the window's sources, whose font licence
    texts stay."""
    return [n for n in names if not n.startswith(LEFT_OUT) or n.startswith(KEPT)]


def copy_app(repo, out, release):
    app = out / 'app'
    if app.exists():
        shutil.rmtree(app)
    names = tracked(repo)
    for n in names:
        dst = app / n
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(repo / n, dst)
    (app / 'RELEASE').write_text(release + '\n', encoding='ascii')
    return len(names)


def make_python(out, downloads, progress=None):
    """python/: the embeddable Python and the wheels, each checked against the manifest before it is unpacked."""
    m = toolbox.load()
    py = out / 'python'
    if py.exists():
        shutil.rmtree(py)
    stage = out / '.incoming-python'
    if stage.exists():
        shutil.rmtree(stage)
    stage.mkdir(parents=True)
    try:
        with zipfile.ZipFile(toolbox.fetch(m['python'], downloads, progress)) as z:
            for i in z.infolist():
                toolbox._safe(stage, i.filename)
                z.extract(i, stage)
        site = stage / 'Lib' / 'site-packages'
        site.mkdir(parents=True)
        for w in m['wheels']['files']:
            with zipfile.ZipFile(toolbox.fetch(w, downloads, progress)) as z:
                for i in z.infolist():
                    toolbox._safe(site, i.filename)
                    z.extract(i, site)
        pth = [p for p in stage.glob('python*._pth')]
        if len(pth) != 1:
            raise BuildError('tools.unpack_failed', file=m['python']['file'], detail='no python*._pth inside')
        pth[0].write_text(PTH, encoding='ascii', newline='\r\n')
        os.replace(stage, py)
    finally:
        shutil.rmtree(stage, ignore_errors=True)
    return len(m['wheels']['files'])


def publish_window(repo, out, dotnet, downloads, log=None):
    """SrBuilder.exe in out/: the window with the .NET runtime inside one file (NuGet brings the runtime packs)."""
    stage = out / '.incoming-window'
    if stage.exists():
        shutil.rmtree(stage)
    env = dict(os.environ, DOTNET_ROOT=str(Path(dotnet).parent), DOTNET_CLI_TELEMETRY_OPTOUT='1', DOTNET_NOLOGO='1',
               DOTNET_SKIP_FIRST_TIME_EXPERIENCE='1', NUGET_PACKAGES=str(Path(downloads) / 'nuget-packages'),
               MSBUILDDISABLENODEREUSE='1')
    cmd = [str(dotnet), 'publish', str(repo / 'builder' / 'window' / 'SrBuilder.csproj'), '-c', 'Release', '-r', 'win-x64',
           '--self-contained', 'true', '-p:PublishSingleFile=true', '-p:IncludeNativeLibrariesForSelfExtract=true',
           '-p:EnableCompressionInSingleFile=true', '-p:DebugType=none', '-o', str(stage), '-nologo', '-v', 'q',
           '--disable-build-servers', '-p:UseSharedCompilation=false']
    r = subprocess.run(cmd, capture_output=True, text=True, env=env, encoding='utf-8', errors='replace')
    if log:
        log(' '.join(cmd) + f'\nexit {r.returncode}\n' + (r.stdout or '')[-3000:] + (r.stderr or '')[-1000:])
    exe = stage / 'SrBuilder.exe'
    if r.returncode or not exe.is_file():
        shutil.rmtree(stage, ignore_errors=True)
        tail = ' '.join(((r.stdout or '') + (r.stderr or '')).split())[-400:]
        raise BuildError('tool.failed', tool='dotnet publish (the window)', code=r.returncode, log=tail)
    for p in stage.iterdir():
        dst = out / p.name
        if dst.is_dir():
            shutil.rmtree(dst)
        elif dst.exists():
            dst.unlink()
        shutil.move(str(p), str(dst))
    shutil.rmtree(stage, ignore_errors=True)
    return (out / 'SrBuilder.exe').stat().st_size


def make(layout, out, reporter, window=True):
    """The whole package into out (an empty folder, or an earlier package: its app/, python/ and window are replaced,
    its tools and downloads kept)."""
    out = Path(out).resolve()
    if out.exists() and any(out.iterdir()) and not (out / MARK).is_file():
        raise BuildError('package.not_empty', path=str(out))
    if layout.repo in out.parents or out == layout.repo:
        raise BuildError('package.inside_repo', path=str(out))
    out.mkdir(parents=True, exist_ok=True)
    release = layout.release
    reporter.progress('package', 'app', force=True)
    files = copy_app(layout.repo, out, release)
    reporter.progress('package', 'python', force=True)
    wheels = make_python(out, layout.downloads, lambda d, t: reporter.progress('package', 'python', d, t, 'bytes'))
    size = 0
    if window:
        dotnet = layout.dotnet / 'dotnet.exe'
        if not dotnet.is_file():
            raise BuildError('check.tool_missing', what='.NET', path=str(dotnet))
        reporter.progress('package', 'window', force=True)
        size = publish_window(layout.repo, out, dotnet, layout.downloads, reporter.log)
    (out / 'README.txt').write_text(NOTE, encoding='utf-8', newline='\r\n')
    (out / MARK).write_text(json.dumps({'format': 1, 'release': release, 'made': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                                        'files': files, 'wheels': wheels, 'window': bool(window)}, indent=1) + '\n', encoding='utf-8')
    return dict(out=str(out), files=files, wheels=wheels, window_bytes=size, release=release)
