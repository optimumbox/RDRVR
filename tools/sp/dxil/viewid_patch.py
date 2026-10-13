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

# block type name -> (rows that differ per eye, the row offset of eye 2's copy). gWorldViewProj (rows 8-11, per draw)
# has no eye-2 copy: view 1 builds eye 2's from the draw's gWorldSingle and eye 2's camera (patch_wvp).
VIEW_BLOCKS = {
    'rage_matrices': ([*range(4, 8), *range(12, 16)], 13),
    'rage_prev_proj': (range(0, 4), 4),
}
WVP_ROWS = range(8, 12)  # rage_matrices: gWorldViewProj = gWorldSingle x T(-camera) x gWorldView
WV2_ROW, CAM2_ROW = 17, 28  # rage_matrices: eye 2's gWorldView, eye 2's camera position (gViewInverse row 3)
# eye 2's inverse gWorldView, packed (a perspective's inverse: row 2 is (0, 0, 0, k), rows 0 and 1 end in 0):
# row 29 (inv[0].xyz, inv[2].w), row 30 (inv[1].xyz, 0), row 31 inv[3] (prepass_arrays.cpp write_inv_wv2)
INV2_ROW = 29
# Variables that are the inverse of (a world x) the camera's view-projection: view 1 reads
#   X2 = inverse(gWorldView2) x T(camera2 - camera1) x gWorldView1 x X1  (patch_inverse)
INVERSE_VARS = {'WorldViewProjInverse', 'WorldViewProjInverseTrees', 'InverseViewProj'}
# Blocks the same for both eyes in a view-instanced frame: the shadow camera (both eyes use pass 1's shadow maps)
SHARED_BLOCKS = {'rage_shadowmap_common'}
# Any other variable whose name says it depends on the camera, read from any block: refused (eye 2 would read eye 1's).
VIEW_NAME = re.compile(r'View(?!port)|Camera|Proj|Eye|FOV', re.I)
# ... except these: the same for both eyes, or handled above (scalars: a distance each)
VIEW_NAME_OK = {'gViewport', 'gBiasToCamera', 'gCameraDistanceAtMaxDisplacement', 'gNearCameraFade'}
# rage_prev_world's gPrevWorldViewProjSingle (rows 0-3, per draw: the motion vectors) is eye 1's in the shared rows; in
# view 1 each row read is multiplied by D (eye 1's previous view-projection inverted, times eye 2's; the core writes it
# to rage_matrices rows 21-24, eye 2's unused gWorldViewProj slot): eye 2's own previous-frame position, as the game's
# second pass computes it (KF2VR's lesson: each eye's own previous-to-current transform).
PREV_WORLD = 'rage_prev_world'
# The screen-space images the main pass reads through global_textures (a uint slot each: the bindless index into the
# pixel shaders' unbounded SRV table, t0 space2): 11 LightBufferSampler, 13 ShadowCollectorSampler, 15 the scene depth
# (DepthMapTexSampler / DepthTextureSampler0). In view 1 a shader reads eye 2's copy at table slot EYE2_SLOT_BASE + slot,
# where the core writes it (stereo_vi.cpp: the table's base from the game's own bind, the slots past what the game uses).
SCREEN_SLOTS = {11, 12, 13, 15}
# ... but a slot's image is the pass's: slot 15 is the sun's shadow map in the light pass (DepthTextureSampler0, read
# with a comparison sampler): a slot is redirected only when this shader names it as one of the screen images
SCREEN_SLOT_NAMES = {11: {'LightBufferSampler'}, 12: {'GlobalWaterSampler0'}, 13: {'ShadowCollectorSampler'}, 15: {'DepthMapTexSampler', 'RMPTFX_DepthMapTexSampler'}}


