"""Write the ExeFS files of a plaintext NSP / NUP (or copy an exefs directory) to a directory outside this repository,
for tools that need them on disk (totk-pc-recomp's gen.py).

usage: python tools/extract_exefs.py <.nsp | .nup | exefs dir> --out <dir> [--nup-package base|update]
"""
import argparse
import hashlib
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, REPO)

from srrecomp import sources  # noqa: E402


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('input')
    ap.add_argument('--out', required=True)
    ap.add_argument('--nup-package', choices=('base', 'update'),
                    help='NUP package to extract (default: the newest update, else the base)')
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    if os.path.commonpath([out, REPO]) == REPO:
        sys.exit('refusing to write game files inside the repository')
    files, _ = sources.open_exefs(a.input, a.nup_package)
    os.makedirs(out, exist_ok=True)
    for name, sl in files.items():
        data = sl.read()
        with open(os.path.join(out, name), 'wb') as f:
            f.write(data)
        print(f'{name}: {len(data)} bytes sha256 {hashlib.sha256(data).hexdigest()[:16]}', file=sys.stderr)


if __name__ == '__main__':
    main()
