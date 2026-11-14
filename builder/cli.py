"""SR Builder, command line: the player's game file to a portable game folder (the window, phase 2, runs this).

    python -m builder --game FILE.nsp --dest DIR [--keys prod.keys --firmware DIR] [--desktop-shortcut] [--start-menu]
                      [--delete-original] [--clean] [--temp DIR] [--workspace DIR]
    python -m builder --dest DIR                      resume, or rebuild after a change of the port's code
    python -m builder restore --dest DIR --out FILE.nsp   the original NSP again, exactly (checked by its SHA-256)
    python -m builder check-game --game FILE.nsp          is this the game and version the port takes (no build)
    python -m builder tools [--list] [--only ID,ID]       the tools of builder/tools.json: what is there, or get the rest
    python -m builder package --out DIR                   the builder as one folder that needs nothing installed

--game: a plaintext NSP of the game (title 0400380020056000, version 65536), or a NUP holding it: the program
that runs in the NUP (its newest update, else its base) must be that version. A NUP is never removed: it cannot
be made again exactly from the game folder. --keys and --firmware: the player's
prod.keys and a Switch firmware folder (an unpacked firmware, or an emulator's Contents/registered), for the system
fonts; read by tools/firmware_fonts.py only, their values never stored. Needed on the first build, not after.
--delete-original: once the folder is built, the NSP is rebuilt in memory from the folder, checked and sent to the
Recycle Bin. --clean: drop the temp folder's C and objects at the end (a rebuild then lifts again).
--fetch-tools: the check step downloads the tools that are missing (tools.py), each from its official source and
checked against the manifest's SHA-256; without it a missing tool stops the build with its name.
Prints JSON events on stdout (events.py); the readable log goes to <temp>/logs/. Exit code: 0 done, 1 failed, 2 bad
command line, 3 cancelled (Ctrl+C or Ctrl+Break).
"""
import argparse
import hashlib
import signal
import sys
import time
from pathlib import Path

from . import layout as layouts
from . import state as statefile
from .events import Reporter
from .runner import BuildError, Context, run_steps
from . import package as packager
from . import tools as toolbox
from .steps import all_steps, read_game, restore_nsp

EXIT = {'ok': 0, 'failed': 1, 'cancelled': 3}


class Parser(argparse.ArgumentParser):
    def error(self, message):
        Reporter().error('cli.usage', {'detail': message})
        sys.exit(2)


def temp_folder(base, dest):
    """One temp folder per game folder, so two builds never share generated files."""
    tag = hashlib.sha256(str(dest).lower().encode()).hexdigest()[:8]
    return base / f'{dest.name or "root"}-{tag}'


def cancel(signum, frame):
    raise KeyboardInterrupt


def restore(argv):
    ap = Parser(prog='python -m builder restore', description='the original NSP again from a game folder')
    ap.add_argument('--dest', type=Path, required=True, help='the game folder')
    ap.add_argument('--out', type=Path, required=True, help='the NSP to write')
    a = ap.parse_args(argv)
    rep = Reporter()
    t0 = time.monotonic()
    if a.out.exists():
        rep.error('restore.exists', {'path': str(a.out)})
        return 1
    try:
        sha = restore_nsp(a.dest, a.out, lambda d, t: rep.progress('restore', 'write', d, t, 'bytes'))
    except BuildError as e:
        rep.error(e.id, e.params)
        rep.emit('end', result='failed', seconds=round(time.monotonic() - t0, 1))
        return 1
    rep.emit('end', result='ok', seconds=round(time.monotonic() - t0, 1), sha256=sha, out=str(a.out))
    return 0


def check_game(argv):
    """One 'game' event (name, version, size, romfs) and end ok, or the error the check step would give."""
    ap = Parser(prog='python -m builder check-game', description='is this file the game and version the port takes')
    ap.add_argument('--game', type=Path, required=True, help='the file to check')
    ap.add_argument('--workspace', type=Path, help="the folder holding vendor/ and .toolchains/")
    a = ap.parse_args(argv)
    rep = Reporter()
    t0 = time.monotonic()
    game = a.game.resolve()
    try:
        got = read_game(game, layouts.find(a.workspace))
    except BuildError as e:
        rep.error(e.id, e.params)
        rep.emit('end', result='failed', seconds=round(time.monotonic() - t0, 1))
        return 1
    rep.emit('game', name=game.name, kind=got['kind'], version=got['version'], size=game.stat().st_size, romfs=got['romfs_size'])
    rep.emit('end', result='ok', seconds=round(time.monotonic() - t0, 1))
    return 0


