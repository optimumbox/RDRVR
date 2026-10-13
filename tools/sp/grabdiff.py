# grabdiff.py <a> <b> [out.png]: per-pixel difference between two frame grabs (mean, 99th percentile, share of pixels
# off by more than 8/255), and an amplified difference image
import sys
import numpy as np
from PIL import Image
a = np.asarray(Image.open(sys.argv[1]).convert('RGB'), dtype=np.int16)
b = np.asarray(Image.open(sys.argv[2]).convert('RGB'), dtype=np.int16)
d = np.abs(a - b).max(axis=2)
print(f"mean {d.mean():.2f}, p99 {np.percentile(d, 99):.0f}, max {d.max()}, >8: {100 * (d > 8).mean():.2f}% of pixels")
if len(sys.argv) > 3:
    Image.fromarray(np.clip(d * 8, 0, 255).astype(np.uint8)).resize((a.shape[1] // 2, a.shape[0] // 2)).save(sys.argv[3])
