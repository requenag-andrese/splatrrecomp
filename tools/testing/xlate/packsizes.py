# How many SPIR-V modules a shader cache pack holds and their mean and median size.
#   python -I packsizes.py <shader-cache.pack> [...]
import struct, sys, statistics
for p in sys.argv[1:]:
    b = open(p, 'rb').read(16); n = struct.unpack_from('<I', b, 8)[0]
    ix = open(p, 'rb').read(16 + 48 * n)
    s = [struct.unpack_from('<32sIIQ', ix, 16 + 48 * i)[2] for i in range(n) if struct.unpack_from('<32sIIQ', ix, 16 + 48 * i)[1] == 1]
    print(p.split('/')[-1], len(s), 'spv, mean %.1f KiB, median %.1f KiB' % (sum(s) / len(s) / 1024, statistics.median(s) / 1024))
