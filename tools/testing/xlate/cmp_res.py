# For every key both translators produced, compares what the runtime binds: constant buffer slots, storage buffer
# addresses (constant buffer + word), texture and image handle words with their types and array lengths.
import json, os, sys, collections
a_dir, b_dir = sys.argv[1], sys.argv[2]   # a: sr-xlate, b: eden-xlate
diff = collections.Counter()
ex = {}
n = same = 0

def load(d, k):
    return json.load(open(os.path.join(d, k + '.res.json'), encoding='utf-8'))

def norm(r):
    cb = sorted(x['slot'] for x in r['cbufs'] if x['slot'] >= 0)
    sb = sorted((x['cb_slot'], x['cb_offset'], x['flags'] & 1) for x in r['sbufs'])
    tx = sorted((2 if x['cbuf_slot'] < 0 else x['cbuf_slot'], x['handle'] & 0x3fff, x['type'], x['array']) for x in r['textures'])
    im = sorted((2 if x['cbuf_slot'] < 0 else x['cbuf_slot'], x['handle'] & 0x3fff, x['type'] & 0xff, x['array']) for x in r['images'])
    return cb, sb, tx, im

for f in sorted(os.listdir(b_dir)):
    if not f.endswith('.res.json'): continue
    k = f[:-9]
    if not os.path.exists(os.path.join(a_dir, f)): continue
    n += 1
    A, B = norm(load(a_dir, k)), norm(load(b_dir, k))
    tags = []
    for name, x, y in zip(('cbufs', 'sbufs', 'textures', 'images'), A, B):
        if x != y:
            # Ryujinx keeps c0/c2 reads it folds? record which side has more
            tags.append(name)
    if not tags:
        same += 1
        continue
    t = '+'.join(tags)
    diff[t] += 1
    if t not in ex: ex[t] = (k, A, B)
print(f'{n} shaders compared, {same} bind the same, {n - same} differ')
for t, c in diff.most_common():
    k, A, B = ex[t]
    print(f'  {c:5d}  {t}   e.g. {k[:16]}')
    for name, x, y in zip(('cbufs', 'sbufs', 'textures', 'images'), A, B):
        if x != y: print(f'           {name}: sr-xlate {x}\n           {" " * len(name)}  eden     {y}')
