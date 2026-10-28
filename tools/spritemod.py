#!/usr/bin/env python3
"""Sprite mods for Guardian Heroes: export frames as PNG, edit them, apply them back into the game files.

Workflow (run from the repo root; the game files are game/pkg/media/chara_ss.mrg and chara.mrg):

    python3 -I tools/spritemod.py export chara_ss 0         # -> mods/chara_ss/00/f000.png ... + info.txt
    (repaint any of the PNGs, keep the file names and the image size)
    python3 -I tools/spritemod.py apply  chara_ss 0         # builds the new .mrg, keeps chara_ss.mrg.orig
    python3 -I tools/spritemod.py restore chara_ss          # puts the original back
    python3 -I tools/spritemod.py export-all mods           # every Remix sprite for the runtime hook (section 8)

The game then loads the patched file; nothing in the program changes. Copy the .mrg to the console the same way as
the original. Game data and exported images never go into git (mods/ is ignored).

    python3 -I tools/spritemod.py apply chara 0 --adaptive [--soft-alpha]
        true-colour repaint: one new 255-colour palette is fitted to ALL frames of the entry (PNGs you gave plus the
        unchanged originals). Costume palettes become copies of it (costume recolours are lost), or with
        --costumes=derive they are guessed from the old recolours (grey areas of different roles get mixed up,
        so expect stray outlines). --soft-alpha keeps partly transparent edges
        in the palette (check in game that the shader honours palette alpha before using it).

PNG rules
- Same width and height as the exported file. Only the files you changed (or all of them) are read.
- Indexed PNG (mode P): exported that way, with the character's real palette. If you keep the palette, indices are
  copied as they are and the costume palettes keep working. Index 0 is transparent.
- Any other PNG (RGBA): every pixel snaps to the nearest colour of palette 0, alpha < 128 is transparent. The game
  draws with 256 palette colours per character, so a true-colour repaint is reduced to them. Costume recolours only
  touch a few palette entries, so they work where your colour is exactly one of those entries and look off elsewhere.
"""
import os, shutil, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mrg
from sprites import Chara

def sprite_hash(data):
    """Texture identity hash shared with src/sprite_mod.cpp: crc32 << 32 | adler32 of the untiled bytes."""
    import zlib
    return (zlib.crc32(data) << 32) | zlib.adler32(data)


def sprite_id(w, h, data):
    return '%dx%d_%016x' % (w, h, sprite_hash(data))


MEDIA = os.environ.get('GH_MEDIA', 'game/pkg/media')
MODS = os.environ.get('GH_MODS', 'mods')


def paths(name):
    return os.path.join(MEDIA, name + '.mrg'), os.path.join(MODS, name)


def frame_png(c, f, pal=0):
    w, h = c.recs[f][:2]
    im = Image.fromarray(c.idx(f), 'P')
    p = c.pals[pal]
    im.putpalette(p[:, :3].astype(np.uint8).reshape(-1).tolist())
    im.info['transparency'] = 0
    return im


def cmd_export(name, entry):
    src, d = paths(name)
    d = os.path.join(d, '%02d' % entry)
    os.makedirs(d, exist_ok=True)
    c = Chara(mrg.Mrg(src).get(entry))
    with open(os.path.join(d, 'info.txt'), 'w') as fh:
        fh.write('frame width height pivot_x pivot_y\n')
        for f, (w, h, x, y, _) in enumerate(c.recs):
            fh.write('f%03d %d %d %d %d\n' % (f, w, h, x, y))
    n = 0
    for f, (w, h, *_r) in enumerate(c.recs):
        if not w or not h:
            continue
        frame_png(c, f).save(os.path.join(d, 'f%03d.png' % f), transparency=0)
        n += 1
    print('exported %d frames to %s' % (n, d))