def slot_names(text):
    """{global_textures slot: the shader's name for it}"""
    m = re.search(r'^; cbuffer global_textures$(.*?)^;\s+\} global_textures;', text, re.M | re.S)
    return {int(o) // 4: n for n, o in re.findall(r'uint (\w+);\s+; Offset:\s+(\d+)', m.group(1))} if m else {}
EYE2_SLOT_BASE = 14960
# The scene depth itself, read by an index of the shader's own (a $Globals one): the game's depth view's index is in
# rage_matrices row 30 .w (a uint's bits; stereo_vi.h kDepthBufferSlot), eye 2's copy at EYE2_SLOT_BASE + 16.
DEPTH_BUFFER_SLOT, DEPTH_INDEX_ROW = 16, 30
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


def check_reads(text, allow_prev_world, found=None):
    """refuses a shader that reads a camera-dependent variable no eye-2 copy exists for; `found` (a dict) gets 'wvp'
    (gWorldViewProj read) and 'inverse' ({block: [first rows of its INVERSE_VARS read]})"""
    found = {} if found is None else found
    found.setdefault('wvp', False)
    found.setdefault('inverse', {})
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
            if block == 'rage_matrices' and not m.group(2).startswith('%') and int(m.group(2)) in WVP_ROWS:
                found['wvp'] = True
            continue
        if block in SHARED_BLOCKS:
            continue
        names = [(a, n) for a, b, n in lay.get(block, []) if m.group(2).startswith('%') or a <= int(m.group(2)) <= b]
        for a, n in names:
            if n in INVERSE_VARS:
                if m.group(2).startswith('%'):
                    raise Refused(f'dynamic row index into {block}.{n}')
                if a not in found['inverse'].setdefault(block, []):
                    found['inverse'][block].append(a)
                continue
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
    found = {}
    approx = check_reads(text, allow_prev_world, found)
    handles = {}
    for m in re.finditer(r'(%\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 2, i32 (\d+), i32 (\S+), i1', text):
        if m.group(2) in ranges:
            handles[m.group(1)] = ranges[m.group(2)]
    screen = screen_slot_reads(text)
    out, n_redirect, in_entry, inserted = [], 0, False, False
    load = re.compile(r'(@dx\.op\.cbufferLoadLegacy\.\w+\(i32 59, %dx\.types\.Handle (%\d+), i32 )([%\w.]+)\)')
    for ln in lines:
        if ln.startswith('define void @') and not inserted:
            out.append(ln)
            out.append(f'  %vid.sp = call i32 @dx.op.viewID.i32(i32 {VIEWID_OPCODE})')
            out.append('  %isv1.sp = icmp ne i32 %vid.sp, 0')
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
    out, n_screen = patch_screen_slots(out, screen)
    text = '\n'.join(out)
    text, n_wvp = patch_wvp(text) if found['wvp'] else (text, 0)
    text, n_inv = patch_inverse(text, found['inverse']) if found['inverse'] else (text, 0)
    text, n_remap = patch_index_remap(text, set(screen))
    text, n_ndc = patch_ndc(text)
    if n_redirect + n_screen + n_wvp + n_inv + n_remap + n_ndc == 0:
        raise Refused('no view-dependent block')
    n_redirect += n_screen + n_wvp + n_inv + n_remap + n_ndc
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


def screen_slot_reads(text):
    """the SRV handles whose index is a screen slot read from global_textures: {handle value: (index value, slot)};
    refuses an index computed from a screen slot (only a direct use can be redirected)"""
    rid = None
    for m in re.finditer(r'^!\d+ = !\{i32 (\d+), %"?(?:hostlayout\.)?([^"*]+)"?\* undef, !"[^"]*", i32 \d+, i32 \d+, i32 \d+, i32 \d+, null\}',
                         text, re.M):
        if m.group(2) == 'global_textures':
            rid = m.group(1)
    if rid is None:
        return {}
    hs = {m.group(1) for m in re.finditer(r'(%\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 2, i32 ' + rid + r',', text)}
    loads = {}
    for m in re.finditer(r'(%\d+) = call %dx\.types\.CBufRet\.i32 @dx\.op\.cbufferLoadLegacy\.i32\(i32 59, %dx\.types\.Handle (%\d+), i32 (\d+)\)', text):
        if m.group(2) in hs:
            loads[m.group(1)] = int(m.group(3))
    slot_of, names = {}, slot_names(text)
    for m in re.finditer(r'(%\d+) = extractvalue %dx\.types\.CBufRet\.i32 (%\d+), (\d)', text):
        if m.group(2) in loads:
            slot = loads[m.group(2)] * 4 + int(m.group(3))
            if slot in SCREEN_SLOTS and names.get(slot) in SCREEN_SLOT_NAMES[slot]:
                slot_of[m.group(1)] = slot
    if not slot_of:
        return {}
    # the compiler adds the range's base to the index (add i32 %slot, 0): followed (a non-zero constant would be the
    # range's lower bound, the same in eye 2's index)
    for m in re.finditer(r'^  (%\d+) = add i32 (%\d+), 0$', text, re.M):
        if m.group(2) in slot_of:
            slot_of[m.group(1)] = slot_of[m.group(2)]

    out = {}
    tok = re.compile(r'%\d+(?![\w.])')
    for ln in text.split('\n'):
        a = re.match(r'^  (%\d+) = add i32 (%\d+), 0$', ln)
        if a and a.group(2) in slot_of:
            continue  # (the alias above)
        m = re.match(r'^  (%\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 0, i32 \d+, i32 (%\d+), i1', ln)
        if m and m.group(2) in slot_of:
            out[m.group(1)] = (m.group(2), slot_of[m.group(2)])
            continue
        d = re.match(r'^  (%\d+) = ', ln)
        if d and any(t in slot_of for t in tok.findall(ln[d.end():].split('  ;')[0])):
            raise Refused("a screen slot's index used other than as a texture handle's index")
    return out


def patch_screen_slots(lines, screen):
    """view 1's handles of the screen slots take eye 2's table slot (EYE2_SLOT_BASE + slot)"""
    if not screen:
        return lines, 0
    out, n = [], 0
    for ln in lines:
        m = re.match(r'^  (%\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 0, i32 \d+, i32 (%\d+), i1', ln)
        if m and m.group(1) in screen:
            idx, slot = screen[m.group(1)]
            n += 1
            t = f'%scr{n}.sp'
            out.append(f'  {t}.v1 = icmp ne i32 %vid.sp, 0')
            out.append(f'  {t} = select i1 {t}.v1, i32 {EYE2_SLOT_BASE + slot}, i32 {idx}')
            ln = ln.replace(f', i32 {idx}, i1', f', i32 {t}, i1', 1)
        out.append(ln)
    return out, n


def range_ids(text):
    """{block type name: CBV range id}"""
    return {m.group(2): m.group(1) for m in re.finditer(
        r'^!\d+ = !\{i32 (\d+), %"?(?:hostlayout\.)?([^"*]+)"?\* undef, !"[^"]*", i32 \d+, i32 \d+, i32 \d+, i32 \d+, null\}', text, re.M)}


def cb_handles(text, rid):
    return sorted({m.group(1) for m in re.finditer(r'(%\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 2, i32 ' + rid + r',', text)},
                  key=lambda h: int(h[1:]))


F32_LOAD = re.compile(r'^  (%\d+) = call %dx\.types\.CBufRet\.f32 @dx\.op\.cbufferLoadLegacy\.f32\(i32 59, %dx\.types\.Handle (%\d+), i32 (\d+)\)')


def emit_row(out, t, h, row, past_end=False, kind='f32'):
    """a load of row `row` of the block behind handle h; its four components are t.c0..c3 (a row past the block's
    declared end is read through a dynamic index, view 0 reading row 0, unused: a constant one fails validation)"""
    idx = row
    if past_end:
        out.append(f'  {t}.i = select i1 %isv1.sp, i32 {row}, i32 0')
        idx = f'{t}.i'
    out.append(f'  {t} = call %dx.types.CBufRet.{kind} @dx.op.cbufferLoadLegacy.{kind}(i32 59, %dx.types.Handle {h}, i32 {idx})')
    for k in range(4):
        out.append(f'  {t}.c{k} = extractvalue %dx.types.CBufRet.{kind} {t}, {k}')
    return [f'{t}.c{k}' for k in range(4)]


def emit_vec_mat(out, t, v, rows):
    """v x M, the row vector v (None entries: zero) times the matrix whose rows are `rows`: four value names"""
    res = []
    for k in range(4):
        acc = None
        for j in range(4):
            if v[j] is None:
                continue
            out.append(f'  {t}.m{j}c{k} = fmul float {v[j]}, {rows[j][k]}')
            if acc is None:
                acc = f'{t}.m{j}c{k}'
            else:
                out.append(f'  {t}.a{j}c{k} = fadd float {acc}, {t}.m{j}c{k}')
                acc = f'{t}.a{j}c{k}'
        res.append(acc)
    return res


def alias_loads(out, rename):
    """the original component reads of each renamed load (load value -> t) now read t.s0..s3 (the original reads stay,
    unused: removing them would renumber the unnamed values after them)"""
    ext = re.compile(r'^  (%\d+) = extractvalue %dx\.types\.CBufRet\.f32 (%\d+), (\d)$')
    alias, defs = {}, set()
    for i, ln in enumerate(out):
        m = ext.match(ln.split('  ;')[0].rstrip())
        if m and m.group(2) in rename:
            alias[m.group(1)] = f'{rename[m.group(2)]}.s{m.group(3)}'
            defs.add(i)
    tok = re.compile(r'%\d+(?![\w.])')
    return chr(10).join(ln if i in defs else tok.sub(lambda mm: alias.get(mm.group(0), mm.group(0)), ln) for i, ln in enumerate(out))


def select_rows(out, t, L, r):
    """t.s0..s3: view 1 the values r, view 0 the load L's own"""
    for k in range(4):
        out.append(f'  {t}.e{k} = extractvalue %dx.types.CBufRet.f32 {L}, {k}')
        out.append(f'  {t}.s{k} = select i1 %isv1.sp, float {r[k]}, float {t}.e{k}')


def patch_wvp(text):
    """view 1's reads of rage_matrices rows 8-11 (gWorldViewProj, per draw) become eye 2's, from the draw's own
    gWorldSingle: row i = (gWorldSingle row i - its w x camera2) x gWorldView2"""
    hs = cb_handles(text, range_ids(text)['rage_matrices'])
    hm = hs[0]
    out, rename, n = [], {}, 0
    for ln in text.split(chr(10)):
        m = F32_LOAD.match(ln)
        out.append(ln)
        if not m or m.group(2) not in hs or int(m.group(3)) not in WVP_ROWS:
            continue
        n += 1
        L, t, i = m.group(1), f'%wvp{n}.sp', int(m.group(3)) - WVP_ROWS[0]
        w = emit_row(out, f'{t}.w', hm, i)
        c2 = emit_row(out, f'{t}.cam', hm, CAM2_ROW, True)
        wv2 = [emit_row(out, f'{t}.wv{j}', hm, WV2_ROW + j, True) for j in range(4)]
        v = []
        for k in range(3):
            out.append(f'  {t}.o{k} = fmul float {w[3]}, {c2[k]}')
            out.append(f'  {t}.v{k} = fsub float {w[k]}, {t}.o{k}')
            v.append(f'{t}.v{k}')
        v.append(w[3])
        select_rows(out, t, L, emit_vec_mat(out, f'{t}.x', v, wv2))
        rename[L] = t
    return (alias_loads(out, rename), n) if n else (text, 0)


def patch_inverse(text, inverse):
    """view 1's reads of an inverse view-projection X (INVERSE_VARS, rows f..f+3 of its block) become eye 2's:
    X2 row i = (P row i + its w x (camera2 - camera1)) x gWorldView1 x X1, P = inverse(gWorldView2) (packed, INV2_ROW)"""
    rid = range_ids(text)
    if 'rage_matrices' not in rid:
        raise Refused('reads an inverse view-projection without rage_matrices')
    hm = cb_handles(text, rid['rage_matrices'])[0]
    firsts = {}  # handle -> first rows of its inverse matrices
    for block, f in inverse.items():
        for h in cb_handles(text, rid[block]):
            firsts[h] = f
    out, rename, n = [], {}, 0
    for ln in text.split(chr(10)):
        m = F32_LOAD.match(ln)
        out.append(ln)
        if not m or m.group(2) not in firsts:
            continue
        row = int(m.group(3))
        first = next((f for f in firsts[m.group(2)] if f <= row < f + 4), None)
        if first is None:
            continue
        n += 1
        L, t, i = m.group(1), f'%inv{n}.sp', row - first
        x1 = [emit_row(out, f'{t}.x{j}', m.group(2), first + j) for j in range(4)]
        wv1 = [emit_row(out, f'{t}.wv{j}', hm, 4 + j) for j in range(4)]
        if i in (0, 1):  # P row i = (inv[i].xyz, 0): no translation
            pr = emit_row(out, f'{t}.p', hm, INV2_ROW + i, True)
            v = [pr[0], pr[1], pr[2], None]
        else:  # row 2 (0, 0, 0, inv[2].w: row 29's w), row 3 (row 31)
            pr = emit_row(out, f'{t}.p', hm, INV2_ROW if i == 2 else INV2_ROW + 2, True)
            pxyz = [None] * 3 if i == 2 else pr[:3]
            c1 = emit_row(out, f'{t}.c1', hm, 15)
            c2 = emit_row(out, f'{t}.c2', hm, CAM2_ROW, True)
            v = []
            for k in range(3):
                out.append(f'  {t}.d{k} = fsub float {c2[k]}, {c1[k]}')
                out.append(f'  {t}.dw{k} = fmul float {pr[3]}, {t}.d{k}')
                if pxyz[k] is None:
                    v.append(f'{t}.dw{k}')
                else:
                    out.append(f'  {t}.v{k} = fadd float {pxyz[k]}, {t}.dw{k}')
                    v.append(f'{t}.v{k}')
            v.append(pr[3])
        q = emit_vec_mat(out, f'{t}.q', v, wv1)
        select_rows(out, t, L, emit_vec_mat(out, f'{t}.y', q, x1))
        rename[L] = t
    return (alias_loads(out, rename), n) if n else (text, 0)


def patch_index_remap(text, done):
    """view 1's bindless 2D texture indices from any source (a material's own index too) equal to the game's scene-depth
    view's (rage_matrices row 30 .w, written by the core) read eye 2's copy. (Not compared with global_textures' values:
    a slot holds the pass's image, the sun's shadow map in the light pass.)"""
    rid = range_ids(text)
    srv = re.search(r'^!\d+ = !\{i32 (\d+), \[0 x %"class\.Texture2D<[^"]*>"\]\* undef, !"[^"]*", i32 \d+, i32 0, i32 -1, i32 2,', text, re.M)
    hm = cb_handles(text, rid['rage_matrices']) if 'rage_matrices' in rid else []
    if not srv or not hm:
        return text, 0
    hre = re.compile(r'^  (%\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 0, i32 ' + srv.group(1) + r', i32 (%\d+), i1')
    out, n = [], 0
    for ln in text.split(chr(10)):
        m = hre.match(ln)
        if not m or m.group(1) in done:
            out.append(ln)
            continue
        n += 1
        t, idx = f'%rmp{n}.sp', m.group(2)
        dr = emit_row(out, f'{t}.dr', hm[0], DEPTH_INDEX_ROW, True)
        out.append(f'  {t}.di = bitcast float {dr[3]} to i32')
        out.append(f'  {t}.qd = icmp eq i32 {idx}, {t}.di')
        out.append(f'  {t}.bd = and i1 {t}.qd, %isv1.sp')
        out.append(f'  {t}.nd = select i1 {t}.bd, i32 {EYE2_SLOT_BASE + DEPTH_BUFFER_SLOT}, i32 {idx}')
        cur = f'{t}.nd'
        out.append(ln.replace(f', i32 {idx}, i1', f', i32 {cur}, i1', 1))
    return chr(10).join(out), n


# The velocity pass (rdr2_taa's PSGenerateVelocityBuffer: NDCToPrevNDC and DepthMapSampler in its $Globals, nothing
# else camera-dependent) for eye 2: view 1 reads eye 2's NDCToPrevNDC and eye 2's scene depth. The core
# (prepass_arrays.cpp set_eye2_ndc) grows the effect's $Globals 0xf0 -> 0x100 (the upload's size) and writes, in its
# padding, the matrix's x, y and w columns (the only ones the pass reads) and the game's two depth views' indices
# plus one (0: not written, compares with nothing).
NDC_GLOBALS = {'NDCToPrevNDC': 128, 'DepthMapSampler': 68, 'TexelSize': 32}
NDC_SIZE = 228
NDC_COLS = (0, 1, 3)
NDC_SPARE = [(15, 0), (15, 1), (15, 2), (15, 3), (7, 1), (7, 2), (7, 3), (14, 1), (14, 2), (14, 3), (12, 2), (12, 3)]
NDC_DEPTH_ROW = 1  # .y: the scene depth buffer's view index + 1 (eye 2's copy: slot DEPTH_BUFFER_SLOT), .z: the depth
NDC_DEPTH_SLOTS = (DEPTH_BUFFER_SLOT, 15)  # resolve's (slot 15)


def ndc_layout(text):
    m = re.search(r'^; cbuffer \$Globals$(.*?)^;\s+\} \$Globals;\s+; Offset:\s+0 Size:\s+(\d+)', text, re.M | re.S)
    if not m or int(m.group(2)) != NDC_SIZE:
        return False
    offs = {n: int(o) for n, o in re.findall(r' (\w+);\s+; Offset:\s+(\d+)', m.group(1))}
    return all(offs.get(n) == o for n, o in NDC_GLOBALS.items())


def patch_ndc(text):
    """view 1's NDCToPrevNDC and scene depth as eye 2's, in a shader with the velocity pass's $Globals layout"""
    if not ndc_layout(text):
        return text, 0
    rid = range_ids(text)
    srv = re.search(r'^!\d+ = !\{i32 (\d+), \[0 x %"class\.Texture2D<[^"]*>"\]\* undef, !"[^"]*", i32 \d+, i32 0, i32 -1, i32 2,', text, re.M)
    hg = cb_handles(text, rid['$Globals']) if '$Globals' in rid else []
    if not hg:
        return text, 0
    base = NDC_GLOBALS['NDCToPrevNDC'] // 16
    out, rename, n = [], {}, 0
    for ln in text.split(chr(10)):
        m = F32_LOAD.match(ln)
        out.append(ln)
        if not m or m.group(2) not in hg or not base <= int(m.group(3)) < base + 4:
            continue
        j = int(m.group(3)) - base
        n += 1
        L, t = m.group(1), f'%ndc{n}.sp'
        sel = {}
        for k, c in enumerate(NDC_COLS):
            row, comp = NDC_SPARE[j * 3 + k]
            v = emit_row(out, f'{t}.r{k}', m.group(2), row, past_end=row * 16 >= NDC_SIZE)
            sel[c] = v[comp]
        for c in range(4):
            out.append(f'  {t}.e{c} = extractvalue %dx.types.CBufRet.f32 {L}, {c}')
            if c in sel:
                out.append(f'  {t}.s{c} = select i1 %isv1.sp, float {sel[c]}, float {t}.e{c}')
            else:
                out.append(f'  {t}.s{c} = select i1 %isv1.sp, float 0.000000e+00, float {t}.e{c}')
        rename[L] = t
    ext = re.compile(r'^  (%\d+) = extractvalue %dx\.types\.CBufRet\.f32 (%\d+), (\d)$')
    alias, defs = {}, set()
    for i, ln in enumerate(out):
        mm = ext.match(ln.split('  ;')[0].rstrip())
        if mm and mm.group(2) in rename:
            alias[mm.group(1)] = f'{rename[mm.group(2)]}.s{mm.group(3)}'
            defs.add(i)
    tok = re.compile(r'%\d+(?![\w.])')
    out = [ln if i in defs else tok.sub(lambda q: alias.get(q.group(0), q.group(0)), ln) for i, ln in enumerate(out)]
    # the scene depth: view 1's 2D texture index equal to a depth view's (the core's value minus one) reads eye 2's copy
    n_depth = 0
    if srv:
        hre = re.compile(r'^  (%\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 0, i32 ' + srv.group(1) + r', i32 (%\d+), i1')
        res = []
        for ln in out:
            m = hre.match(ln)
            if not m:
                res.append(ln)
                continue
            n_depth += 1
            t, cur = f'%ndd{n_depth}.sp', m.group(2)
            d = emit_row(res, f'{t}.d', hg[0], NDC_DEPTH_ROW, kind='i32')
            for s, slot in enumerate(NDC_DEPTH_SLOTS):
                res.append(f'  {t}.v{s} = sub i32 {d[1 + s]}, 1')
                res.append(f'  {t}.q{s} = icmp eq i32 {m.group(2)}, {t}.v{s}')
                res.append(f'  {t}.b{s} = and i1 {t}.q{s}, %isv1.sp')
                res.append(f'  {t}.n{s} = select i1 {t}.b{s}, i32 {EYE2_SLOT_BASE + slot}, i32 {cur}')
                cur = f'{t}.n{s}'
            res.append(ln.replace(f', i32 {m.group(2)}, i1', f', i32 {cur}, i1', 1))
        out = res
    return chr(10).join(out), n + n_depth


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
