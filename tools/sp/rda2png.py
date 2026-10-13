"""rda2png.py: the core's "prepass grab <kind> <path>" file (RDA1: both slices of a pre-pass array) as PNGs.

<path>-s0.png (slice 0, eye 1), <path>-s1.png (slice 1, eye 2: what eye 2's copy got), <path>-sbs.png, and per-slice
statistics (mean, min, max of each channel; the share of texels equal to the slice's most common value).

    python rda2png.py <file.rda> [...]
"""
import struct
import sys

import numpy as np
from PIL import Image

# DXGI format -> (bytes per texel, channels as (dtype, count), how to turn into 8-bit RGB)
BGRA8 = {87, 88, 90, 91}
RGBA8 = {27, 28, 29}


def read(path):
    b = open(path, 'rb').read()
    magic, w, h, fmt, row = struct.unpack_from('<5I', b)
    if magic != 0x31414452:
        raise SystemExit(f'{path}: not an RDA1 file')
    n = row * h
    s = [np.frombuffer(b, np.uint8, n, 20 + i * n).reshape(h, row) for i in range(2)]
    return w, h, fmt, s


def to_rgb(w, fmt, a):
    if fmt in BGRA8 | RGBA8:
        px = a[:, :w * 4].reshape(a.shape[0], w, 4)
        rgb = px[..., [2, 1, 0]] if fmt in BGRA8 else px[..., :3]
        return rgb, px
    if fmt in (10,):  # rgba16f
        px = a[:, :w * 8].view(np.float16).reshape(a.shape[0], w, 4).astype(np.float32)
        return np.clip(px[..., :3] * 255, 0, 255).astype(np.uint8), px
    if fmt == 34:  # rg16f (velocity): x in red, y in green, around grey; scaled to the 99th percentile of |v|
        px = a[:, :w * 4].view(np.float16).reshape(a.shape[0], w, 2).astype(np.float32)
        s = max(float(np.nanpercentile(np.abs(px), 99)), 1e-6)
        rgb = np.zeros(px.shape[:2] + (3,), np.uint8)
        rgb[..., :2] = np.clip(128 + np.nan_to_num(px) / s * 127, 0, 255).astype(np.uint8)
        rgb[..., 2] = 128
        return rgb, px
    if fmt in (39, 40, 41, 45, 46, 19, 20):  # 32-bit: depth-ish, shown as normalised grey
        px = a[:, :w * 4].view(np.uint32).reshape(a.shape[0], w)
        v = (px & 0xffffff).astype(np.float64) if fmt in (39, 45, 46) else px.view(np.float32).astype(np.float64)
        lo, hi = np.percentile(v, 1), np.percentile(v, 99)
        g = np.clip((v - lo) / max(hi - lo, 1e-12) * 255, 0, 255).astype(np.uint8)
        return np.stack([g] * 3, -1), v[..., None]
    raise SystemExit(f'format {fmt} not handled')


def main():
    for path in sys.argv[1:]:
        w, h, fmt, s = read(path)
        base = path.rsplit('.', 1)[0]
        imgs = []
        for i, a in enumerate(s):
            rgb, px = to_rgb(w, fmt, a)
            Image.fromarray(rgb).save(f'{base}-s{i}.png')
            imgs.append(rgb)
            flat = px.reshape(-1, px.shape[-1])
            vals, counts = np.unique(flat, axis=0, return_counts=True)
            print(f'{base} slice {i}: {w}x{h} fmt {fmt} | mean {np.round(flat.mean(0), 3).tolist()} min {flat.min(0).tolist()} '
                  f'max {flat.max(0).tolist()} | most common {vals[counts.argmax()].tolist()} on {counts.max() / len(flat):.1%}')
        sbs = np.concatenate(imgs, 1)
        Image.fromarray(sbs).resize((sbs.shape[1] // 2, sbs.shape[0] // 2)).save(f'{base}-sbs.png')


if __name__ == '__main__':
    main()