def quantise(rgba, pal):
    """RGBA array -> palette indices (nearest colour of the opaque palette entries, alpha < 128 -> 0)."""
    cols = pal[:, :3].astype(np.int32)
    ok = (pal[:, 3] > 0)
    ok[0] = False
    ids = np.nonzero(ok)[0]
    flat = rgba[..., :3].reshape(-1, 3).astype(np.int32)
    out = np.zeros(len(flat), np.uint8)
    for s in range(0, len(flat), 65536):  # keep the distance matrix small
        d = ((flat[s:s + 65536, None, :] - cols[ids][None, :, :]) ** 2).sum(-1)
        out[s:s + 65536] = ids[d.argmin(1)]
    out = out.reshape(rgba.shape[:2])
    out[rgba[..., 3] < 128] = 0
    return out


def load_frame(path, c, f):
    w, h = c.recs[f][:2]
    im = Image.open(path)
    if im.size != (w, h):
        raise SystemExit('%s: size %dx%d, the game frame is %dx%d' % (path, im.width, im.height, w, h))
    pal0 = c.pals[0]
    if im.mode == 'P':
        pp = np.array(im.getpalette() or [], np.uint8)
        pp = np.pad(pp, (0, max(0, 768 - len(pp)))).reshape(-1, 3)[:256]
        used = np.unique(np.asarray(im))
        if all((pp[i] == pal0[i][:3]).all() for i in used if i < 256 and i != 0):
            a = np.asarray(im).copy()
            return a, 'indices'
    rgba = np.asarray(im.convert('RGBA'))
    return quantise(rgba, pal0), 'snapped'


def adaptive_palette(frames, max_colours=255, soft_alpha=False):
    """frames: list of RGBA arrays. Returns RGBA palette [256, 4] (index 0 transparent) and a function image -> indices."""
    px = np.concatenate([f.reshape(-1, 4) for f in frames])
    px = px[px[:, 3] >= 128] if not soft_alpha else px[px[:, 3] >= 8]
    if not soft_alpha:
        px = px.copy(); px[:, 3] = 255
    if len(px) > 3_000_000:  # sample for speed
        px = px[np.random.default_rng(1).choice(len(px), 3_000_000, replace=False)]
    side = int(np.ceil(np.sqrt(len(px))))
    mosaic = np.zeros((side * side, 4), np.uint8); mosaic[:len(px)] = px; mosaic[len(px):] = px[0]
    q = Image.fromarray(mosaic.reshape(side, side, 4), 'RGBA').quantize(colors=max_colours, method=Image.Quantize.FASTOCTREE)
    pal = np.array(q.getpalette(rawmode='RGBA'), np.uint8).reshape(-1, 4)[:max_colours]
    out = np.zeros((256, 4), np.uint8)
    out[1:1 + len(pal)] = pal
    if not soft_alpha:
        out[1:, 3] = 255
    return out


def to_indices(rgba, pal, soft_alpha):
    ids = np.nonzero(pal[:, 3] > 0)[0]
    ids = ids[ids != 0]
    cols = pal[ids].astype(np.int32)
    flat = rgba.reshape(-1, 4).astype(np.int32)
    out = np.zeros(len(flat), np.uint8)
    for s in range(0, len(flat), 32768):
        c = flat[s:s + 32768]
        d = ((c[:, None, :3] - cols[None, :, :3]) ** 2).sum(-1)
        if soft_alpha:
            d = d + ((c[:, None, 3] - cols[None, :, 3]) ** 2)
        out[s:s + 32768] = ids[d.argmin(1)]
    out = out.reshape(rgba.shape[:2])
    out[rgba[..., 3] < (8 if soft_alpha else 128)] = 0
    return out


