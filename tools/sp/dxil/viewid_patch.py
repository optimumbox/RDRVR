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

# block type name -> (rows that differ per eye, the row offset of eye 2's copy). gWorldViewProj (rows 8-11) has no eye-2
# copy yet (PrepassViews=2 writes gWorldView and gViewInverse): a shader reading it is refused.
VIEW_BLOCKS = {
    'rage_matrices': ([*range(4, 8), *range(12, 16)], 13),
    'rage_prev_proj': (range(0, 4), 4),
}
REFUSED_ROWS = {'rage_matrices': range(8, 12)}
# Any other variable whose name says it depends on the camera, read from any block: refused (eye 2 would read eye 1's).
VIEW_NAME = re.compile(r'View(?!port)|Camera|Proj|Eye|FOV', re.I)
# ... except these: the same for both eyes, or handled above
VIEW_NAME_OK = {'gViewport'}
# rage_prev_world's gPrevWorldViewProjSingle (rows 0-3, per draw: the motion vectors) is eye 1's in the shared rows; in
# view 1 each row read is multiplied by D (eye 1's previous view-projection inverted, times eye 2's; the core writes it
# to rage_matrices rows 21-24, eye 2's unused gWorldViewProj slot): eye 2's own previous-frame position, as the game's
# second pass computes it (KF2VR's lesson: each eye's own previous-to-current transform).
PREV_WORLD = 'rage_prev_world'
PREV_WORLD_ROWS = range(0, 4)
DELTA_ROW = 21  # rage_matrices
VIEWID_OPCODE = 138
VIEWID_FLAG = 1 << 21  # ShaderFlags: ViewID


class Refused(Exception):
    pass


