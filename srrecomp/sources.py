"""Read ExeFS / RomFS files from plaintext dumps without extracting them to disk.

Supported inputs:
  * a directory holding exefs files (main, main.npdm, rtld, sdk, subsdk*)
  * an NSP whose program NCA is plaintext (section encryption type "None")
  * a NUP container (a55uka/nup SPEC.md v1.0-draft), base package by default

Encrypted content is refused: this tool never takes or looks for keys.
"""
import hashlib
import os
import struct

MEDIA = 0x200


class PlaintextRequired(Exception):
    pass


class FileSlice:
    """A read-only window [off, off+size) on an open file."""

    def __init__(self, fh, off, size):
        self.fh, self.off, self.size = fh, off, size

    def read(self, pos=0, n=None):
        if n is None:
            n = self.size - pos
        if pos < 0 or pos + n > self.size:
            raise ValueError('read outside slice')
        self.fh.seek(self.off + pos)
        data = self.fh.read(n)
        if len(data) != n:
            raise IOError('short read')
        return data

    def sub(self, pos, size):
        if pos + size > self.size:
            raise ValueError('sub-slice outside slice')
        return FileSlice(self.fh, self.off + pos, size)


def parse_pfs0(sl):
    hdr = sl.read(0, 0x10)
    if hdr[:4] != b'PFS0':
        raise ValueError('not a PFS0')
    n, strsz = struct.unpack_from('<II', hdr, 4)
    ent = sl.read(0x10, n * 0x18)
    strs = sl.read(0x10 + n * 0x18, strsz)
    data0 = 0x10 + n * 0x18 + strsz
    out = {}
    for i in range(n):
        off, size, name_off = struct.unpack_from('<QQI', ent, i * 0x18)
        name = strs[name_off:strs.index(b'\0', name_off)].decode('utf-8')
        out[name] = sl.sub(data0 + off, size)
    return out


class Nca:
    def __init__(self, sl):
        self.sl = sl
        h = sl.read(0, 0xC00)
        self.magic = h[0x200:0x204]
        if self.magic not in (b'NCA3', b'NCA2'):
            raise PlaintextRequired('NCA header is not plaintext (encrypted dump); keys are out of scope')
        self.content_type = h[0x205]
        self.key_generation = max(h[0x206], h[0x220])
        self.program_id = struct.unpack_from('<Q', h, 0x210)[0]
        self.sdk_addon_version = struct.unpack_from('<I', h, 0x21C)[0]
        self.rights_id_set = any(h[0x230:0x240])
        self.sections = []
        for i in range(4):
            start, end = struct.unpack_from('<II', h, 0x240 + i * 0x10)
            if end <= start:
                continue
            fsh = h[0x400 + i * 0x200:0x600 + i * 0x200]
            self.sections.append({
                'index': i,
                'offset': start * MEDIA,
                'size': (end - start) * MEDIA,
                'fs_type': fsh[2],      # 0 RomFS, 1 PartitionFS
                'hash_type': fsh[3],    # 2 HierarchicalSha256, 3 HierarchicalIntegrity (IVFC)
                'encryption': fsh[4],   # 1 None
                'fsh': fsh,
            })

    def _check(self, s):
        if s['encryption'] != 1:
            raise PlaintextRequired(f'NCA section {s["index"]} is encrypted (type {s["encryption"]}); keys are out of scope')

    def pfs0(self, s):
        self._check(s)
        fsh = s['fsh']
        if s['hash_type'] == 2:
            layers = struct.unpack_from('<I', fsh, 0x08 + 0x24)[0]
            off, size = struct.unpack_from('<QQ', fsh, 0x08 + 0x28 + (layers - 1) * 0x10)
        else:
            raise ValueError(f'unsupported PFS0 hash type {s["hash_type"]}')
        return parse_pfs0(self.sl.sub(s['offset'] + off, size))

    def romfs_slice(self, s):
        self._check(s)
        fsh = s['fsh']
        if fsh[0x08:0x0C] != b'IVFC':
            raise ValueError('RomFS section without IVFC header')
        max_layers = struct.unpack_from('<I', fsh, 0x08 + 0x0C)[0]
        off, size = struct.unpack_from('<QQ', fsh, 0x08 + 0x10 + (max_layers - 2) * 0x18)
        return self.sl.sub(s['offset'] + off, size)


