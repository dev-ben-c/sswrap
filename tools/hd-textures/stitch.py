# Find textures that continue into each other (the game draws skies as a ring of panels and big
# pictures such as the splash screen as a grid of tiles) and lay each set out as one image, so
# Real-ESRGAN upscales it in one piece and the pieces still join seamlessly afterwards. Upscaled
# one by one, each piece gets its own made-up detail at its edges and every join shows as a seam.
#
#   stitch.py find <dumpdir> <outdir> [minsize]   -> <outdir>/strips.txt + padded mosaics to upscale
#                                                    (pieces smaller than minsize, default 128, are skipped)
#   stitch.py split <upscaled> <outdir> <loaddir> <scale>  -> one PNG per piece in <loaddir>
#
# Two same-sized textures join (side by side, or one above the other) when the jump across their
# shared edge is no bigger than a typical pixel-to-pixel step inside them, and each is the other's
# clear best match. Only edges with something on them count: a plain black border matches every
# other plain black border and says nothing. Linked pieces are placed on a grid; a single row that
# closes on itself is a ring (a sky) and is padded by wrapping, everything else by extending the
# edges. Textures that already tile with themselves are left alone.
import sys, os, numpy as np
from PIL import Image

def load(path):
    return np.asarray(Image.open(path).convert('RGBA')).astype(np.float32)

def used(im):
    """Size of the part the game draws: big pictures are cut into pieces that sit top-left in
    their textures with pure black padding after them (the splash screen: 213x240 in 256x256)."""
    rgb = im[..., :3].sum(axis=2)
    cols = np.nonzero(rgb.any(axis=0))[0]; rows = np.nonzero(rgb.any(axis=1))[0]
    return (int(cols[-1]) + 1 if len(cols) else im.shape[1]), (int(rows[-1]) + 1 if len(rows) else im.shape[0])

def links(ims, axis, sizes):
    """Mutual best matches of each texture's far edge (right, or bottom, at the end of its used
    part) to another's near edge."""
    far = np.stack([im[:, sizes[k][0] - 1, :3] if axis == 1 else im[sizes[k][1] - 1, :, :3]
                    for k, im in enumerate(ims)])
    near = np.stack([np.take(im[..., :3], 0, axis) for im in ims])
    inner = np.array([max(np.abs(np.diff(im[..., :3], axis=axis)).mean(), 0.5) for im in ims])
    busy_far = far.std(axis=(1, 2)) > 8                         # edges with picture on them
    busy_near = near.std(axis=(1, 2)) > 8
    n = len(ims)
    D = np.empty((n, n), np.float32)
    for a in range(n):
        D[a] = np.abs(far[a][None] - near).mean(axis=(1, 2))
    self_tiles = np.diag(D) <= 2 * inner
    np.fill_diagonal(D, np.inf)
    S = D / inner[:, None]
    nxt = {}
    for a in range(n):
        if self_tiles[a]: continue
        order = np.argsort(S[a]); b = order[0]
        if S[a, b] > 2.0 or self_tiles[b] or not (busy_far[a] or busy_near[b]): continue
        if n > 1 and S[a, order[1]] < 2 * S[a, b]: continue    # ambiguous
        if np.argmin(S[:, b]) != a: continue                    # must be mutual
        nxt[a] = b
    return nxt, S

def pictures(dump):
    """Big pictures (the splash screen, menu backgrounds) are cut into tiles that the game uploads
    one after another, so they are dumped one after another too. For each run of same-sized,
    partly used tiles in dump order, try every grid layout and every boundary position, and keep
    the one whose joins look like the inside of a picture."""
    fs = [f for f in sorted(os.listdir(dump), key=lambda f: os.path.getmtime(os.path.join(dump, f)))
          if f.endswith('.tga') and not f.startswith('sub_')]
    runs, cur, key = [], [], None
    for f in fs:
        w, h = map(int, f.split('_')[0].split('x'))
        k = None
        if w >= 128 and h >= 128:
            im = load(os.path.join(dump, f)); uw, uh = used(im)
            if uw < w or uh < h: k = (w, h)
        if k and k == key: cur.append((f, im))
        else:
            if len(cur) >= 2: runs.append((key, cur))
            cur = [(f, im)] if k else []; key = k
    if len(cur) >= 2: runs.append((key, cur))
    found = []
    for (w, h), whole in runs:
        # a neighbouring texture may have joined the run by chance: also try without up to two
        # tiles at either end
        best = None                              # the longest run that makes a clean picture
        for cut in range(0, min(5, len(whole) - 1)):
            for a in range(cut + 1):
                b = cut - a
                if a > 2 or b > 2: continue
                r = layout(whole[a:len(whole) - b], w, h)
                if r and r[0] <= 2.5 and (best is None or r[0] < best[0]): best = r
            if best: break
        # Only full 640x480 screens (the splash and menu backgrounds): the engine cuts them into
        # 3 x 2 equal pieces, 213/213/214 x 240/240. Measured boundaries are not trusted (a dark
        # band can look like a join), and other "pictures" in dump order are usually live pages.
        if best and len(best[3]) == 6 and max(c for _, (c, r) in best[3]) == 2:
            t = {xy: load(os.path.join(dump, f))[..., :3] for f, xy in best[3]}
            inner = np.mean([max(np.abs(np.diff(im[:240, :213], axis=1)).mean(), 0.5) for im in t.values()])
            joins = [np.abs(t[(x, y)][:240, 212] - t[(x + 1, y)][:240, 0]).mean() for y in range(2) for x in range(2)]
            joins += [np.abs(t[(x, 0)][239, :213] - t[(x, 1)][0, :213]).mean() for x in range(3)]
            if np.mean(joins) / inner <= 3.0:     # real screens score 0.2-1.5, mixed-up tiles 10+
                found.append((False, [213, 213, 214], [240, 240], best[3], w, h))
    return found

