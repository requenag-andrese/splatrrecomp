"""Find texture size queries that read a component the declared image type does not have.

The PR #88 translator types a texture it has no answer-table row for as 2D; a TXQ that reads component 2 (depth or
layer count) then becomes OpCompositeExtract index 2 of an ivec2, which spirv-val rejects. Returns the descriptor
bindings of those textures so the census can retranslate them with an inferred array type.
"""
import struct

OP_DECORATE, OP_TYPE_VECTOR, OP_LOAD, OP_COMPOSITE_EXTRACT = 71, 23, 61, 81
OP_IMAGE, OP_QUERY_SIZE_LOD, OP_QUERY_SIZE, OP_SAMPLED_IMAGE = 100, 103, 104, 86
DEC_BINDING = 33


def oob_query_bindings(data):
    w = struct.unpack(f'<{len(data) // 4}I', data)
    vec_n, loads, images, queries, binding = {}, {}, {}, {}, {}
    extracts = []
    i = 5
    while i < len(w):
        n, op = w[i] >> 16, w[i] & 0xFFFF
        if n == 0:
            break
        a = w[i + 1:i + n]
        if op == OP_TYPE_VECTOR:
            vec_n[a[0]] = a[2]
        elif op == OP_DECORATE and len(a) >= 3 and a[1] == DEC_BINDING:
            binding[a[0]] = a[2]
        elif op == OP_LOAD:
            loads[a[1]] = a[2]
        elif op == OP_IMAGE:
            images[a[1]] = a[2]
        elif op == OP_SAMPLED_IMAGE:
            loads.setdefault(a[1], a[2])  # follow the image operand back to its load
        elif op in (OP_QUERY_SIZE_LOD, OP_QUERY_SIZE):
            queries[a[1]] = (a[0], a[2])
        elif op == OP_COMPOSITE_EXTRACT and len(a) == 4:
            extracts.append((a[2], a[3]))
        i += n
    out = set()
    for src, idx in extracts:
        q = queries.get(src)
        if not q or idx < vec_n.get(q[0], 1):
            continue
        img = q[1]
        img = images.get(img, img)
        for _ in range(4):  # OpImage <- OpLoad <- variable (possibly through OpSampledImage)
            if img in binding:
                break
            img = loads.get(img, img)
        if img in binding:
            out.add(binding[img])
    return out
