"""read_samplers.py: the $Globals texture indices a shader really uses (loaded from $Globals and made into a 2D texture
handle), by name: to find the passes that read a screen image by an index of their own (view 1 cannot be redirected
there: no twin for them, stereo_vs.py NO_TWIN_PS).

    python read_samplers.py <dumpbin .ll> [...]
"""
import re
import sys

sys.path.insert(0, __import__('os').path.dirname(__file__))
import viewid_patch  # noqa: E402


def globals_uints(text):
    m = re.search(r'^; cbuffer \$Globals$(.*?)^;\s+\} \$Globals;', text, re.M | re.S)
    return {int(o): n for n, o in re.findall(r'uint (\w+);\s+; Offset:\s+(\d+)', m.group(1))} if m else {}


def read_samplers(text):
    names = globals_uints(text)
    rid = viewid_patch.range_ids(text)
    if '$Globals' not in rid:
        return []
    hg = set(viewid_patch.cb_handles(text, rid['$Globals']))
    loads = {}
    for m in re.finditer(r'^  (%\d+) = call %dx\.types\.CBufRet\.i32 @dx\.op\.cbufferLoadLegacy\.i32\(i32 59, %dx\.types\.Handle (%\d+), i32 (\d+)\)',
                         text, re.M):
        if m.group(2) in hg:
            loads[m.group(1)] = int(m.group(3))
    val = {}
    for m in re.finditer(r'^  (%\d+) = extractvalue %dx\.types\.CBufRet\.i32 (%\d+), (\d)$', text, re.M):
        if m.group(2) in loads:
            val[m.group(1)] = loads[m.group(2)] * 16 + int(m.group(3)) * 4
    for m in re.finditer(r'^  (%\d+) = add i32 (%\d+), 0$', text, re.M):
        if m.group(2) in val:
            val[m.group(1)] = val[m.group(2)]
    out = set()
    for m in re.finditer(r'@dx\.op\.createHandle\(i32 57, i8 0, i32 \d+, i32 (%\d+),', text):
        if m.group(1) in val and val[m.group(1)] in names:
            out.add(names[val[m.group(1)]])
    return sorted(out)


if __name__ == '__main__':
    for p in sys.argv[1:]:
        t = open(p, encoding='utf-8', errors='replace').read()
        e = re.search(r'^define void @(\w+)\(', t, re.M)
        print(p.rsplit('/', 1)[-1].rsplit('\\', 1)[-1], e.group(1) if e else '?', ':', ' '.join(read_samplers(t)))