def layout(run, w, h):
    """Best grid layout of a run of tiles: (score, column widths, row heights, pieces)."""
    if True:
        n = len(run); ims = [im[..., :3] for _, im in run]
        inner = np.mean([max(np.abs(np.diff(im, axis=1)).mean(), 0.5) for im in ims])
        best = None
        for cols in range(2, n + 1):
            if n % cols: continue
            rows = n // cols
            grid = [[ims[r * cols + c] for c in range(cols)] for r in range(rows)]
            cw, rh, cost, joins = [], [], 0.0, 0
            # the engine cuts a bitmap into equal pieces of at most 256 (640 -> 3 x 213), so every
            # inner column ends at the same place, and the last may be one wider
            def ends(n_in, size, cost_at):
                if n_in == 0: return [], 0.0, 0
                v, x = min((cost_at(x), x) for x in range(size // 2, size + 1))
                return [x] * n_in, v * n_in, n_in
            col_cost = lambda x: np.mean([np.abs(grid[r][c][:, x - 1] - grid[r][c + 1][:, 0]).mean()
                                          for r in range(rows) for c in range(cols - 1)])
            cw, v, j = ends(cols - 1, w, col_cost); cost += v; joins += j
            last = max(used(np.dstack([grid[r][-1], np.full(grid[r][-1].shape[:2] + (1,), 255.0)]))[0] for r in range(rows))
            cw.append(cw[0] + (last > cw[0]) if cw else last)   # equal pieces: the last is the same or one more
            row_cost = lambda y: np.mean([np.abs(grid[r][c][y - 1, :cw[c]] - grid[r + 1][c][0, :cw[c]]).mean()
                                          for r in range(rows - 1) for c in range(cols)])
            rh, v, j = ends(rows - 1, h, row_cost); cost += v; joins += j
            last = max(used(np.dstack([grid[-1][c], np.full(grid[-1][c].shape[:2] + (1,), 255.0)]))[1] for c in range(cols))
            rh.append(rh[0] + (last > rh[0]) if rh else last)
            score = cost / max(joins, 1) / inner
            aspect = sum(cw) / sum(rh)               # screens and pictures, not long bands
            if not 0.5 <= aspect <= 2.5: continue
            if best is None or score < best[0]:
                best = (score, cw, rh, [(run[r * cols + c][0], (c, r)) for r in range(rows) for c in range(cols)])
        return best

def find(dump, out, minsize=128):
    os.makedirs(out, exist_ok=True)
    sets = pictures(dump)
    taken = {f for s in sets for f, _ in s[3]}
    by_size = {}
    for f in sorted(os.listdir(dump)):
        if not f.endswith('.tga') or f.startswith('sub_') or f in taken: continue
        w, h = map(int, f.split('_')[0].split('x'))
        if w < minsize or h < minsize: continue
        by_size.setdefault((w, h), []).append(f)
    for (w, h), files in by_size.items():
        ims = [load(os.path.join(dump, f)) for f in files]
        sizes = [used(im) for im in ims]
        right, S = links(ims, 1, sizes)
        down, _ = links(ims, 0, sizes)
        pos, done = {}, set()
        for start in list(right) + list(down):
            if start in done: continue
            comp = {start: (0, 0)}; todo = [start]
            while todo:                              # place linked pieces on a grid
                a = todo.pop(); x, y = comp[a]
                nb = [(right.get(a), (x + 1, y)), (down.get(a), (x, y + 1))]
                nb += [(b, (x - 1, y)) for b, c in right.items() if c == a]
                nb += [(b, (x, y - 1)) for b, c in down.items() if c == a]
                for b, p in nb:
                    if b is not None and b not in comp: comp[b] = p; todo.append(b)
            done |= set(comp)
            cells = list(comp.values())
            if len(comp) < 2 or len(set(cells)) != len(cells): continue   # inconsistent: skip
            x0 = min(c[0] for c in cells); y0 = min(c[1] for c in cells)
            grid = {a: (x - x0, y - y0) for a, (x, y) in comp.items()}
            cols = 1 + max(c[0] for c in grid.values()); rows = 1 + max(c[1] for c in grid.values())
            ring = False
            if rows == 1 and cols >= 3:
                row = sorted(grid, key=lambda a: grid[a][0])
                busy = ims[row[-1]][:, sizes[row[-1]][0] - 1, :3].std() > 8
                ring = busy and S[row[-1], row[0]] <= 2.0   # the closing link may only have been ambiguous
            cw = [max(sizes[a][0] for a in grid if grid[a][0] == c) for c in range(cols)]   # used widths
            rh = [max(sizes[a][1] for a in grid if grid[a][1] == r) for r in range(rows)]   # used heights
            sets.append((ring, cw, rh, [(files[a], grid[a]) for a in grid], w, h))
    with open(os.path.join(out, 'strips.txt'), 'w') as lst:
        for k, (ring, cw, rh, pieces, w, h) in enumerate(sets):
            X = np.concatenate([[0], np.cumsum(cw)]); Y = np.concatenate([[0], np.cumsum(rh)])
            mos = np.zeros((Y[-1], X[-1], 4), np.uint8)
            have = np.zeros((len(rh), len(cw)), bool)
            for f, (x, y) in pieces:
                im = load(os.path.join(dump, f)).astype(np.uint8)
                mos[Y[y]:Y[y + 1], X[x]:X[x + 1]] = im[:rh[y], :cw[x]]
                have[y, x] = True
            if not have.all():                       # holes: fill with the average colour
                mask = np.zeros(mos.shape[:2], bool)
                for y, x in zip(*np.nonzero(have)): mask[Y[y]:Y[y + 1], X[x]:X[x + 1]] = True
                avg = mos[mask].mean(axis=0).astype(np.uint8)
                for y, x in zip(*np.nonzero(~have)): mos[Y[y]:Y[y + 1], X[x]:X[x + 1]] = avg
            p = max(4, min(w, h) // 8)
            mos = np.pad(mos, ((0, 0), (p, p), (0, 0)), mode='wrap' if ring else 'edge')
            mos = np.pad(mos, ((p, p), (0, 0), (0, 0)), mode='edge')
            Image.fromarray(mos).save(os.path.join(out, f'strip{k:03d}__p{p}.png'))
            kind = 'ring' if ring else 'grid'
            lst.write(f"strip{k:03d} {kind} {w}x{h} {','.join(map(str, cw))} {','.join(map(str, rh))} "
                      + ' '.join(f'{f}@{x},{y}' for f, (x, y) in pieces) + '\n')
    print(f"{len(sets)} sets ({sum(s[0] for s in sets)} rings, {sum(len(s[2]) > 1 for s in sets)} grids), "
          f"{sum(len(s[3]) for s in sets)} textures", file=sys.stderr)

def split(up, out, load_dir, scale):
    import re
    for line in open(os.path.join(out, 'strips.txt')):
        key, kind, size, cws, rhs, *pieces = line.split()
        w, h = (int(v) * scale for v in size.split('x'))
        cw = [int(v) * scale for v in cws.split(',')]; rh = [int(v) * scale for v in rhs.split(',')]
        X = np.concatenate([[0], np.cumsum(cw)]); Y = np.concatenate([[0], np.cumsum(rh)])
        f = [x for x in os.listdir(up) if re.match(re.escape(key) + r'__p(\d+)\.png$', x)]
        if not f: print('missing', key, file=sys.stderr); continue
        p = int(re.search(r'__p(\d+)', f[0]).group(1)) * scale
        im = Image.open(os.path.join(up, f[0]))
        im = im.crop((p, p, im.width - p, im.height - p))
        for piece in pieces:
            name, xy = piece.rsplit('@', 1); x, y = map(int, xy.split(','))
            cell = np.asarray(im.convert('RGBA').crop((X[x], Y[y], X[x + 1], Y[y + 1])))
            # back into a full texture; the padding repeats the edge pixels instead of black, so
            # filtering at the picture's edge never pulls in dark
            tex = np.pad(cell, ((0, h - cell.shape[0]), (0, w - cell.shape[1]), (0, 0)), mode='edge')
            Image.fromarray(tex).save(os.path.join(load_dir, name[:-4] + '.png'), optimize=True)

if __name__ == '__main__':
    if sys.argv[1] == 'find': find(sys.argv[2], sys.argv[3], int(sys.argv[4]) if len(sys.argv) > 4 else 128)
    else: split(sys.argv[2], sys.argv[3], sys.argv[4], int(sys.argv[5]))
