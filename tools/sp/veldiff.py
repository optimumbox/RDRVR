"""veldiff.py: the two slices of a "prepass grab velocity" file compared (eye 2's motion vectors against eye 1's).

For a still camera both eyes' motion vectors should agree: prints the share of pixels whose vectors differ by more than
0.25 and 1 px, the median difference over those, and writes <file>-diff.png (red: eye 2 further right than eye 1,
green: further down; grey where they agree) beside eye 1's |v|.

    python veldiff.py <file.rda> [...]
"""
import sys

import numpy as np
from PIL import Image

import rda2png


def main():
    for path in sys.argv[1:]:
        w, h, fmt, s = rda2png.read(path)
        if fmt != 34:
            raise SystemExit(f'{path}: not RG16F velocity (format {fmt})')
        v = [a[:, :w * 4].view(np.float16).reshape(h, w, 2).astype(np.float32) for a in s]
        d = np.nan_to_num(v[1] - v[0])
        m = np.hypot(d[..., 0], d[..., 1])
        big = m > 0.25
        print(f'{path}: |eye2 - eye1| > 0.25 px on {big.mean():.1%}, > 1 px on {(m > 1).mean():.1%}; '
              f'median over those {np.median(d[big], axis=0) if big.any() else "-"}')
        img = np.full((h, w, 3), 128, np.uint8)
        img[..., :2] = np.clip(128 + d * 64, 0, 255).astype(np.uint8)
        Image.fromarray(img).resize((w // 2, h // 2)).save(path.rsplit('.', 1)[0] + '-diff.png')


if __name__ == '__main__':
    main()
