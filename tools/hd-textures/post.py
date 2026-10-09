# Crop the padding off the upscaled PNGs and name them for sswrap's load folder.
import sys, os, re
from PIL import Image
src, dst, scale = sys.argv[1], sys.argv[2], int(sys.argv[3])
os.makedirs(dst, exist_ok=True)
for f in os.listdir(src):
    m = re.match(r'(.+)__p(\d+)\.png$', f)
    if not m: continue
    p = int(m.group(2)) * scale
    im = Image.open(os.path.join(src, f))
    im.crop((p, p, im.width - p, im.height - p)).save(os.path.join(dst, m.group(1) + '.png'), optimize=True)