def open_nsp_program(path):
    fh = open(path, 'rb')
    size = os.fstat(fh.fileno()).st_size
    top = parse_pfs0(FileSlice(fh, 0, size))
    for name, sl in top.items():
        if not name.endswith('.nca') or name.endswith('.cnmt.nca'):
            continue
        nca = Nca(sl)
        if nca.content_type == 0:  # Program
            return nca
    raise ValueError('no program NCA in NSP')


def romfs_entries(sl):
    """Walks a RomFS (level-6 data of the IVFC section). Yields (path, offset in sl, size)."""
    h = sl.read(0, 0x50)
    (hsz, dh_off, dh_sz, dm_off, dm_sz, fh_off, fh_sz, fm_off, fm_sz, data_off) = struct.unpack('<10Q', h)
    dmeta = sl.read(dm_off, dm_sz)
    fmeta = sl.read(fm_off, fm_sz)
    none = 0xFFFFFFFF
    stack = [(0, '')]
    while stack:
        d, path = stack.pop()
        _, _, child_dir, child_file, _, _ = struct.unpack_from('<6I', dmeta, d)
        f = child_file
        while f != none:
            _, sib, off, size, _, nlen = struct.unpack_from('<IIQQII', fmeta, f)
            name = fmeta[f + 0x20:f + 0x20 + nlen].decode('utf-8', 'replace')
            yield path + name, data_off + off, size
            f = sib
        c = child_dir
        while c != none:
            _, sib, _, _, _, nlen = struct.unpack_from('<6I', dmeta, c)
            name = dmeta[c + 0x18:c + 0x18 + nlen].decode('utf-8', 'replace')
            stack.append((c, path + name + '/'))
            c = sib


def nca_romfs(nca):
    for s in nca.sections:
        if s['fs_type'] == 0 and s['hash_type'] == 3:
            return nca.romfs_slice(s)
    raise ValueError('no RomFS section in program NCA')


def nca_exefs(nca):
    for s in nca.sections:
        if s['fs_type'] == 1:
            files = nca.pfs0(s)
            if 'main.npdm' in files:
                return files
    raise ValueError('no ExeFS section in program NCA')


# ---------------------------------------------------------------- NUP


class _Bytes:
    def __init__(self, data):
        self.data, self.size = data, len(data)

    def read(self, pos=0, n=None):
        return self.data[pos:] if n is None else self.data[pos:pos + n]


