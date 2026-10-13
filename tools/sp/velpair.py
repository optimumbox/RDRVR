"""velpair.py: the two eyes' motion vectors as their post runs read them ("grabburst <prefix> 1 <w> <h> vel"), compared.

Prints, per eye, the median vector of a few screen regions (sky, ground, right side) and the share of pixels where the
eyes differ by more than 1 px. A still or turning camera gives near-equal eyes (parallax only); a constant offset in
one eye is a wrong transform for that eye.

    python velpair.py <prefix> [...]
"""
import struct
import sys

import numpy as np

REGIONS = {'sky': (slice(20, 200), slice(1300, 2500)), 'ground': (slice(1100, 1400), slice(1400, 2400)),
           'right': (slice(300, 900), slice(1800, 2500))}


def read(path):
    b = open(path, 'rb').read()
    _, w, h, fmt, eye, frames, first, row = struct.unpack_from('<4sIIIiIII', b)
    if fmt != 34:
        raise SystemExit(f'{path}: not RG16F (format {fmt})')
    return np.frombuffer(b, np.uint8, row * h, 36).reshape(h, row)[:, :w * 4].view(np.float16).reshape(h, w, 2).astype(np.float32)


def main():
    for prefix in sys.argv[1:]:
        v = {e: read(f'{prefix}-{e}.rdb') for e in 'LR'}
        for e in 'LR':
            parts = ' '.join(f'{k} {np.round(np.median(v[e][r], axis=(0, 1)), 2).tolist()}' for k, r in REGIONS.items())
            print(f'{prefix} {e}: {parts}')
        d = np.hypot(*(v['R'] - v['L']).transpose(2, 0, 1))
        print(f'{prefix}: |R - L| > 1 px on {(d > 1).mean():.1%}')


if __name__ == '__main__':
    main()
