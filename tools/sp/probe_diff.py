# probe_diff.py "<perf line A>" "<perf line B>": per-frame means between two cumulative probe lines, split by mode
import re, sys
def parse(s):
    s = s.split('|| ', 1)[-1]
    f = {k: float(v) for k, v in re.findall(r'([a-z+0-9 x]+?) (-?\d+\.\d+)', s)}
    n = re.search(r'probe (\d+) frames \(stereo (\d+), mono (\d+)\)', s)
    return int(n.group(1)), int(n.group(2)), int(n.group(3)), s
a, b = parse(sys.argv[1]), parse(sys.argv[2])
na, nb = a[0], b[0]
keys = ['pass 1', 'pass 2', 'mono', 'between', 'playback wait', 'idle', 'render period', 'draws']
def val(s, k):
    m = re.search(re.escape(k) + r' (\d+\.\d+)', s)
    return float(m.group(1)) if m else 0.0
dn, ds, dm = nb - na, b[1] - a[1], b[2] - a[2]
print(f"frames {dn} (stereo {ds}, mono {dm})")
for k in keys:
    tot = val(b[3], k) * nb - val(a[3], k) * na
    print(f"  {k:14s} {tot / dn:8.3f} ms/frame" + (f"   {tot / ds:8.3f} per stereo frame" if ds and k in ('pass 1', 'pass 2', 'between') else ""))
