"""Where the builder finds its tools: the repository it lives in and the workspace around it.

The workspace is the repository's parent (README "Layout"): vendor/ (the pinned clones the lifter and the scanners come
from), .toolchains/ (the venv's Python, llvm-mingw, .NET, the translator build, SDL3, the Vulkan headers, Streamline,
FidelityFX, NVIDIA's DLSS library) and out/ (the builder's default temp folder). Every path can be moved with an option
of the command line (cli.py); a missing tool is one clear message from the check step, not a stack trace.
"""
import glob
import hashlib
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent


@dataclass
class Layout:
    repo: Path
    workspace: Path
    vendor: Path
    toolchains: Path
    python: Path
    clang: Path
    dotnet: Path
    xlate: Path
    streamline: Path
    fidelityfx: Path
    dlss: Path
    temp: Path
    downloads: Path = None
    release: str = ''
    missing: list = field(default_factory=list)   # (what, path) of the tools not found


def _first(pattern):
    hits = sorted(glob.glob(str(pattern)))
    return Path(hits[-1]) if hits else Path(str(pattern).replace('*', 'X'))


def release_id(repo):
    """The repository's commit and whether its tree has changes: what built a game folder, recorded in sr-build.json.
    In a package (package.py) there is no git: RELEASE, written when it was made, says it."""
    stamp = Path(repo) / 'RELEASE'
    if stamp.is_file() and not (Path(repo) / '.git').exists():
        return stamp.read_text(encoding='ascii', errors='replace').strip() or 'unknown'
    try:
        head = subprocess.run(['git', '-C', str(repo), 'rev-parse', '--short=12', 'HEAD'], capture_output=True,
                              text=True, timeout=20).stdout.strip()
        dirty = subprocess.run(['git', '-C', str(repo), 'status', '--porcelain', '--untracked-files=no'],
                               capture_output=True, text=True, timeout=20).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return 'unknown'
    if not head:
        return 'unknown'
    if dirty:   # a changed tree: the fingerprint of the diff keeps two different trees apart
        diff = subprocess.run(['git', '-C', str(repo), 'diff', 'HEAD'], capture_output=True, timeout=60).stdout
        return f'{head}+{hashlib.sha256(diff).hexdigest()[:8]}'
    return head


def find(workspace=None, toolchains=None, vendor=None, temp=None, downloads=None):
    ws = Path(workspace).resolve() if workspace else REPO.parent
    tc = Path(toolchains).resolve() if toolchains else ws / '.toolchains'
    vd = Path(vendor).resolve() if vendor else ws / 'vendor'
    venv_py = tc / 'venv' / 'Scripts' / 'python.exe'
    lay = Layout(
        repo=REPO, workspace=ws, vendor=vd, toolchains=tc,
        python=venv_py if venv_py.is_file() else Path(sys.executable),
        clang=_first(tc / 'llvm-mingw-*' / 'bin' / 'clang.exe'),
        dotnet=tc / 'dotnet',
        xlate=tc / 'sr_xlate',
        streamline=tc / 'streamline',
        fidelityfx=_first(tc / 'fidelityfx' / 'FidelityFX-SDK-*'),
        dlss=tc / 'dlss-dl' / 'nvngx_dlss.dll',
        temp=Path(temp).resolve() if temp else ws / 'out' / 'builder-temp',
        downloads=Path(downloads).resolve() if downloads else ws / 'out' / 'builder-downloads',
        release=release_id(REPO),
    )
    needed = [('vendor: totk-pc-recomp', vd / 'totk-pc-recomp'), ('vendor: nx2android PR #88', vd / 'nx2android-pr88'),
              ('vendor: nup', vd / 'nup'), ('llvm-mingw clang', lay.clang), ('.NET', lay.dotnet / 'dotnet.exe'),
              ('shader translator (tools/sr_xlate build)', lay.xlate / 'sr-xlate.exe'),
              ('Vulkan headers', tc / 'vulkan-headers'), ('SDL3', tc / 'sdl3')]
    lay.missing = [(what, p) for what, p in needed if not p.exists()]
    return lay
