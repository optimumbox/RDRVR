# cbrows.py <dumpbin .ll files>: per cbuffer, which 16-byte rows the shaders read (constant indices) and how many
# shaders index one dynamically. The handle of each createHandle(CBV) is mapped to its cbuffer by the range id in
# !dx.resources' CBV list.
import re, sys, collections
rows = collections.defaultdict(collections.Counter)
dyn = collections.Counter()
users = collections.Counter()
for p in sys.argv[1:]:
    t = open(p, encoding='utf-8', errors='replace').read()
    # CBV list: the 3rd element of !dx.resources; each entry !{i32 id, ... !"name", ...}
    names = {}
    # the names are stripped (!""): the cbuffer's struct type names it (%hostlayout.rage_matrices, %"hostlayout.$Globals")
    for m in re.finditer(r'^!\d+ = !\{i32 (\d+), %"?(?:hostlayout\.)?([^"*]+)"?\* undef, !"[^"]*", i32 \d+, i32 \d+, i32 \d+, i32 \d+, null\}', t, re.M):
        names[m.group(1)] = m.group(2)
    handles = {}
    for m in re.finditer(r'(%\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 2, i32 (\d+),', t):
        handles[m.group(1)] = names.get(m.group(2), 'cb' + m.group(2))
    seen = set()
    for m in re.finditer(r'@dx\.op\.cbufferLoadLegacy\.\w+\(i32 59, %dx\.types\.Handle (%\d+), i32 ([%\w]+)\)', t):
        cb = handles.get(m.group(1), '?')
        seen.add(cb)
        if m.group(2).startswith('%'): dyn[cb] += 1
        else: rows[cb][int(m.group(2))] += 1
    for cb in seen: users[cb] += 1
for cb, n in users.most_common():
    r = rows[cb]
    print(f"{cb}: {n} shaders; rows {sorted(r)}; dynamic loads {dyn[cb]}")
