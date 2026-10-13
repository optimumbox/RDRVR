"""depthdiff.py <a.rdz> <b.rdz> [out.png]: two scene-depth grabs ("grabdepth", frame_grab.h: "RDZ1", width, height,
format, then 32-bit texels; D24S8's depth in the low 24 bits) compared: the share of pixels whose depth differs, by how
much, and an image of where (white: differs by more than 1e-4 of the range).
"""
import struct
import sys

import numpy as np
from PIL import Image


def read(path):
    b = open(path, 'rb').read()
    magic, w, h, fmt = struct.unpack_from('<4sIII', b, 0)
    if magic != b'RDZ1':
        raise ValueError(f'{path}: not RDZ1')
    v = np.frombuffer(b, dtype='<u4', count=w * h, offset=16).reshape(h, w)
    if fmt in (45, 44, 46, 47):  # D24S8 family: depth in the low 24 bits
        d = (v & 0xFFFFFF).astype(np.float64) / 0xFFFFFF
    else:  # R32 float family
        d = v.view('<f4').astype(np.float64)
    return d, fmt


a, fa = read(sys.argv[1])
b, fb = read(sys.argv[2])
diff = np.abs(a - b)
far = (a >= 0.99999) & (b >= 0.99999)
print(f'format {fa}/{fb}: differs >1e-6: {100 * (diff > 1e-6).mean():.1f}%, >1e-4: {100 * (diff > 1e-4).mean():.1f}%, '
      f'>1e-2: {100 * (diff > 1e-2).mean():.1f}% | median of the differing {np.median(diff[diff > 1e-6]) if (diff > 1e-6).any() else 0:.2e}'
      f' | sky in both {100 * far.mean():.0f}%')
if len(sys.argv) > 3:
    Image.fromarray(((diff > 1e-4) * 255).astype(np.uint8)).resize((a.shape[1] // 2, a.shape[0] // 2)).save(sys.argv[3])