def tools(argv):
    """--list: one 'tools' event (each tool, whether it is there, what a fetch would download). Else: get what is missing."""
    ap = Parser(prog='python -m builder tools', description='the tools of builder/tools.json')
    ap.add_argument('--list', action='store_true', help='only say what is there')
    ap.add_argument('--only', help='comma-separated ids to get')
    ap.add_argument('--workspace', type=Path, help="the folder holding vendor/ and .toolchains/")
    ap.add_argument('--toolchains', type=Path, help='the tools folder (default <workspace>/.toolchains)')
    ap.add_argument('--vendor', type=Path, help='the pinned clones folder (default <workspace>/vendor)')
    ap.add_argument('--downloads', type=Path, help='where downloads are kept (default <workspace>/out/builder-downloads)')
    a = ap.parse_args(argv)
    lay = layouts.find(a.workspace, toolchains=a.toolchains, vendor=a.vendor, downloads=a.downloads)
    rep = Reporter()
    t0 = time.monotonic()
    st = toolbox.status(lay.toolchains, lay.vendor)
    missing = [t for t in st if not t['present']]
    rep.emit('tools', toolchains=str(lay.toolchains), tools=st,
             missing=[t['id'] for t in missing if t['needed']], optional=[t['id'] for t in missing if not t['needed']],
             bytes=sum(t['size'] for t in missing if t['needed']))
    if a.list:
        rep.emit('end', result='ok', seconds=round(time.monotonic() - t0, 1))
        return 0
    for name in ('SIGBREAK', 'SIGTERM'):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), cancel)
    try:
        got = toolbox.ensure(lay.toolchains, lay.downloads, rep, 'tools', only=a.only.split(',') if a.only else None,
                             vendor=lay.vendor, repo=lay.repo)
    except BuildError as e:
        rep.error(e.id, e.params, step='tools')
        rep.emit('end', result='failed', seconds=round(time.monotonic() - t0, 1))
        return 1
    except KeyboardInterrupt:
        rep.emit('end', result='cancelled', seconds=round(time.monotonic() - t0, 1))
        return 3
    rep.emit('end', result='ok', seconds=round(time.monotonic() - t0, 1), got=got)
    return 0


def package(argv):
    ap = Parser(prog='python -m builder package', description='the builder as one folder that needs nothing installed')
    ap.add_argument('--out', type=Path, required=True, help='the package folder to make')
    ap.add_argument('--no-window', action='store_true', help='without SrBuilder.exe (the sources and Python only)')
    ap.add_argument('--workspace', type=Path, help="the folder holding vendor/ and .toolchains/")
    ap.add_argument('--downloads', type=Path, help='where downloads are kept (default <workspace>/out/builder-downloads)')
    a = ap.parse_args(argv)
    lay = layouts.find(a.workspace, downloads=a.downloads)
    rep = Reporter()
    t0 = time.monotonic()
    try:
        made = packager.make(lay, a.out, rep, window=not a.no_window)
    except BuildError as e:
        rep.error(e.id, e.params, step='package')
        rep.emit('end', result='failed', seconds=round(time.monotonic() - t0, 1))
        return 1
    rep.emit('end', result='ok', seconds=round(time.monotonic() - t0, 1), **made)
    return 0


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv[:1] == ['package']:
        return package(argv[1:])
    if argv[:1] == ['tools']:
        return tools(argv[1:])
    if argv[:1] == ['restore']:
        return restore(argv[1:])
    if argv[:1] == ['check-game']:
        return check_game(argv[1:])
    ap = Parser(prog='python -m builder', description=__doc__.splitlines()[0])
    ap.add_argument('--game', type=Path, help='the game: a plaintext NSP')
    ap.add_argument('--keys', type=Path, help='prod.keys, for the system fonts (first build)')
    ap.add_argument('--firmware', type=Path, help='Switch firmware folder, for the system fonts (first build)')
    ap.add_argument('--dest', type=Path, required=True, help='the game folder to make')
    ap.add_argument('--desktop-shortcut', action='store_true', help='a shortcut to sr.exe on the desktop')
    ap.add_argument('--start-menu', action='store_true', help='a shortcut to sr.exe in the Start menu')
    ap.add_argument('--delete-original', action='store_true',
                    help='at the end, check that the folder rebuilds the NSP exactly and send the NSP to the Recycle Bin')
    ap.add_argument('--clean', action='store_true', help="drop the temp folder's C and objects at the end")
    ap.add_argument('--temp', type=Path, help='temp folder (default <workspace>/out/builder-temp)')
    ap.add_argument('--fetch-tools', action='store_true', help='download the tools that are missing')
    ap.add_argument('--toolchains', type=Path, help='the tools folder (default <workspace>/.toolchains)')
    ap.add_argument('--vendor', type=Path, help='the pinned clones folder (default <workspace>/vendor)')
    ap.add_argument('--downloads', type=Path, help='where downloads are kept (default <workspace>/out/builder-downloads)')
    ap.add_argument('--workspace', type=Path, help="the folder holding vendor/ and .toolchains/ (default: the "
                                                   "repository's parent)")
    args = ap.parse_args(argv)

    # The window cancels with Ctrl+Break to the builder's process group, which also reaches the tools it runs
    for name in ('SIGBREAK', 'SIGTERM'):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), cancel)
    layout = layouts.find(args.workspace, toolchains=args.toolchains, vendor=args.vendor, temp=args.temp,
                          downloads=args.downloads)
    dest = args.dest.resolve()
    temp = temp_folder(layout.temp, dest)
    log = temp / 'logs' / time.strftime('build-%Y%m%d-%H%M%S.log')
    rep = Reporter(log_path=log)
    t0 = time.monotonic()
    try:
        state = statefile.load(dest)
    except statefile.StateError as e:
        rep.error(e.id, {'path': str(e.path)})
        rep.emit('end', result='failed', seconds=0)
        return 1
    ctx = Context(layout, dest, temp, rep, args, state=state)
    steps = all_steps()
    rep.emit('start', release=layout.release, mode=ctx.mode, dest=str(dest), log=str(log), steps=[s.id for s in steps])
    result = run_steps(steps, ctx)
    rep.emit('end', result=result, seconds=round(time.monotonic() - t0, 1))
    rep.close()
    return EXIT[result]
