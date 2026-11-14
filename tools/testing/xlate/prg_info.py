# Records of shader-programs.bin by stage, and the first bytes of one program per stage (header and code layout).
#   python -I prg_info.py <shader-programs.bin>
import struct, sys, collections
b = open(sys.argv[1], 'rb').read()
assert b[:8] == b'SRRCSPRG', b[:8]
o = 16; st = collections.Counter(); n = 0; ex = {}
while len(b) - o >= 12 + 32 + 0x880:
    stage, ln, c1 = struct.unpack_from('<III', b, o)
    code = b[o + 12 + 32 + 0x880: o + 12 + 32 + 0x880 + ln]
    st[stage] += 1; n += 1
    if stage not in ex: ex[stage] = code[:0xa0]
    o += 12 + 32 + 0x880 + ln + c1
print(n, 'records', dict(st), 'end', o, len(b))
for s, c in ex.items():
    print('stage', s)
    for i in range(0, len(c), 16): print('  %03x' % i, c[i:i+16].hex(' ', 4))
