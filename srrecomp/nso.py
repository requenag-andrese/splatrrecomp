"""NSO0 loader: segments, MOD0, dynamic symbols and .eh_frame_hdr function count."""
import hashlib
import struct

import lz4.block

DT_NEEDED, DT_STRTAB, DT_SYMTAB, DT_STRSZ = 1, 5, 6, 10


class Nso:
    def __init__(self, data):
        if data[:4] != b'NSO0':
            raise ValueError(f'not NSO0 (magic {data[:4]!r})')
        self.version = struct.unpack_from('<I', data, 4)[0]
        flags = struct.unpack_from('<I', data, 0x0C)[0]
        segs = []
        for i in range(3):
            foff, moff, size = struct.unpack_from('<III', data, 0x10 + i * 0x10)
            fsize = struct.unpack_from('<I', data, 0x60 + i * 4)[0]
            segs.append((foff, moff, size, fsize, bool(flags >> i & 1), bool(flags >> (i + 3) & 1)))
        self.bss = struct.unpack_from('<I', data, 0x3C)[0]
        self.module_id = data[0x40:0x60]
        emb_off, emb_sz, dstr_off, dstr_sz, dsym_off, dsym_sz = struct.unpack_from('<IIIIII', data, 0x88)
        end = max(m + s for _, m, s, *_ in segs)
        img = bytearray(end)
        self.hash_ok = []
        for i, (foff, moff, size, fsize, comp, check) in enumerate(segs):
            raw = data[foff:foff + fsize]
            seg = lz4.block.decompress(raw, uncompressed_size=size) if comp else raw[:size]
            if len(seg) != size:
                raise ValueError('segment size mismatch')
            if check:
                self.hash_ok.append(hashlib.sha256(seg).digest() == data[0xA0 + i * 0x20:0xC0 + i * 0x20])
            img[moff:moff + size] = seg
        self.image = bytes(img)
        self.text = (segs[0][1], segs[0][2])
        self.ro = (segs[1][1], segs[1][2])
        self.data = (segs[2][1], segs[2][2])
        self.compressed = [s[4] for s in segs]
        ro0 = self.ro[0]
        self.dynstr = self.image[ro0 + dstr_off:ro0 + dstr_off + dstr_sz]
        self.dynsym_raw = self.image[ro0 + dsym_off:ro0 + dsym_off + dsym_sz]
        self._parse_mod0()

    def _parse_mod0(self):
        self.mod0 = None
        self.needed = []
        self.eh_fde_count = None
        mo = struct.unpack_from('<I', self.image, 4)[0]
        if mo + 0x1C > len(self.image) or self.image[mo:mo + 4] != b'MOD0':
            return
        dyn, bss_s, bss_e, eh_s, eh_e, obj = struct.unpack_from('<iiiiii', self.image, mo + 4)
        self.mod0 = mo
        dyn += mo
        tags = {}
        needed = []
        p = dyn
        while p + 16 <= len(self.image):
            tag, val = struct.unpack_from('<qQ', self.image, p)
            p += 16
            if tag == 0:
                break
            if tag == DT_NEEDED:
                needed.append(val)
            else:
                tags.setdefault(tag, val)
        strtab = tags.get(DT_STRTAB)
        if strtab is not None:
            for off in needed:
                s = strtab + off
                self.needed.append(self.image[s:self.image.index(b'\0', s)].decode('ascii', 'replace'))
        eh_s += mo
        self.eh_hdr = None
        if eh_e != eh_s - mo and eh_s + 12 <= len(self.image) and self.image[eh_s] == 1:
            fde_enc = self.image[eh_s + 2]
            # eh_frame_ptr (4 bytes for sdata4/udata4) then fde_count
            if fde_enc in (0x03, 0x0B) and self.image[eh_s + 1] in (0x1B, 0x03, 0x0B):
                self.eh_fde_count = struct.unpack_from('<I', self.image, eh_s + 8)[0]
                self.eh_hdr = eh_s

    # ---- unwind tables: function extents

    def _enc(self, at, enc, datarel):
        """DWARF pointer at image offset `at` with encoding `enc` -> (value, size)."""
        fmt, app = enc & 0x0F, enc & 0x70
        if fmt == 0x0B:
            v, n = struct.unpack_from('<i', self.image, at)[0], 4
        elif fmt == 0x03:
            v, n = struct.unpack_from('<I', self.image, at)[0], 4
        elif fmt in (0x0C, 0x04, 0x00):
            v, n = struct.unpack_from('<q' if fmt == 0x0C else '<Q', self.image, at)[0], 8
        else:
            raise ValueError(f'pointer encoding {enc:#x}')
        if app == 0x10:
            v += at
        elif app == 0x30:
            v += datarel
        return v, n

    def _uleb(self, at):
        v = s = 0
        while True:
            b = self.image[at]
            at += 1
            v |= (b & 0x7F) << s
            s += 7
            if not b & 0x80:
                return v, at

    def _cie_r_enc(self, cie):
        p = cie + 8  # length, id
        version = self.image[p]
        p += 1
        z = self.image.index(b'\0', p)
        aug = self.image[p:z].decode('ascii')
        p = z + 1
        _, p = self._uleb(p)              # code alignment
        _, p = self._uleb(p)              # data alignment (sleb, same length)
        p = p + 1 if version == 1 else self._uleb(p)[1]  # return register
        enc = 0x00
        if aug.startswith('z'):
            _, p = self._uleb(p)
            for ch in aug[1:]:
                if ch == 'R':
                    enc = self.image[p]
                    p += 1
                elif ch == 'P':
                    penc = self.image[p]
                    p += 1 + self._enc(p + 1, penc & 0x7F, 0)[1]
                elif ch == 'L':
                    p += 1
                elif ch in 'SB':
                    pass
                else:
                    break
        return enc

    def function_ranges(self):
        """[(start, end)] image offsets of every function with an FDE (.eh_frame_hdr search table)."""
        if self.eh_hdr is None:
            return []
        h = self.eh_hdr
        ptr_enc, cnt_enc, tab_enc = self.image[h + 1], self.image[h + 2], self.image[h + 3]
        p = h + 4
        _, n = self._enc(p, ptr_enc, h)
        p += n
        count, n = self._enc(p, cnt_enc, h)
        p += n
        cies = {}
        out = []
        for _ in range(count):
            start, n = self._enc(p, tab_enc, h)
            p += n
            fde, n = self._enc(p, tab_enc, h)
            p += n
            cie = fde + 4 - struct.unpack_from('<I', self.image, fde + 4)[0]
            if cie not in cies:
                cies[cie] = self._cie_r_enc(cie)
            enc = cies[cie]
            _, n = self._enc(fde + 8, enc, h)
            rng, _ = self._enc(fde + 8 + n, enc & 0x0F, h)
            out.append((start, start + rng))
        return out

    def symbols(self):
        """Yields (name, bind, type, defined, value)."""
        for i in range(len(self.dynsym_raw) // 24):
            st_name, st_info, st_other, st_shndx, st_value, st_size = struct.unpack_from('<IBBHQQ', self.dynsym_raw, i * 24)
            if i == 0 and st_name == 0:
                continue
            end = self.dynstr.index(b'\0', st_name)
            name = self.dynstr[st_name:end].decode('ascii', 'replace')
            yield name, st_info >> 4, st_info & 0xF, st_shndx != 0, st_value

    def text_words(self):
        off, size = self.text
        return self.image[off:off + size]
