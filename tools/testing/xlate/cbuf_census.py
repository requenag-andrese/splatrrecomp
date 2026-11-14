# Which constant buffer slots sr-xlate's reflections bind, over every .res.json of the cache packs, by stage.
import json, re, struct, sys, collections
cnt = collections.Counter()
n = 0
for p in sys.argv[1:]:
    f = open(p, 'rb')
    head = f.read(16)
    k = struct.unpack_from('<I', head, 8)[0]
    ix = f.read(48 * k)
    for i in range(k):
        name, kind, size, off = struct.unpack_from('<32sIIQ', ix, 48 * i)
        if kind != 2: continue
        f.seek(off)
        r = json.loads(f.read(size))
        n += 1
        for c in r.get('cbufs', []):
            cnt[(r['stage'], c['slot'])] += 1
print(n, 'reflections')
for (st, slot), c in sorted(cnt.items()):
    print(f'  {st} c{slot}: {c}')
