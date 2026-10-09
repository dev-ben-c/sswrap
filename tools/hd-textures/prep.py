# Pad each texture before upscaling so its edges come out right, then write PNGs for Real-ESRGAN.
# Per axis: if the texture tiles along that axis (its opposite edges continue into each other,
# like terrain), pad by wrapping; otherwise (skies, skins, logos) pad by extending the edge, so
# unrelated content from the far side never bleeds in.
# Usage: prep.py <dumpdir> <listfile> <outdir>
import sys, os, numpy as np
from PIL import Image
dump, lst, out = sys.argv[1:4]

def tiles(a, axis):
    """True if the texture continues across its opposite edges along `axis`."""
    a = a[..., :3].astype(np.float32)
    first, last = np.take(a, 0, axis), np.take(a, -1, axis)
    across = np.abs(first - last).mean()                       # jump across the wrap seam
    inner = np.abs(np.diff(a, axis=axis)).mean()               # typical jump between neighbours
    return across <= max(2.0 * inner, 6.0)

wrapped = edged = 0
for f in open(lst).read().split():
    im = np.asarray(Image.open(os.path.join(dump, f)).convert('RGBA'))
    h, w, _ = im.shape
    p = max(4, min(w, h) // 8)
    ty, tx = tiles(im, 0), tiles(im, 1)
    im = np.pad(im, ((p, p), (0, 0), (0, 0)), mode='wrap' if ty else 'edge')
    im = np.pad(im, ((0, 0), (p, p), (0, 0)), mode='wrap' if tx else 'edge')
    wrapped += ty and tx; edged += not (ty or tx)
    Image.fromarray(im).save(os.path.join(out, f[:-4] + f'__p{p}.png'))
print(f"prepared: {wrapped} tile both ways, {edged} tile neither way", file=sys.stderr)
