"""Extract the system built-in fonts (nn::pl) from the user's own firmware NCA directory.

The firmware NCAs are encrypted: --keys names the user's prod.keys, whose values stay inside this process (never
printed, logged or copied). Decryption and RomFS verification (NCA FS header hash, all six IVFC levels) reuse
totk-pc-recomp's vendored MacNativeGames scripts (MIT) from --vendor.

    python tools/firmware_fonts.py --vendor ../vendor --keys <prod.keys> --firmware <NCA dir> --list
    python tools/firmware_fonts.py --vendor ../vendor --keys <prod.keys> --firmware <NCA dir> --out ../out/fonts

--list prints, for each system data title, the font files in its RomFS (names and sizes only). Without it, every
font file found is written to --out (outside the repository) with a manifest.json of names, sizes and SHA-256, and
the nn::pl fonts again under --out/shared-fonts with the names the runtime loads (build_runtime.py --fonts).
"""
import argparse
import hashlib
import json
import struct
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FONT_EXT = ('.bfttf', '.bfotf', '.ttf', '.otf', '.ttc', '.bffnt')
# Firmware file -> the name the runtime loads (runtime/core/ov_system.c font_files, runtime/sr/ov_pl_switch2.c).
# nn::pl SharedFontType / BuiltInFontTypeNx order 0..5; 6 is the second Nintendo-extension font of firmware 20+.
RUNTIME_NAMES = {
    'nintendo_udsg-r_std_003.bfttf': '0-standard.bfttf',
    'nintendo_udsg-r_org_zh-cn_003.bfttf': '1-chinese-simplified.bfttf',
    'nintendo_udsg-r_ext_zh-cn_003.bfttf': '2-chinese-simplified-extension.bfttf',
    'nintendo_udjxh-db_zh-tw_003.bfttf': '3-chinese-traditional.bfttf',
    'nintendo_udsg-r_ko_003.bfttf': '4-korean.bfttf',
    'nintendo_ext_003.bfttf': '5-nintendo-extension.bfttf',
    'nintendo_ext2_003.bfttf': '6-nintendo-extension2.bfttf',
}


def load_vendor(vendor):
    d = vendor / 'totk-pc-recomp' / 'tools' / 'vendor' / 'macnativegames'
    if not d.is_dir():
        sys.exit(f'error: {d} not found')
    sys.path.insert(0, str(d))
    import extract_exefs, extract_romfs, extract_shared_fonts  # noqa: E401
    return extract_exefs, extract_romfs, extract_shared_fonts


def inside_repo(path):
    try:
        path.resolve().relative_to(REPO)
        return True
    except ValueError:
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--vendor', type=Path, required=True)
    ap.add_argument('--keys', type=Path, required=True)
    ap.add_argument('--firmware', type=Path, required=True)
    ap.add_argument('--out', type=Path)
    ap.add_argument('--list', action='store_true')
    args = ap.parse_args()
    if not args.list and not args.out:
        sys.exit('error: give --out <dir> or --list')
    if args.out and inside_repo(args.out):
        sys.exit('error: --out must be outside the repository')
    ex, rf, sf = load_vendor(args.vendor)
    try:
        keys = ex.load_keys(args.keys)
    except ValueError as e:
        sys.exit(f'error: {e}')
    if 'header_key' not in keys:
        sys.exit('error: the keys file has no header_key')

    ncas = sorted(p for p in args.firmware.iterdir() if p.is_file() and p.suffix.lower() == '.nca')
    stats = dict(ncas=len(ncas), header_ok=0, data=0)
    data = {}
    for p in ncas:
        with p.open('rb') as f:
            hdr = sf.nca_header(f, keys)
        if hdr[0x200:0x204] != b'NCA3':
            continue
        stats['header_ok'] += 1
        if hdr[0x205] != 4:  # content type 4: Data
            continue
        stats['data'] += 1
        title = f'{struct.unpack_from("<Q", hdr, 0x210)[0]:016x}'
        data.setdefault(title, []).append(p)
    print(json.dumps(stats))
    if stats['header_ok'] == 0:
        sys.exit('error: no NCA header decrypts with this header_key')

    found = []
    errors = {}
    with tempfile.TemporaryDirectory() as tmp:
        for title in sorted(data):
            for p in data[title]:
                image = Path(tmp) / f'{p.stem}.romfs'
                try:
                    sf.nca_romfs(p, keys, image)
                    entries = rf.index_romfs(image)
                except ValueError as e:
                    errors.setdefault(str(e), []).append(title)
                    continue
                fonts = [e for e in entries if e['path'].lower().endswith(FONT_EXT)]
                if fonts and args.list:
                    print(title, [(e['path'], e['size']) for e in fonts])
                if fonts and args.out:
                    with image.open('rb') as f:
                        for e in fonts:
                            blob = ex.read_at(f, e['offset'], e['size'])
                            dest = args.out / title / e['path']
                            dest.parent.mkdir(parents=True, exist_ok=True)
                            dest.write_bytes(blob)
                            named = RUNTIME_NAMES.get(e['path'])
                            if named:
                                (args.out / 'shared-fonts').mkdir(parents=True, exist_ok=True)
                                (args.out / 'shared-fonts' / named).write_bytes(blob)
                            found.append(dict(title_id=title, path=e['path'], bytes=len(blob),
                                              sha256=hashlib.sha256(blob).hexdigest()))
                image.unlink()
    for msg, titles in errors.items():
        print(f"skipped {len(titles)} data NCA(s): {msg}: {sorted(titles)}")
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)
        (args.out / 'manifest.json').write_text(json.dumps(dict(fonts=found), indent=2) + '\n')
        print(json.dumps(dict(written=len(found), out=str(args.out))))


if __name__ == '__main__':
    main()
