# psoinfo.py <psodump dir> [converted dir]: the depth test of every dumped pipeline ([Debug] PsoDump, "PSO1" files),
# grouped by its vertex shader, with its render-target formats; marks vertex shaders that have a converted twin
import collections, os, struct, sys
FUNCS = {1: 'NEVER', 2: 'LESS', 3: 'EQUAL', 4: 'LESS_EQUAL', 5: 'GREATER', 6: 'NOT_EQUAL', 7: 'GREATER_EQUAL', 8: 'ALWAYS'}
conv = set()
if len(sys.argv) > 2:
    conv = {f[3:19] for f in os.listdir(sys.argv[2]) if f.startswith('vs_') and f.endswith('.dxil')}
def fnv(b):
    h = 1469598103934665603
    for x in b: h = ((h ^ x) * 1099511628211) & 0xffffffffffffffff
    return h
rows = collections.Counter()
for f in sorted(os.listdir(sys.argv[1])):
    b = open(os.path.join(sys.argv[1], f), 'rb').read()
    if b[:4] != b'PSO1': continue
    o = 4; blobs = []
    for _ in range(6):
        n = struct.unpack_from('<I', b, o)[0]; blobs.append(b[o + 4:o + 4 + n]); o += 4 + n
    o += 4 + 328 + 4 + 44  # stream-output count, blend, sample mask, rasterizer
    de, dw, dfunc = struct.unpack_from('<III', b, o); o += 52
    ne = struct.unpack_from('<I', b, o)[0]; o += 4
    for _ in range(ne):
        n = struct.unpack_from('<I', b, o)[0]; o += 4 + n + 24
    cut, topo, nrt, dsv, node = struct.unpack_from('<5I', b, o); o += 20
    rtv = struct.unpack_from('<8I', b, o)[:nrt]
    vs = '%016x' % fnv(blobs[0]) if blobs[0] else '-'
    key = (vs, 'twin' if vs in conv else '', FUNCS.get(dfunc, dfunc) if de else 'off', 'w' if dw else '-', tuple(rtv), dsv)
    rows[key] += 1
for k, n in sorted(rows.items(), key=lambda x: (x[0][0], x[0][4])):
    print(n, *k)
