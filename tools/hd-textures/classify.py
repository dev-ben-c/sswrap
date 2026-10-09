# Sort dumped textures into upscale-worthy art and junk. Games that pack their HUD/menu bitmaps
# into texture pages at runtime leave uninitialised memory (random noise) in the unused parts;
# those pages never get the same fingerprint twice, so they are skipped, as are tiny and
# single-colour textures. Usage: classify.py <dumpdir> <out.json>
import os, sys, numpy as np
from PIL import Image
D=sys.argv[1]
out={'good':[], 'garbage':[], 'tiny':[], 'flat':[]}
for f in sorted(os.listdir(D)):
    im=np.asarray(Image.open(os.path.join(D,f)).convert('RGBA')).astype(np.int16)
    h,w,_=im.shape
    if w<16 or h<16: out['tiny'].append(f); continue
    rgb=im[...,:3]
    if rgb.std()<2: out['flat'].append(f); continue
    # garbage = blocks whose neighbouring pixels are uncorrelated (random memory)
    dx=np.abs(np.diff(rgb,axis=1)).mean(axis=2); dy=np.abs(np.diff(rgb,axis=0)).mean(axis=2)
    b=8; garbage=False
    for y in range(0,h-b,b):
        for x in range(0,w-b,b):
            if dx[y:y+b,x:x+b-1].mean()>55 and dy[y:y+b-1,x:x+b].mean()>55: garbage=True; break
        if garbage: break
    out['garbage' if garbage else 'good'].append(f)
for k,v in out.items(): print(k, len(v))
import json; json.dump(out, open(sys.argv[2],'w'))
