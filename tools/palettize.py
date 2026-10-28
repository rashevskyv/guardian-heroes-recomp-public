#!/usr/bin/env python3
"""Put the game's own palette back on upscaled sprites.

An AI upscaler (or a painter) returns a true-colour PNG. The game draws sprites with a 256-colour palette per
character, and the costume palettes recolour single palette entries. This tool turns each upscaled PNG into an
indexed PNG that uses the ORIGINAL palette: every pixel gets the index of the original-palette colour that is closest,
chosen only among the indices around that place in the original frame. Index meaning is kept, so costumes work.
The runtime hook takes an indexed PNG as indices into the game's palette (src/sprite_mod.h).

    python3 -I tools/palettize.py <in_dir> <out_dir>

Input names are the ones tools/spritemod.py export-all wrote: c<entry>_f<frame>__<id>.png (the Remix frames in
mods/sprites). Any size is fine (2x, 4x, ...) as long as the picture is the same frame, uncropped. The output is
named with the Remix frame's id, so it can go straight into the mod folder. Needs the game files (GH_MEDIA, default
game/pkg/media) for the palette and the original frame.
"""
import os, re, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mrg
from sprites import Chara
from spritemod import MEDIA, sprite_id


def palettize(rgba, ref_idx, pal):
    """rgba [H, W, 4] uint8 (any size), ref_idx [h, w] original indices, pal [256, 4] RGBA -> indices [H, W]."""
    H, W = rgba.shape[:2]
    h, w = ref_idx.shape
    pad = np.pad(ref_idx, 1, mode='edge')
    opaque_pal = pal[:, 3] > 0
    cols = pal[:, :3].astype(np.int32)
    out = np.zeros((H, W), np.uint8)
    xs = np.minimum((np.arange(W) + 0.5) * w / W, w - 1e-6).astype(int)
    for y0 in range(0, H, 64):
        y1 = min(H, y0 + 64)
        ys = np.minimum((np.arange(y0, y1) + 0.5) * h / H, h - 1e-6).astype(int)
        px = rgba[y0:y1, :, :3].astype(np.int32)
        best = np.full((y1 - y0, W), 1 << 30, np.int64)
        arg = np.zeros((y1 - y0, W), np.uint8)
        for dy in (0, 1, 2):
            for dx in (0, 1, 2):
                cand = pad[ys[:, None] + dy, xs[None, :] + dx]  # [rows, W] original index at that neighbour
                d = ((cols[cand] - px) ** 2).sum(-1)
                d = np.where(opaque_pal[cand], d, 1 << 30)
                better = d < best
                best = np.where(better, d, best)
                arg = np.where(better, cand, arg)
        out[y0:y1] = arg
    out[rgba[..., 3] < 128] = 0
    return out


def main(src, dst):
    os.makedirs(dst, exist_ok=True)
    hdm = mrg.Mrg(os.path.join(MEDIA, 'chara.mrg'))
    cache = {}
    n = 0
    for fn in sorted(os.listdir(src)):
        m = re.match(r'c(\d+)_f(\d+)__.*\.png$', fn)
        if not m:
            continue
        e, f = int(m.group(1)), int(m.group(2))
        if e not in cache:
            cache = {e: Chara(hdm.get(e))}
        c = cache[e]
        w, h = c.recs[f][:2]
        ref = c.idx(f)
        im = Image.open(os.path.join(src, fn)).convert('RGBA')
        idx = palettize(np.asarray(im), ref, c.pals[0])
        out = Image.fromarray(idx, 'P')
        out.putpalette(c.pals[0][:, :3].astype(np.uint8).reshape(-1).tolist())
        name = 'c%02d_f%03d__%s.png' % (e, f, sprite_id(w, h, ref.tobytes()))
        out.save(os.path.join(dst, name), transparency=0)
        n += 1
    print('%d sprites palettized into %s' % (n, dst))


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
