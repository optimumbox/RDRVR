"""eyecap.py: the eye captures (the core's "eyecap" / [Debug] EyeCapture, eye_capture.h) as images and a mismatch report.

For each <prefix>-L.rde / -R.rde pair in DIR (or the pairs named): <prefix>-L.png, -R.png, -sbs.png (side by side, half
size), -ana.png (red/cyan anaglyph) and, in DIR\\report.txt, the block match of the right eye against the left:
  - each 32x32 patch of the left image searched in the right within +-96 px horizontally and +-24 vertically;
  - dy: a correct stereo pair differs horizontally only (|dy| <= 1): vertical offsets mean a warped or misplaced eye;
  - dx: the right eye sees near things further left (dx <= 0); a patch whose best match is at dx 0 with large dx
    around it, or a positive dx, is suspect (an eye drawn from the other eye's camera);
  - unmatched: the best match is poor (something in one eye only).
Patches without texture (sky, flat colour) are skipped.

    python eyecap.py <dir> [prefix ...]
"""
import pathlib
import struct
import sys

import numpy as np
from PIL import Image

FORMATS = {28: ('rgba8', False), 29: ('rgba8', False), 87: ('bgra8', False), 91: ('bgra8', False), 24: ('rgb10a2', False),
           10: ('rgba16f', True), 2: ('rgba32f', True)}


def read_rde(path):
    b = pathlib.Path(path).read_bytes()
    magic, w, h, fmt, eye, frame, pitch, _ = struct.unpack_from('<4sIIIiIII', b, 0)
    if magic != b'RDE1':
        raise ValueError(f'{path}: not an RDE1 file')
    rows = np.frombuffer(b, dtype=np.uint8, count=pitch * h, offset=32).reshape(h, pitch)
    kind = FORMATS.get(fmt, (None,))[0]
    if kind in ('rgba8', 'bgra8'):
        px = rows[:, :w * 4].reshape(h, w, 4)
        rgb = px[..., :3] if kind == 'rgba8' else px[..., 2::-1]
    elif kind == 'rgb10a2':
        v = rows[:, :w * 4].copy().view('<u4').reshape(h, w)
        rgb = np.stack([(v >> s) & 1023 for s in (0, 10, 20)], -1).astype(np.float32) / 1023.0
        rgb = (rgb * 255 + 0.5).astype(np.uint8)
    elif kind == 'rgba16f':
        f = rows[:, :w * 8].copy().view('<f2').reshape(h, w, 4)[..., :3].astype(np.float32)
        rgb = (np.clip(f / (1 + f), 0, 1) ** (1 / 2.2) * 255 + 0.5).astype(np.uint8)  # (a rough tonemap for viewing)
    else:
        raise ValueError(f'{path}: DXGI format {fmt} not handled')
    return np.ascontiguousarray(rgb), {'w': w, 'h': h, 'fmt': fmt, 'eye': eye, 'frame': frame}


def match(left, right, patch=32, step=64, sx=96, sy=24):
    """block match the right eye against the left on a grey, contrast-normalised grid"""
    gl = left.astype(np.float32).mean(-1)
    gr = right.astype(np.float32).mean(-1)
    h, w = gl.shape
    res = []
    for y in range(sy + patch, h - sy - 2 * patch, step):
        for x in range(sx + patch, w - sx - 2 * patch, step):
            a = gl[y:y + patch, x:x + patch]
            if a.std() < 6:
                continue  # no texture
            a = (a - a.mean()) / a.std()
            win = gr[y - sy:y + sy + patch, x - sx:x + sx + patch]
            best = (-2.0, 0, 0)
            for dy in range(-sy, sy + 1, 2):
                for dx in range(-sx, sx + 1, 2):
                    b = win[dy + sy:dy + sy + patch, dx + sx:dx + sx + patch]
                    s = b.std()
                    if s < 1e-3:
                        continue
                    c = float(((b - b.mean()) / s * a).mean())
                    if c > best[0]:
                        best = (c, dx, dy)
            res.append((x, y) + best)
    return res


def report(prefix, out):
    left, ml = read_rde(f'{prefix}-L.rde')
    right, mr = read_rde(f'{prefix}-R.rde')
    Image.fromarray(left).save(f'{prefix}-L.png')
    Image.fromarray(right).save(f'{prefix}-R.png')
    sbs = Image.fromarray(np.concatenate([left, right], 1))
    sbs.resize((sbs.width // 2, sbs.height // 2)).save(f'{prefix}-sbs.png')
    ana = np.stack([left.mean(-1), right.mean(-1), right.mean(-1)], -1).astype(np.uint8)
    Image.fromarray(ana).save(f'{prefix}-ana.png')
    m = match(left, right)
    good = [r for r in m if r[2] > 0.6]
    bad = len(m) - len(good)
    dys = np.array([abs(r[4]) for r in good]) if good else np.array([0])
    dxs = np.array([r[3] for r in good]) if good else np.array([0])
    worst = sorted(good, key=lambda r: -abs(r[4]))[:5]
    line = (f'{pathlib.Path(prefix).name}: frames L {ml["frame"]} R {mr["frame"]}, {ml["w"]}x{ml["h"]} fmt {ml["fmt"]} | '
            f'patches {len(m)}, matched {len(good)}, unmatched {bad} | |dy| median {np.median(dys):.0f}, >2px: '
            f'{100 * (dys > 2).mean():.0f}% | dx median {np.median(dxs):.0f}, positive (wrong side) >4px: {100 * (dxs > 4).mean():.0f}%'
            f' | worst dy at ' + ', '.join(f'({x},{y}) dy {dy} dx {dx}' for x, y, c, dx, dy in worst))
    if ml['frame'] != mr['frame']:
        line += '  [eyes from different frames]'
    print(line)
    out.write(line + '\n')


def main():
    d = pathlib.Path(sys.argv[1])
    prefixes = [str(d / p) for p in sys.argv[2:]] or sorted({str(p)[:-6] for p in d.glob('*-L.rde')})
    with open(d / 'report.txt', 'a', encoding='utf-8') as out:
        for p in prefixes:
            try:
                report(p, out)
            except (OSError, ValueError) as e:
                print(f'{p}: {e}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