class Nup:
    KINDS = {0: 'base', 1: 'update', 2: 'addon'}

    def __init__(self, path):
        self.fh = open(path, 'rb')
        try:
            self._read_toc()
        except Exception:
            self.fh.close()   # not a NUP, or a damaged one: the file is not left open (Windows keeps it locked)
            raise

    def _read_toc(self):
        sb = self.fh.read(0x80)
        if sb[:4] != b'NUP\x01':
            raise ValueError('not a NUP v1 file')
        toc_off, toc_size = struct.unpack_from('<QQ', sb, 0x08)
        self.blob_off = struct.unpack_from('<Q', sb, 0x38)[0]
        if struct.unpack_from('<I', sb, 0x4C)[0]:
            raise ValueError('unknown NUP flags')
        self.fh.seek(toc_off)
        toc = self.fh.read(toc_size)
        if hashlib.sha256(toc).digest() != sb[0x18:0x38]:
            raise ValueError('NUP TOC hash mismatch')
        if toc[:4] != b'TOC\x01':
            raise ValueError('bad NUP TOC magic')
        pc, self.dir_count, self.file_count, sp_off, sp_size = struct.unpack_from('<IQQQQ', toc, 4)
        self.toc = toc
        self.pool = toc[sp_off:sp_off + sp_size]
        self.pkg_off = 0x30
        self.dir_off = self.pkg_off + pc * 0x40
        self.file_off = self.dir_off + self.dir_count * 0x18
        self.child_off = self.file_off + self.file_count * 0x50
        self.packages = []
        for i in range(pc):
            tid, root, exefs, romfs, ver, kind, flags = struct.unpack_from('<QIIIIBB', toc, self.pkg_off + i * 0x40)
            meta_off, meta_size = struct.unpack_from('<QI', toc, self.pkg_off + i * 0x40 + 0x20)
            self.packages.append({'title_id': tid, 'root': root, 'exefs': exefs, 'romfs': romfs,
                                  'version': ver, 'kind': self.KINDS.get(kind, kind), 'flags': flags,
                                  'meta_offset': meta_off, 'meta_size': meta_size})

    def _name(self, off, n):
        return self.pool[off:off + n].decode('utf-8')

    def children(self, d):
        name_off, name_len, first, count = struct.unpack_from('<IIII', self.toc, self.dir_off + d * 0x18)
        for k in range(count):
            node = struct.unpack_from('<I', self.toc, self.child_off + (first + k) * 4)[0]
            if node < self.dir_count:
                no, nl = struct.unpack_from('<II', self.toc, self.dir_off + node * 0x18)
                yield self._name(no, nl), ('dir', node)
            else:
                yield self._file(node - self.dir_count)

    def _file(self, i):
        rec = self.toc[self.file_off + i * 0x50:self.file_off + (i + 1) * 0x50]
        no, nl, size, ext_off, ext_size = struct.unpack_from('<IIQQQ', rec)
        return self._name(no, nl), ('file', {'size': size, 'off': ext_off, 'stored': ext_size,
                                             'sha256': rec[0x20:0x40], 'codec': rec[0x40]})

    def load(self, f):
        if f['size'] == 0:
            return _Bytes(b'')
        self.fh.seek(f['off'])
        raw = self.fh.read(f['stored'])
        if f['codec'] == 0:
            data = raw
        elif f['codec'] == 1:
            import zstandard
            data = zstandard.ZstdDecompressor().decompress(raw, max_output_size=f['size'])
        else:
            raise ValueError(f'unknown NUP codec {f["codec"]}')
        if hashlib.sha256(data).digest() != f['sha256']:
            raise ValueError('NUP file hash mismatch')
        return _Bytes(data)

    def package(self, kind=None, need='exefs'):
        """The highest-version package of `kind` that has `need` (exefs / romfs). kind None: the program that runs,
        the newest update when there is one, else the base."""
        for k in ((kind,) if kind else ('update', 'base')):
            pkgs = [p for p in self.packages if p['kind'] == k and p[need] != 0xFFFFFFFF]
            if pkgs:
                return max(pkgs, key=lambda p: p['version'])
        raise ValueError(f'no {kind or "update or base"} package with {need} in NUP')

    def exefs(self, kind=None):
        pkg = self.package(kind, 'exefs')
        return {name: self.load(node[1]) for name, node in self.children(pkg['exefs']) if node[0] == 'file'}, pkg

    def tree(self, d, prefix=''):
        """(path, file record) for every file under directory d, depth first in the stored (sorted) order."""
        for name, node in self.children(d):
            if node[0] == 'dir':
                yield from self.tree(node[1], prefix + name + '/')
            else:
                yield prefix + name, node[1]

    def meta(self, pkg):
        """The package's meta TLVs as {type: payload} (SPEC.md section 5); unknown types are kept, not interpreted."""
        out = {}
        if not pkg['meta_offset'] or not pkg['meta_size']:
            return out
        self.fh.seek(pkg['meta_offset'])
        m = self.fh.read(pkg['meta_size'])
        p = 0
        while p + 8 <= len(m):
            t, _, n = struct.unpack_from('<HHI', m, p)
            if p + 8 + n > len(m):
                break
            out.setdefault(t, m[p + 8:p + 8 + n])
            p += 8 + n
        return out

    def display_version(self):
        """TLV 0x0004 of the running package, else of the base; else the NACP TLV's display version; '' if none."""
        pkgs = [self.package(None, 'exefs')] + [p for p in self.packages if p['kind'] == 'base']
        for pkg in pkgs:
            v = self.meta(pkg).get(0x0004)
            if v:
                return v.decode('utf-8', 'replace')
        for pkg in pkgs:
            nacp = self.meta(pkg).get(0x0002)
            if nacp and len(nacp) >= 0x3070:
                return nacp[0x3060:0x3070].split(b'\0')[0].decode('ascii', 'replace')
        return ''


