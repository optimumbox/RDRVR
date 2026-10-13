# grab2png.py <grab> [out.png] [width]: a frame_grab file (BMP) to a scaled PNG for viewing
import sys
from PIL import Image
src = sys.argv[1]; out = sys.argv[2] if len(sys.argv) > 2 else src + '.png'; w = int(sys.argv[3]) if len(sys.argv) > 3 else 1280
im = Image.open(src).convert('RGB')
im.thumbnail((w, w)); im.save(out); print(out, im.size)