def layouts(text):
    """{block type name: [(first row, last row, variable name)]} from dumpbin's buffer definitions"""
    out, cur, entries = {}, None, []
    for ln in text.split('\n'):
        m = re.match(r'^; cbuffer (\S+)$', ln)
        if m:
            cur, entries = m.group(1), []
            continue
        if cur:
            v = re.match(r'^;\s+(?:row_major |column_major )?[\w$.]+\s+([\w$]+)(\[\d+\])?;\s+; Offset:\s+(\d+)', ln)
            if v:
                entries.append((int(v.group(3)), v.group(1)))
            e = re.match(r'^;\s+\} [\w$]+;\s+; Offset:\s+\d+ Size:\s+(\d+)', ln)
            if e:
                size = int(e.group(1))
                rows = []
                for i, (off, name) in enumerate(entries):
                    end = entries[i + 1][0] - 1 if i + 1 < len(entries) else size - 1
                    rows.append((off // 16, max(off, end) // 16, name))
                out[cur] = rows
                cur = None
    return out


def check_reads(text, allow_prev_world):
    """refuses a shader that reads a camera-dependent variable no eye-2 copy exists for"""
    lay = layouts(text)
    ranges = {}
    for m in re.finditer(r'^!\d+ = !\{i32 (\d+), %"?(?:hostlayout\.)?([^"*]+)"?\* undef, !"[^"]*", i32 \d+, i32 \d+, i32 \d+, i32 \d+, null\}',
                         text, re.M):
        ranges[m.group(1)] = m.group(2)
    handles = {}
    for m in re.finditer(r'(%\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 2, i32 (\d+),', text):
        handles[m.group(1)] = ranges.get(m.group(2), '?')
    prev_world = False
    for m in re.finditer(r'@dx\.op\.cbufferLoadLegacy\.\w+\(i32 59, %dx\.types\.Handle (%\d+), i32 ([%\w.]+)\)', text):
        block = handles.get(m.group(1), '?')
        if block in VIEW_BLOCKS:
            if not m.group(2).startswith('%') and int(m.group(2)) in REFUSED_ROWS.get(block, ()):
                raise Refused(f'reads {block} row {m.group(2)} (no eye-2 copy)')
            continue
        names = [n for a, b, n in lay.get(block, []) if m.group(2).startswith('%') or a <= int(m.group(2)) <= b]
        for n in names:
            if VIEW_NAME.search(n) and n not in VIEW_NAME_OK:
                if block == PREV_WORLD and not m.group(2).startswith('%') and int(m.group(2)) in PREV_WORLD_ROWS:
                    prev_world = True  # handled: patch_prev_world
                    continue
                raise Refused(f'reads {block}.{n} (camera-dependent, no eye-2 copy)')
    return prev_world


def patch(text, allow_prev_world=True):  # (allow_prev_world: kept for callers; rage_prev_world is handled now)
    if '@dx.op.viewID' in text:
        raise Refused('already uses ViewID')
    lines = text.split('\n')
    # range id -> block name, from the CBV entries of !dx.resources (names stripped; the struct type names the block)
    ranges = {}
    for m in re.finditer(r'^!\d+ = !\{i32 (\d+), %"?(?:hostlayout\.)?([^"*]+)"?\* undef, !"[^"]*", i32 \d+, i32 \d+, i32 \d+, i32 \d+, null\}',
                         text, re.M):
        if m.group(2) in VIEW_BLOCKS:
            ranges[m.group(1)] = m.group(2)
    approx = check_reads(text, allow_prev_world)
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
        raise Refused('no view-dependent block')
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
    text = re.sub(r'^(' + re.escape(sm.group(1)) + r' = !\{!"(?:vs|ps)", i32 6, i32 )(\d+)(\})', bump, text, flags=re.M)
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
    if approx:
        text = patch_prev_world(text)
    return text, n_redirect, approx


def patch_prev_world(text):
    """view 1's reads of rage_prev_world rows 0-3 become row x D (D: rage_matrices rows 21-24); view 0's unchanged"""
    rid = {}
    for m in re.finditer(r'^!\d+ = !\{i32 (\d+), %"?(?:hostlayout\.)?([^"*]+)"?\* undef, !"[^"]*", i32 \d+, i32 \d+, i32 \d+, i32 \d+, null\}',
                         text, re.M):
        rid[m.group(2)] = m.group(1)
    if 'rage_matrices' not in rid or PREV_WORLD not in rid:
        raise Refused('reads rage_prev_world without rage_matrices')
    hm = re.search(r'(%\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 2, i32 ' + rid['rage_matrices'] + r',', text)
    hp = {m.group(1) for m in re.finditer(r'(%\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 2, i32 '
                                          + rid[PREV_WORLD] + r',', text)}
    if not hm or not hp:
        raise Refused('rage_prev_world or rage_matrices handle not found')
    hm = hm.group(1)
    lines = text.split(chr(10))
    load = re.compile(r'^  (%\d+) = call %dx\.types\.CBufRet\.f32 @dx\.op\.cbufferLoadLegacy\.f32\(i32 59, %dx\.types\.Handle (%\d+), i32 (\d+)\)')
    out, rename, n = [], {}, 0
    for i, ln in enumerate(lines):
        if ln.startswith('  %vid.sp = '):
            out.append(ln)
            out.append('  %isv1.sp = icmp ne i32 %vid.sp, 0')
            continue
        m = load.match(ln)
        out.append(ln)
        if not m or m.group(2) not in hp or int(m.group(3)) not in PREV_WORLD_ROWS:
            continue
        n += 1
        L, t = m.group(1), f'%pw{n}.sp'
        e = [f'{t}.e{k}' for k in range(4)]
        for k in range(4):
            out.append(f'  {e[k]} = extractvalue %dx.types.CBufRet.f32 {L}, {k}')
        for j in range(4):
            # (a constant row past the block's declared end fails validation: view 0 reads row j instead, unused)
            out.append(f'  {t}.r{j} = select i1 %isv1.sp, i32 {DELTA_ROW + j}, i32 {j}')
            out.append(f'  {t}.d{j} = call %dx.types.CBufRet.f32 @dx.op.cbufferLoadLegacy.f32(i32 59, %dx.types.Handle {hm}, i32 {t}.r{j})')
            for k in range(4):
                out.append(f'  {t}.d{j}c{k} = extractvalue %dx.types.CBufRet.f32 {t}.d{j}, {k}')
        for k in range(4):  # (row x D)[k] = sum_j row[j] D[j][k]
            out.append(f'  {t}.m0c{k} = fmul float {e[0]}, {t}.d0c{k}')
            for j in range(1, 4):
                out.append(f'  {t}.m{j}c{k} = fmul float {e[j]}, {t}.d{j}c{k}')
                prev = f'{t}.m0c{k}' if j == 1 else f'{t}.a{j - 1}c{k}'
                out.append(f'  {t}.a{j}c{k} = fadd float {prev}, {t}.m{j}c{k}')
            out.append(f'  {t}.s{k} = select i1 %isv1.sp, float {t}.a3c{k}, float {e[k]}')
        rename[L] = t
    if not n:
        return text
    # the original component reads of each such load now read the selected values
    ext = re.compile(r'^  (%\d+) = extractvalue %dx\.types\.CBufRet\.f32 (%\d+), (\d)$')
    # (the original reads stay, now unused: removing them would renumber the unnamed values after them)
    alias, defs = {}, set()
    for i, ln in enumerate(out):
        m = ext.match(ln.split('  ;')[0].rstrip())
        if m and m.group(2) in rename:
            alias[m.group(1)] = f'{rename[m.group(2)]}.s{m.group(3)}'
            defs.add(i)
    tok = re.compile(r'%\d+(?![\w.])')
    return chr(10).join(ln if i in defs else tok.sub(lambda mm: alias.get(mm.group(0), mm.group(0)), ln) for i, ln in enumerate(out))


def main():
    src, dst = sys.argv[1], sys.argv[2]
    try:
        text, n, _ = patch(open(src, encoding='utf-8', errors='replace').read())
    except Refused as e:
        print(f'refused: {e}', file=sys.stderr)
        return 2
    open(dst, 'w', encoding='utf-8', newline='\n').write(text)
    print(f'{n} row reads redirected')
    return 0


if __name__ == '__main__':
    sys.exit(main())