def nup_romfs(paths, overlay='replace'):
    """The program RomFS of one NUP or of the files of a split pack (nup pack --split), packages pooled.

    The newest update's tree; overlay 'replace' takes it alone, as the console does with a patch NCA (its RomFS is the
    whole patched RomFS), 'shadow' lays it over the base tree by path (SPEC.md's layering, which also keeps files only
    the base has). Without an update, the base tree. Returns (the Nup holding the tree, {path: file record},
    {'base': (Nup, package) or (None, None), 'update': ...})."""
    found = [(n, p) for n in (Nup(x) for x in paths) for p in n.packages if p['romfs'] != 0xFFFFFFFF]
    pick = {k: max(((n, p) for n, p in found if p['kind'] == k), key=lambda np: np[1]['version'], default=(None, None))
            for k in ('base', 'update')}
    use = 'update' if pick['update'][1] else 'base'
    nup, pkg = pick[use]
    if not pkg:
        raise ValueError('the NUP has no base or update package with a RomFS')
    if overlay == 'shadow' and use == 'update' and pick['base'][1] and pick['base'][0] is not nup:
        raise ValueError('overlay shadow needs the base and the update in one container')
    tree = {}
    if use == 'base' or (overlay == 'shadow' and pick['base'][1]):
        tree.update(pick['base'][0].tree(pick['base'][1]['romfs']))
    if use == 'update':
        tree.update(nup.tree(pkg['romfs']))
    return nup, tree, pick


class _NupRomfsReader:
    """read(off, size) for the (path, off, size) entries of open_romfs on a NUP: off is the file's extent offset;
    zstd files come back decompressed and SHA-256 checked."""

    def __init__(self, nup, tree):
        self.nup = nup
        self.by_off = {f['off']: f for f in tree.values() if f['size']}

    def read(self, off, size):
        if not size:
            return b''
        return self.nup.load(self.by_off[off]).read()


def open_romfs(path):
    """(entries [(path, off, size)], reader with read(off, size)) of the program RomFS of a plaintext NSP or a NUP."""
    with open(path, 'rb') as fh:
        magic = fh.read(4)
    if magic == b'NUP\x01':
        nup, tree, _ = nup_romfs([path])
        return [(n, f['off'], f['size']) for n, f in tree.items()], _NupRomfsReader(nup, tree)
    sl = nca_romfs(open_nsp_program(path))
    return list(romfs_entries(sl)), sl


# ---------------------------------------------------------------- entry point


def open_exefs(path, nup_kind=None):
    """Returns (files: {name: slice}, meta: dict). Slices expose read(pos, n) and size."""
    if os.path.isdir(path):
        files = {}
        for name in sorted(os.listdir(path)):
            p = os.path.join(path, name)
            if os.path.isfile(p):
                fh = open(p, 'rb')
                files[name] = FileSlice(fh, 0, os.fstat(fh.fileno()).st_size)
        return files, {'container': 'directory'}
    with open(path, 'rb') as fh:
        magic = fh.read(4)
    if magic == b'PFS0':
        nca = open_nsp_program(path)
        meta = {'container': 'nsp', 'nca_magic': nca.magic.decode(), 'program_id': f'{nca.program_id:016x}',
                'key_generation': nca.key_generation, 'rights_id_set': nca.rights_id_set,
                'sdk_addon_version_raw': f'{nca.sdk_addon_version:08x}',
                'sdk_addon_version': '.'.join(str((nca.sdk_addon_version >> s) & 0xFF) for s in (24, 16, 8, 0)),
                'sections': [{k: s[k] for k in ('index', 'size', 'fs_type', 'hash_type', 'encryption')}
                             for s in nca.sections]}
        return nca_exefs(nca), meta
    if magic == b'NUP\x01':
        nup = Nup(path)
        files, pkg = nup.exefs(nup_kind)
        return files, {'container': 'nup', 'program_id': f'{pkg["title_id"]:016x}', 'version': pkg['version'],
                       'package_kind': pkg['kind']}
    raise ValueError('unrecognised input (expected exefs directory, NSP or NUP)')