def costume_palettes(c, new0):
    """Palette slots for the new art: every old costume recolour (old index i: P0[i] -> Pk[i]) is applied
    as a colour shift to the new colours that were nearest to the old colour i."""
    old0 = c.pals[0].astype(np.int32)
    ids = np.nonzero(old0[:, 3] > 0)[0]; ids = ids[ids != 0]
    nc = new0.astype(np.int32)
    nid = np.nonzero(nc[:, 3] > 0)[0]
    dist = ((nc[nid, None, :3] - old0[None, ids, :3]) ** 2).sum(-1)
    nearest = ids[dist.argmin(1)]
    # a colour far from every old colour is a blend (an edge): it gets only part of the shift, or none
    weight = np.clip(1.0 - np.sqrt(dist.min(1)) / 40.0, 0.0, 1.0)[:, None]
    pals = []
    for k in range(len(c.pals)):
        pk = new0.copy()
        if c.pals[k][..., 3].any():
            delta = c.pals[k].astype(np.int32)[:, :3] - old0[:, :3]
            sh = delta[nearest]
            pk[nid, :3] = np.clip(nc[nid, :3] + sh * weight, 0, 255).astype(np.uint8)
            pk[nid, 3] = new0[nid, 3]
        else:
            pk = np.zeros_like(new0)
        pals.append(pk)
    return pals


def cmd_apply_adaptive(name, entry, soft_alpha, derive):
    src, d = paths(name)
    d = os.path.join(d, '%02d' % entry)
    orig = src + '.orig'
    if not os.path.exists(orig):
        shutil.copy2(src, orig)
    M = mrg.Mrg(orig)
    c = Chara(M.get(entry))
    rgba = {}
    for f, (w, h, *_r) in enumerate(c.recs):
        if not w or not h:
            continue
        p = os.path.join(d, 'f%03d.png' % f)
        if os.path.exists(p):
            im = Image.open(p)
            if im.size != (w, h):
                raise SystemExit('%s: size %dx%d, the game frame is %dx%d' % (p, im.width, im.height, w, h))
            rgba[f] = np.asarray(im.convert('RGBA'))
        else:
            rgba[f] = c.rgba(f, 0)
    new0 = adaptive_palette(list(rgba.values()), soft_alpha=soft_alpha)
    for f, a in rgba.items():
        c.set_idx(f, to_indices(a, new0, soft_alpha))
    if derive:
        pals = costume_palettes(c, new0)
    else:  # every costume slot gets the new base palette: costume recolours are lost
        pals = [new0 if c.pals[k][..., 3].any() else np.zeros_like(new0) for k in range(len(c.pals))]
    for k, pk in enumerate(pals):
        o = c.palbase + k * 1024
        blk = np.stack([pk[:, 3], pk[:, 0], pk[:, 1], pk[:, 2]], -1).astype(np.uint8)
        c.x[o:o + 1024] = blk.tobytes()
    ents = [M.raw(i) for i in range(M.n)]
    ents[entry] = mrg.pack_entry(bytes(c.x))
    open(src, 'wb').write(mrg.build(ents))
    print('%s entry %d: %d frames re-indexed with a new %d-colour palette (%d costume palettes)'
          % (name, entry, len(rgba), int((new0[:, 3] > 0).sum()), sum(1 for p in c.pals if p[..., 3].any())))


def cmd_apply(name, entry):
    src, d = paths(name)
    d = os.path.join(d, '%02d' % entry)
    if not os.path.isdir(d):
        raise SystemExit('no folder %s (run export first)' % d)
    orig = src + '.orig'
    if not os.path.exists(orig):
        shutil.copy2(src, orig)
    M = mrg.Mrg(orig)  # always build from the untouched original
    c = Chara(M.get(entry))
    n = {'indices': 0, 'snapped': 0, 'same': 0}
    for f in range(len(c.recs)):
        p = os.path.join(d, 'f%03d.png' % f)
        if not os.path.exists(p):
            continue
        a, how = load_frame(p, c, f)
        if (a == c.idx(f)).all():
            n['same'] += 1
            continue
        c.set_idx(f, a)
        n[how] += 1
    ents = [M.raw(i) for i in range(M.n)]
    if n['indices'] + n['snapped']:
        ents[entry] = mrg.pack_entry(bytes(c.x))
    open(src, 'wb').write(mrg.build(ents))
    print('%s entry %d: %d frames from indexed PNG, %d snapped to the palette, %d unchanged'
          % (name, entry, n['indices'], n['snapped'], n['same']))


