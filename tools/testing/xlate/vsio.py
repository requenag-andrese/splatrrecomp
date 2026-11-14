# Vertex shaders: input and output Locations each translator's module declares.
import os, struct, sys, collections
def io(path):
    w = open(path, 'rb').read(); w = struct.unpack('<%dI' % (len(w) // 4), w)
    loc, ins, outs, model, i = {}, set(), set(), None, 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xffff
        if op == 15: model = w[i + 1]
        if op == 71 and w[i + 2] == 30: loc[w[i + 1]] = w[i + 3]
        if op == 59 and w[i + 3] == 1: ins.add(w[i + 2])
        if op == 59 and w[i + 3] == 3: outs.add(w[i + 2])
        i += n
    return model, sorted(loc[v] for v in ins if v in loc), sorted(loc[v] for v in outs if v in loc)
a, b = sys.argv[1], sys.argv[2]
d = collections.Counter(); ex = {}; n = 0
for f in sorted(os.listdir(b)):
    if not f.endswith('.spv') or not os.path.exists(os.path.join(a, f)): continue
    ma, ia, oa = io(os.path.join(a, f)); mb, ib, ob = io(os.path.join(b, f))
    if ma != 0: continue
    n += 1
    k = ('in' if ia != ib else '') + ('out' if oa != ob else '')
    if k: d[k] += 1; ex.setdefault(k, (f, ia, ib, oa, ob))
print(n, 'vertex shaders')
for k, c in d.most_common():
    f, ia, ib, oa, ob = ex[k]; print(f'  {c:5d} differ in {k}, e.g. {f[:16]}\n     in  sr {ia}\n         ed {ib}\n     out sr {oa}\n         ed {ob}')
