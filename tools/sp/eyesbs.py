"""eyesbs.py: eye captures (<prefix>-L.rde / -R.rde, the core's "eyecap") as one quick side-by-side PNG, each eye cropped
to its rectangle in the post output (the headset's eye shape: [XR] EyeShape) and scaled to `width`; no mismatch
analysis (eyecap.py's block match is slow at headset sizes).

    python eyesbs.py <out.png> <prefix-or-dir> [...] [--width 1600] [--eye-width 2716]
"""
import argparse
import glob
import os

import numpy as np
from PIL import Image

import eyecap


def to_rgb(path, eye_w):
    r = eyecap.read_rde(path)
    a = np.asarray(r[0] if isinstance(r, tuple) else r)
    if eye_w:
        a = a[:, :eye_w]
    return Image.fromarray(a[..., :3].astype(np.uint8))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('src', nargs='+')
    ap.add_argument('--width', type=int, default=1600)
    ap.add_argument('--eye-width', type=int, default=0)
    a = ap.parse_args()
    rows = []
    for s in a.src:
        prefix = sorted(glob.glob(os.path.join(s, '*-L.rde')))[-1][:-6] if os.path.isdir(s) else s
        l, r = to_rgb(prefix + '-L.rde', a.eye_width), to_rgb(prefix + '-R.rde', a.eye_width)
        row = Image.new('RGB', (l.width + r.width + 8, max(l.height, r.height)))
        row.paste(l, (0, 0))
        row.paste(r, (l.width + 8, 0))
        rows.append(row.resize((a.width, int(row.height * a.width / row.width))))
    out = Image.new('RGB', (a.width, sum(r.height for r in rows)))
    y = 0
    for r in rows:
        out.paste(r, (0, y))
        y += r.height
    out.save(a.out)
    print(f'wrote {a.out}')


if __name__ == '__main__':
    main()
