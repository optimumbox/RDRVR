"""depth_readers.py: the shaders that cannot read the scene depth, for the core's deferred depth copy (prepass_arrays.cpp
publish: the game turns the depth to read about eleven times a view-instanced region, mostly around draws that only
depth-test it, and each copy out is two full-size depth copies).

A shader is listed when every texture it reads is a $Globals index whose name says nothing of depth or stencil
(read_samplers.py) and it reads no other texture (a global_textures slot, a fixed register, an index from elsewhere):
anything else may be the depth. Writes <shader set>/no_depth.txt: one hash per line (the dump's name: vs_<hash>,
ps_<hash>).

    python depth_readers.py <dump dir> <shader set dir> [--dxc-build <dir>]
"""
import argparse
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import viewid_patch  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[3]
DEPTHY = re.compile(r'depth|stencil', re.I)


def cb_names(text, cb):
    m = re.search(r'^; cbuffer ' + re.escape(cb) + r'$(.*?)^;\s+\} ' + re.escape(cb) + ';', text, re.M | re.S)
    return {int(o): n for n, o in re.findall(r'uint (\w+);\s+; Offset:\s+(\d+)', m.group(1))} if m else {}


def index_names(text):
    """{SSA value: the uint's name} for the indices loaded from $Globals or global_textures"""
    rid = viewid_patch.range_ids(text)
    val = {}
    for cb in ('$Globals', 'global_textures'):
        if cb not in rid:
            continue
        names = cb_names(text, cb)
        hg = set(viewid_patch.cb_handles(text, rid[cb]))
        loads = {}
        for m in re.finditer(r'^  (%\d+) = call %dx\.types\.CBufRet\.i32 @dx\.op\.cbufferLoadLegacy\.i32\(i32 59, %dx\.types\.Handle (%\d+), i32 (\d+)\)',
                             text, re.M):
            if m.group(2) in hg:
                loads[m.group(1)] = int(m.group(3))
        for m in re.finditer(r'^  (%\d+) = extractvalue %dx\.types\.CBufRet\.i32 (%\d+), (\d)$', text, re.M):
            if m.group(2) in loads:
                val[m.group(1)] = names.get(loads[m.group(2)] * 16 + int(m.group(3)) * 4, '?')
    for m in re.finditer(r'^  (%\d+) = add i32 (%\d+), 0$', text, re.M):
        if m.group(2) in val:
            val[m.group(1)] = val[m.group(2)]
    return val


def cannot_read_depth(text):
    if re.search(r'@dx\.op\.createHandle(FromHeap|FromBinding)', text):
        return False
    val = index_names(text)
    for idx in re.findall(r'@dx\.op\.createHandle\(i32 57, i8 0, i32 \d+, i32 (%\d+|\d+),', text):
        n = val.get(idx)
        if n is None or n == '?' or DEPTHY.search(n):
            return False  # a texture by an index of unknown origin, or a depth's
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--dxc-build', default=ROOT / 'research' / 'dxc-build' / 'bin')
    a = ap.parse_args()
    dxc = pathlib.Path(a.dxc_build).resolve() / 'dxc.exe'
    out, n = [], 0
    for f in sorted(pathlib.Path(a.src).glob('*.dxbc')):
        r = subprocess.run([str(dxc), '-dumpbin', str(f)], capture_output=True, text=True, errors='replace')
        if r.returncode:
            continue
        n += 1
        if cannot_read_depth(r.stdout):
            out.append(f.stem)
    p = pathlib.Path(a.dst) / 'no_depth.txt'
    p.write_text('\n'.join(out) + '\n', encoding='ascii')
    print(f'{len(out)} of {n} shaders cannot read the depth: {p}')


if __name__ == '__main__':
    main()
