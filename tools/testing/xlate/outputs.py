# Fragment output locations each translator's module declares (Output variables with a Location), per shader.
import os, struct, sys, collections

def outputs(path):
    w = open(path, 'rb').read()
    w = struct.unpack('<%dI' % (len(w) // 4), w)
    loc, out, model = {}, set(), None
    i = 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xffff
        if op == 15: model = w[i + 1]
        if op == 71 and w[i + 2] == 30: loc[w[i + 1]] = w[i + 3]   # OpDecorate Location
        if op == 59 and w[i + 3] == 3: out.add(w[i + 2])           # OpVariable Output
        i += n
    return model, sorted(loc[v] for v in out if v in loc)

a_dir, b_dir = sys.argv[1], sys.argv[2]
diff = collections.Counter(); ex = {}; n = 0
for f in sorted(os.listdir(b_dir)):
    if not f.endswith('.spv') or not os.path.exists(os.path.join(a_dir, f)): continue
    ma, oa = outputs(os.path.join(a_dir, f)); mb, ob = outputs(os.path.join(b_dir, f))
    if ma != 4: continue   # fragment only
    n += 1
    if oa != ob:
        k = (tuple(oa), tuple(ob)); diff[k] += 1; ex.setdefault(k, f)
print(n, 'fragment shaders,', sum(diff.values()), 'with other outputs')
for (oa, ob), c in diff.most_common(10): print(f'  {c:5d}  sr-xlate {list(oa)}  eden {list(ob)}  e.g. {ex[(oa, ob)][:16]}')