def cmd_export_all(out):
    """Every Remix (HD) sprite frame as RGBA PNG for the runtime hook (docs/SPRITES-RESEARCH.md section 8):
    out/sprites/c<entry>_f<frame>__<id>.png. Frames whose texture is byte-identical in the Saturn-size set are left
    out (a replacement would also hit the Original graphics mode); they are listed in out/shared.txt. The Saturn-size
    frames go to out/sprites_ss/ for reference. out/manifest.txt maps ids to entry, frame, pivot."""
    hdm = mrg.Mrg(os.path.join(MEDIA, 'chara.mrg'))
    ssm = mrg.Mrg(os.path.join(MEDIA, 'chara_ss.mrg'))
    d_hd, d_ss = os.path.join(out, 'sprites'), os.path.join(out, 'sprites_ss')
    os.makedirs(d_hd, exist_ok=True); os.makedirs(d_ss, exist_ok=True)
    ss_ids = set()
    man, shared = [], []
    n_hd = n_ss = 0
    for e in range(hdm.n):
        try:
            hd, ss = Chara(hdm.get(e)), Chara(ssm.get(e))
        except Exception:
            continue
        if not hd.recs or not ss.recs:
            continue
        for f, (w, h, x, y, _) in enumerate(ss.recs):
            if not w or not h:
                continue
            a = ss.idx(f)
            if not (ss.pals[0][a][..., 3] > 0).any():
                continue
            sid = sprite_id(w, h, a.tobytes())
            ss_ids.add(sid)
            Image.fromarray(ss.rgba(f, 0), 'RGBA').save(os.path.join(d_ss, 'c%02d_f%03d__%s.png' % (e, f, sid)))
            n_ss += 1
        seen = {}
        for f, (w, h, x, y, _) in enumerate(hd.recs):
            if not w or not h:
                continue
            a = hd.idx(f)
            if not (hd.pals[0][a][..., 3] > 0).any():
                continue
            hid = sprite_id(w, h, a.tobytes())
            man.append('%s c%02d f%03d %dx%d pivot %d %d' % (hid, e, f, w, h, x, y))
            if hid in ss_ids or hid in seen:
                if hid in ss_ids:
                    shared.append('c%02d_f%03d %s' % (e, f, hid))
                continue
            seen[hid] = f
            Image.fromarray(hd.rgba(f, 0), 'RGBA').save(os.path.join(d_hd, 'c%02d_f%03d__%s.png' % (e, f, hid)))
            n_hd += 1
        print('entry %02d done: %d HD, %d SS so far' % (e, n_hd, n_ss), flush=True)
    # ss ids of later entries can also match earlier HD frames: re-check the written files
    for fn in os.listdir(d_hd):
        hid = fn.rsplit('__', 1)[1][:-4]
        if hid in ss_ids:
            os.remove(os.path.join(d_hd, fn)); n_hd -= 1
            shared.append(fn[:-4])
    open(os.path.join(out, 'manifest.txt'), 'w').write('\n'.join(man) + '\n')
    open(os.path.join(out, 'shared.txt'), 'w').write('\n'.join(shared) + '\n')
    print('wrote %d HD frames, %d Saturn-size frames, %d shared (skipped)' % (n_hd, n_ss, len(shared)))


def cmd_restore(name):
    src, _ = paths(name)
    if os.path.exists(src + '.orig'):
        shutil.copy2(src + '.orig', src)
        print('restored', src)
    else:
        print('no backup, nothing to do')


if __name__ == '__main__':
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == 'apply' and '--adaptive' in a:
        cmd_apply_adaptive(a[1], int(a[2]), '--soft-alpha' in a, '--costumes=derive' in a)
    elif len(a) == 2 and a[0] == 'export-all':
        cmd_export_all(a[1])
    elif len(a) >= 3 and a[0] in ('export', 'apply'):
        {'export': cmd_export, 'apply': cmd_apply}[a[0]](a[1], int(a[2]))
    elif len(a) == 2 and a[0] == 'restore':
        cmd_restore(a[1])
    else:
        sys.exit(__doc__)
