"""stereo_vs.py: the game's vertex and pixel shaders made view-instanced, in bulk.

For each vs_<hash>.dxbc and ps_<hash>.dxbc in IN (the core's "vi dump" output; a bare <hash>.dxbc is a vertex shader):
dumpbin -> viewid_patch -> dxa (assemble) -> extract the module -> dxopt (-viewid-state: the ViewID dependency tables the
validator checks) -> dxa -> dxv (the retail validator). Writes OUT/<vs|ps>_<hash>.dxil for each shader that validates,
OUT/ps_safe.txt (pixel shaders that read nothing camera-dependent: used as they are) and OUT/report.txt (one line per
shader: ok, safe, refused with the reason, or failed with the stage).

    python stereo_vs.py <in dir> <out dir> [--dxc-build research\\dxc-build\\bin] [--dxv research\\dxc\\x\\bin\\x64\\dxv.exe]
"""
import argparse
import pathlib
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import viewid_patch  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parents[3]
# Full-screen passes in the view-instanced region that read a screen image by an index of their own (a $Globals one,
# not a global_textures slot view 1 can be redirected by): view 1 would read eye 1's image (2026-10-13, the headset:
# the right eye saw eye 1's scene through windows and black bushes). No twin: each eye's region draws them itself.
# (The grass and foliage resolves have twins again: their images' indices are remapped, viewid_patch.py
# SCREEN_INDEX_FIELDS, so the recorder skip does not lose them.)
NO_TWIN_PS = {
    'PS_Blit': 'the refraction copy reads FullScreenCopy through TextureSampler',
    'PS_lightglow': 'the light glows test the scene depth through DepthBufferSampler (glows in eye 2 only)',
    'PSFootprintCollector': 'reads the scene depth through DepthBufferSampler',
    'PSFootprintColor': 'reads the scene depth through DepthBufferSampler',
    'PSMiragerSingle': 'the heat haze reads the scene copy through RenderMapBilinearSampler',
}
import os
for _n in filter(None, os.environ.get('RDRVR_NO_TWIN_EXTRA', '').split(',')):  # (experiments)
    NO_TWIN_PS[_n] = 'experiment'
# (found with tools/sp/dxil/read_samplers.py: the $Globals texture indices a pixel shader really uses)


def run(args, cwd=None):
    r = subprocess.run([str(a) for a in args], cwd=cwd, capture_output=True, text=True, errors='replace')
    return r.returncode, (r.stdout + r.stderr).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('src')
    ap.add_argument('dst')
    ap.add_argument('--dxc-build', default=ROOT / 'research' / 'dxc-build' / 'bin')
    ap.add_argument('--dxv', default=ROOT / 'research' / 'dxc' / 'x' / 'bin' / 'x64' / 'dxv.exe')
    a = ap.parse_args()
    bin_dir = pathlib.Path(a.dxc_build).resolve()
    dxc, dxa, dxopt = bin_dir / 'dxc.exe', bin_dir / 'dxa.exe', bin_dir / 'dxopt.exe'
    out = pathlib.Path(a.dst)
    out.mkdir(parents=True, exist_ok=True)
    report, safe = [], []
    counts = {'ok': 0, 'safe': 0, 'refused': 0, 'failed': 0}
    with tempfile.TemporaryDirectory() as tmp:
        t = pathlib.Path(tmp)
        for f in sorted(pathlib.Path(a.src).glob('*.dxbc')):
            kind, h = f.stem.split('_', 1) if '_' in f.stem else ('vs', f.stem)
            code, text = run([dxc, '-dumpbin', f])
            if code:
                report.append(f'{kind} {h} failed dumpbin: {text[:200]}')
                counts['failed'] += 1
                continue
            entry = re.search(r'^define void @(\w+)\(', text, re.M)
            if kind == 'ps' and entry and entry.group(1) in NO_TWIN_PS:
                # neither converted nor listed safe: no twin, so each eye's region draws it as the game's, with its own
                # pass's constants and images
                report.append(f'ps {h} no twin ({entry.group(1)}: {NO_TWIN_PS[entry.group(1)]})')
                counts['no twin'] = counts.get('no twin', 0) + 1
                continue
            try:
                patched, n, approx = viewid_patch.patch(text)
            except viewid_patch.Refused as e:
                if kind == 'ps' and str(e) == 'no view-dependent block':
                    safe.append(h)
                    report.append(f'ps {h} safe')
                    counts['safe'] += 1
                elif kind == 'vs' and str(e) == 'no view-dependent block':
                    # nothing camera-dependent (a full-screen pass): both views draw the same positions, so the game's
                    # own shader serves the twin (the pixel shader picks each eye's images)
                    (out / f'vs_{h}.dxil').write_bytes(f.read_bytes())
                    report.append(f'vs {h} shared as is')
                    counts['safe'] += 1
                else:
                    report.append(f'{kind} {h} refused: {e}')
                    counts['refused'] += 1
                continue
            ll, c1, bc, bc2, unsigned, fin = (t / f'{kind}{h}{s}' for s in ('.ll', '.1.dxbc', '.bc', '.2.bc', '.u.dxil', '.dxil'))
            ll.write_text(patched, encoding='utf-8', newline='\n')
            steps = [
                ('dxa', [dxa, ll, '-o', c1], None),
                ('extract', [dxa, c1, '-extractpart=module', '-o', bc], None),
                # dxopt loads dxcompiler.dll from its own folder: run it there, with absolute paths
                ('viewid-state', [dxopt, f'-o={bc2}', bc, '-hlsl-dxilload', '-viewid-state', '-hlsl-dxilemit'], bin_dir),
                ('dxa 2', [dxa, bc2, '-o', unsigned], None),
                # the retail validator validates and signs (the runtime refuses an unsigned container: E_INVALIDARG)
                ('dxv', [a.dxv, unsigned, f'-o={fin}'], None),
            ]
            failed = None
            for name, cmd, cwd in steps:
                code, text = run(cmd, cwd)
                if code or (name == 'dxv' and 'Validation succeeded' not in text):
                    failed = f'{name}: {text.splitlines()[0] if text else code}'
                    break
            if failed:
                report.append(f'{kind} {h} failed {failed}')
                counts['failed'] += 1
                continue
            signed = fin.read_bytes() if fin.exists() else b''
            if len(signed) < 20 or signed[4:20] == bytes(16):
                report.append(f'{kind} {h} failed sign: the container digest is empty')
                counts['failed'] += 1
                continue
            (out / f'{kind}_{h}.dxil').write_bytes(signed)
            report.append(f'{kind} {h} ok ({n} row reads{"; motion vectors through D" if approx else ""})')
            counts['ok'] += 1
    (out / 'ps_safe.txt').write_text(''.join(x + '\n' for x in safe), encoding='utf-8')
    (out / 'report.txt').write_text('\n'.join(report) + '\n', encoding='utf-8')
    print(f"ok {counts['ok']}, safe {counts['safe']}, no twin {counts.get('no twin', 0)}, refused {counts['refused']}, failed {counts['failed']} -> {out}")
    return 0


if __name__ == '__main__':
    sys.exit(main())
