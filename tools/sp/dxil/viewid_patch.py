"""viewid_patch.py: makes a game vertex shader view-instanced (single-pass stereo, docs/SINGLE_PASS_PLAN.md).

Input: the shader's DXIL as `dxc -dumpbin` text. Output: the same IR with
  - SV_ViewID read once at the entry (dx.op.viewID, opcode 138), the shader model raised to 6.1 (ViewID needs it);
  - each constant-row read of a view-dependent block redirected, in view 1, to eye 2's copy that the core appends past
    the block's declared end (the layout of prepass_arrays.cpp's grown buffers):
        rage_matrices  rows 4-15 (gWorldView, gWorldViewProj, gViewInverse) -> +13 (eye 2's rows 17-28)
        rage_prev_proj rows 0-3  (gPrevViewProj)                          -> +4  (eye 2's rows 4-7)
    rows 0-3 of rage_matrices (gWorldSingle, camera-relative per draw) and row 16 (gTAAJitter) are shared.
The result is assembled with dxa (research\\dxc-build\\bin\\dxa.exe), which validates and signs it.

Refuses (exit 2, the reason on stderr) a shader it cannot patch safely: a dynamic row index into a view-dependent block,
a view-dependent block bound through more than one range, or a shader that already uses ViewID.

    python viewid_patch.py in.ll out.ll
"""
import re
import sys

# block type name -> (rows that differ per eye, the row offset of eye 2's copy)
VIEW_BLOCKS = {
    'rage_matrices': (range(4, 16), 13),
    'rage_prev_proj': (range(0, 4), 4),
}
VIEWID_OPCODE = 138
VIEWID_FLAG = 1 << 21  # ShaderFlags: ViewID


class Refused(Exception):
    pass


def patch(text):
    if '@dx.op.viewID' in text:
        raise Refused('already uses ViewID')
    lines = text.split('\n')
    # range id -> block name, from the CBV entries of !dx.resources (names stripped; the struct type names the block)
    ranges = {}
    for m in re.finditer(r'^!\d+ = !\{i32 (\d+), %"?(?:hostlayout\.)?([^"*]+)"?\* undef, !"[^"]*", i32 \d+, i32 \d+, i32 \d+, i32 \d+, null\}',
                         text, re.M):
        if m.group(2) in VIEW_BLOCKS:
            ranges[m.group(1)] = m.group(2)
    if not ranges:
        raise Refused('no view-dependent block')
    handles = {}
    for m in re.finditer(r'(%\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 2, i32 (\d+), i32 (\S+), i1', text):
        if m.group(2) in ranges:
            handles[m.group(1)] = ranges[m.group(2)]
    if not handles:
        raise Refused('view-dependent block declared but never loaded')

    out, n_redirect, in_entry, inserted = [], 0, False, False
    load = re.compile(r'(@dx\.op\.cbufferLoadLegacy\.\w+\(i32 59, %dx\.types\.Handle (%\d+), i32 )([%\w.]+)\)')
    for ln in lines:
        if ln.startswith('define void @') and not inserted:
            out.append(ln)
            out.append(f'  %vid.sp = call i32 @dx.op.viewID.i32(i32 {VIEWID_OPCODE})')
            inserted = True
            in_entry = True
            continue
        m = load.search(ln) if in_entry else None
        if m and m.group(2) in handles:
            rows, shift = VIEW_BLOCKS[handles[m.group(2)]]
            idx = m.group(3)
            if idx.startswith('%'):
                raise Refused(f'dynamic row index into {handles[m.group(2)]}')
            r = int(idx)
            if r in rows:
                n_redirect += 1
                tmp = f'%row{n_redirect}.sp'
                out.append(f'  {tmp}.k = mul i32 %vid.sp, {shift}')
                out.append(f'  {tmp} = add i32 {tmp}.k, {r}')
                ln = ln[:m.start(3)] + tmp + ln[m.end(3):]
        if ln.startswith('}'):
            in_entry = False
        out.append(ln)
    if not inserted:
        raise Refused('no entry function')
    if n_redirect == 0:
        raise Refused('no view-dependent row read')
    text = '\n'.join(out)
    # the ViewID op's declaration, beside the other dx.op declarations (readnone, as the compiler declares it)
    ro = re.search(r'^attributes (#\d+) = \{ nounwind readnone \}', text, re.M)
    if not ro:
        raise Refused('no nounwind readnone attribute group')
    decl = f'declare i32 @dx.op.viewID.i32(i32) {ro.group(1)}'
    text = re.sub(r'^(declare )', decl + '\n\n\\1', text, count=1, flags=re.M)
    # shader model 6.1 and DXIL 1.1 at least
    def bump(m, minor_min=1):
        return f'{m.group(1)}{max(int(m.group(2)), minor_min)}{m.group(3)}'
    dxver = re.search(r'^!dx\.version = !\{(!\d+)\}', text, re.M)
    sm = re.search(r'^!dx\.shaderModel = !\{(!\d+)\}', text, re.M)
    if not dxver or not sm:
        raise Refused('no dx.version or dx.shaderModel')
    text = re.sub(r'^(' + re.escape(dxver.group(1)) + r' = !\{i32 1, i32 )(\d+)(\})', bump, text, flags=re.M)
    text = re.sub(r'^(' + re.escape(sm.group(1)) + r' = !\{!"vs", i32 6, i32 )(\d+)(\})', bump, text, flags=re.M)
    # the entry's shader flags (extended properties, tag 0) declare ViewID use (DXIL ShaderFlags bit 21)
    ep = re.search(r'^!dx\.entryPoints = !\{(!\d+)\}', text, re.M)
    epm = ep and re.search(r'^' + re.escape(ep.group(1)) + r' = !\{[^\n]*, (!\d+|null)\}$', text, re.M)
    if not epm or epm.group(1) == 'null':
        raise Refused('no entry properties')
    props = re.search(r'^' + re.escape(epm.group(1)) + r' = !\{(.*)\}$', text, re.M)
    flags = re.search(r'i32 0, i64 (\d+)', props.group(1)) if props else None
    if not flags:
        raise Refused('no shader flags entry in the entry properties')
    new_props = props.group(1).replace(flags.group(0), f'i32 0, i64 {int(flags.group(1)) | VIEWID_FLAG}', 1)
    text = text[:props.start(1)] + new_props + text[props.end(1):]
    return text, n_redirect


def main():
    src, dst = sys.argv[1], sys.argv[2]
    try:
        text, n = patch(open(src, encoding='utf-8', errors='replace').read())
    except Refused as e:
        print(f'refused: {e}', file=sys.stderr)
        return 2
    open(dst, 'w', encoding='utf-8', newline='\n').write(text)
    print(f'{n} row reads redirected')
    return 0


if __name__ == '__main__':
    sys.exit(main())
