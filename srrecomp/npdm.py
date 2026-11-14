"""main.npdm: META header, ACI0 kernel capabilities (allowed SVCs) and service access list."""
import struct


def _kcaps(blob):
    svcs, other = set(), {}
    for (d,) in struct.iter_unpack('<I', blob[:len(blob) // 4 * 4]):
        if d == 0xFFFFFFFF:
            continue
        ones = 0
        while d >> ones & 1:
            ones += 1
        if ones == 4:  # EnableSystemCalls: bits 5..28 mask, 29..31 index
            mask, idx = (d >> 5) & 0xFFFFFF, d >> 29
            for b in range(24):
                if mask >> b & 1:
                    svcs.add(idx * 24 + b)
        else:
            name = {3: 'ThreadInfo', 6: 'MemoryMap', 7: 'IoMemoryMap', 10: 'MemoryRegionMap',
                    11: 'EnableInterrupts', 13: 'MiscParams', 14: 'KernelVersion',
                    15: 'HandleTableSize', 16: 'MiscFlags'}.get(ones, f'type{ones}')
            other[name] = other.get(name, 0) + 1
    return sorted(svcs), other


def _services(blob):
    out, p = [], 0
    while p < len(blob):
        ctrl = blob[p]
        if ctrl == 0:
            break
        n = (ctrl & 7) + 1
        out.append(('host:' if ctrl & 0x80 else '') + blob[p + 1:p + 1 + n].decode('ascii', 'replace'))
        p += 1 + n
    return out


def parse(data):
    if data[:4] != b'META':
        raise ValueError('not an NPDM')
    flags = data[0x0C]
    info = {
        'flags': flags,
        'is_64bit': bool(flags & 1),
        'address_space': (flags >> 1) & 7,
        'main_thread_priority': data[0x0E],
        'main_thread_core': data[0x0F],
        'system_resource_size': struct.unpack_from('<I', data, 0x14)[0],
        'version': struct.unpack_from('<I', data, 0x18)[0],
        'main_thread_stack_size': struct.unpack_from('<I', data, 0x1C)[0],
        'name': data[0x20:0x30].split(b'\0')[0].decode('ascii', 'replace'),
    }
    aci_off, aci_sz, acid_off, acid_sz = struct.unpack_from('<IIII', data, 0x70)
    aci = data[aci_off:aci_off + aci_sz]
    if aci[:4] == b'ACI0':
        info['program_id'] = f'{struct.unpack_from("<Q", aci, 0x10)[0]:016x}'
        srv_off, srv_sz, kc_off, kc_sz = struct.unpack_from('<IIII', aci, 0x28)
        info['services'] = _services(aci[srv_off:srv_off + srv_sz])
        info['allowed_svcs'], info['kernel_caps'] = _kcaps(aci[kc_off:kc_off + kc_sz])
    return info
