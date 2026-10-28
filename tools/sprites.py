#!/usr/bin/env python3
"""Guardian Heroes character sprites: dump, rescale, repack (research tool, see docs/SPRITES-RESEARCH.md).

chara.mrg / chara_ss.mrg hold one packed entry per character (55). An unpacked entry is:
  0x00  header words (BE u32): 0x14 = start of the palettes
  0x14  u32 palette start (0x4000); 0x18.. frame records of 12 bytes: u16 w, u16 h, s16 x, s16 y, u32 data offset
        (126 records, the table ends before 0x600; the animation tables follow up to the palettes)
  pal   64 palettes x 256 x ARGB (BE bytes A,R,G,B); index 0 is transparent
  data  8 bit indexed frames, Xenos 2D tiled (tools/gh_tex.tiled_offsets, 1 byte per texel), each 4 KB aligned
chara.mrg is the "HD" set, chara_ss.mrg the Saturn-size set. HD = Scale3x(SS) pixel for pixel; x, y are x3.

    python3 -I tools/sprites.py dump <mrg> <entry> <out_dir> [--pal N]          frames + contact sheet as PNG
    python3 -I tools/sprites.py rescale <chara.mrg> <chara_ss.mrg> <entry> <algo> <out.mrg>
        algo: scale3x (the game's own, must reproduce the file), nn (square pixels), hq3x-q (hq3x, snapped to
        the neighbouring source indices so the palette variants keep working)
    python3 -I tools/sprites.py check <chara.mrg> <chara_ss.mrg> <entry>        HD == Scale3x(SS)?
"""
import os, struct, sys
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mrg
from gh_tex import tiled_offsets


class Chara:
    def __init__(self, x):
        self.x = bytearray(x)
        self.palbase = struct.unpack('>I', x[0x14:0x18])[0]
        self.recs = []
        p = 0x18
        while p + 12 <= 0x600:
            sz, pos, off = struct.unpack('>III', x[p:p + 12])
            if off == 0:
                break
            px, py = struct.unpack('>hh', struct.pack('>I', pos))
            self.recs.append((sz >> 16, sz & 0xFFFF, px, py, off))
            p += 12
        n = (self.recs[0][4] - self.palbase) // 1024
        pal = np.frombuffer(bytes(x[self.palbase:self.palbase + n * 1024]), np.uint8).reshape(n, 256, 4)
        self.pals = np.stack([pal[..., 1], pal[..., 2], pal[..., 3], pal[..., 0]], -1)  # RGBA

    def span(self, f):
        w, h = self.recs[f][:2]
        if not w or not h:
            return 0
        return (int(tiled_offsets((w + 31) & ~31, (h + 31) & ~31, 0).max()) + 4096) // 4096 * 4096

    def idx(self, f):
        w, h, _, _, off = self.recs[f]
        if not w or not h:
            return np.zeros((h, w), np.uint8)
        o = tiled_offsets((w + 31) & ~31, (h + 31) & ~31, 0)[:h, :w]
        return np.frombuffer(bytes(self.x[off:off + self.span(f)]).ljust(self.span(f), b'\0'), np.uint8)[o]

    def set_idx(self, f, a):
        w, h, _, _, off = self.recs[f]
        assert a.shape == (h, w)
        if not w or not h:
            return
        o = tiled_offsets((w + 31) & ~31, (h + 31) & ~31, 0)[:h, :w]
        buf = np.zeros(self.span(f), np.uint8)
        buf[o] = a
        self.x[off:off + len(buf)] = buf.tobytes()

    def rgba(self, f, pal=0):
        return self.pals[pal][self.idx(f)]


def scale3x(a):
    """AdvMAME3x on a 2D array of ids."""
    H, W = a.shape
    p = np.pad(a, 1, mode='edge')
    A, B, C = p[:-2, :-2], p[:-2, 1:-1], p[:-2, 2:]
    D, E, F = p[1:-1, :-2], p[1:-1, 1:-1], p[1:-1, 2:]
    G, Hh, I = p[2:, :-2], p[2:, 1:-1], p[2:, 2:]
    out = np.zeros((H * 3, W * 3), a.dtype)
    c = (B != Hh) & (D != F)

    def put(dy, dx, v):
        out[dy::3, dx::3] = v
    put(0, 0, np.where(c & (D == B), D, E))
    put(0, 1, np.where(c & (((D == B) & (E != C)) | ((B == F) & (E != A))), B, E))
    put(0, 2, np.where(c & (B == F), F, E))
    put(1, 0, np.where(c & (((D == B) & (E != G)) | ((D == Hh) & (E != A))), D, E))
    put(1, 1, E)
    put(1, 2, np.where(c & (((B == F) & (E != I)) | ((Hh == F) & (E != C))), F, E))
    put(2, 0, np.where(c & (D == Hh), D, E))
    put(2, 1, np.where(c & (((D == Hh) & (E != I)) | ((Hh == F) & (E != G))), Hh, E))
    put(2, 2, np.where(c & (Hh == F), F, E))
    return out


def _hqx():
    import types, PIL
    if 'PIL.PyAccess' not in sys.modules:  # hqx imports a module current Pillow dropped
        m = types.ModuleType('PIL.PyAccess'); sys.modules['PIL.PyAccess'] = m; PIL.PyAccess = m
    import hqx
    return hqx


def hq3x_quant(ss_idx, pal):
    """hq3x on the frame, then each output pixel takes the index of the 3x3 source neighbour whose colour
    is closest to the blended value (or becomes transparent). Only indices already present around the
    pixel are used, so the costume palettes, which recolour single indices, keep their meaning."""
    h, w = ss_idx.shape
    opaque = pal[ss_idx][..., 3] > 0
    bg = np.array((255, 0, 255), np.int32)
    rgb = np.where(opaque[..., None], pal[ss_idx][..., :3], bg.astype(np.uint8))
    big = np.asarray(_hqx().hq3x(Image.fromarray(rgb.astype(np.uint8), 'RGB')), np.int32)
    p = np.pad(ss_idx, 1, mode='edge')
    po = np.pad(opaque, 1, mode='constant', constant_values=False)
    cand = np.stack([p[dy:dy + h, dx:dx + w] for dy in range(3) for dx in range(3)], -1)  # [h, w, 9]
    cand_ok = np.stack([po[dy:dy + h, dx:dx + w] for dy in range(3) for dx in range(3)], -1)
    ccol = pal[cand][..., :3].astype(np.int32)  # [h, w, 9, 3]
    big6 = big.reshape(h, 3, w, 3, 3).transpose(0, 2, 1, 3, 4)  # [h, w, 3, 3, rgb]
    out = np.zeros((h, w, 3, 3), np.uint8)
    for ty in range(3):
        for tx in range(3):
            v = big6[:, :, ty, tx, :]  # [h, w, 3]
            d = ((ccol - v[:, :, None, :]) ** 2).sum(-1)
            d = np.where(cand_ok, d, 1 << 30)
            k = d.argmin(-1)
            best = np.take_along_axis(cand, k[..., None], -1)[..., 0]
            dmin = np.take_along_axis(d, k[..., None], -1)[..., 0]
            dbg = ((v - bg) ** 2).sum(-1)
            out[:, :, ty, tx] = np.where(dbg < dmin, 0, best)
    return out.transpose(0, 2, 1, 3).reshape(h * 3, w * 3)


def to_hd_canvas(big, ss, hd, f):
    """Place a 3x frame (from SS frame f) on the HD frame's canvas; anything outside is dropped."""
    _, _, ax, ay, _ = ss.recs[f]
    W, H, bx, by, _ = hd.recs[f]
    ox, oy = 3 * ax - bx, 3 * ay - by
    out = np.zeros((H, W), np.uint8)
    h, w = big.shape
    y0, x0 = max(0, oy), max(0, ox)
    y1, x1 = min(H, oy + h), min(W, ox + w)
    out[y0:y1, x0:x1] = big[y0 - oy:y1 - oy, x0 - ox:x1 - ox]
    return out


def rescale(ss, hd, f, algo):
    s = ss.idx(f)
    trans = ss.pals[0][..., 3] == 0
    if algo == 'nn':
        big = np.kron(s, np.ones((3, 3), np.uint8))
    elif algo == 'scale3x':
        # the transparent index may be more than one value; the game only has 0
        big = scale3x(s.astype(np.int32)).astype(np.uint8)
    elif algo == 'hq3x-q':
        big = hq3x_quant(s, ss.pals[0])
    else:
        raise SystemExit('unknown algo ' + algo)
    return to_hd_canvas(big, ss, hd, f)


def cmd_dump(argv):
    path, entry, out = argv[0], int(argv[1]), argv[2]
    pal = int(argv[argv.index('--pal') + 1]) if '--pal' in argv else 0
    os.makedirs(out, exist_ok=True)
    c = Chara(mrg.Mrg(path).get(entry))
    tiles = []
    for f in range(len(c.recs)):
        im = Image.fromarray(c.rgba(f, pal), 'RGBA')
        im.save(os.path.join(out, 'f%03d.png' % f))
        tiles.append(im)
    cw = max(t.width for t in tiles); ch = max(t.height for t in tiles)
    cols = 10
    rows = (len(tiles) + cols - 1) // cols
    sheet = Image.new('RGBA', (cols * cw, rows * ch), (64, 64, 80, 255))
    for i, t in enumerate(tiles):
        sheet.alpha_composite(t, ((i % cols) * cw, (i // cols) * ch))
    sheet.save(os.path.join(out, 'sheet.png'))
    print(len(tiles), 'frames, sheet', sheet.size)


def cmd_check(argv):
    hdm, ssm, entry = argv[0], argv[1], int(argv[2])
    hd = Chara(mrg.Mrg(hdm).get(entry)); ss = Chara(mrg.Mrg(ssm).get(entry))
    assert len(hd.recs) == len(ss.recs)
    same = tot = 0
    for f in range(len(ss.recs)):
        a = hd.idx(f); b = rescale(ss, hd, f, 'scale3x')
        ta = hd.pals[0][a][..., 3] > 0; tb = hd.pals[0][b][..., 3] > 0
        m = ta | tb
        same += int(((a == b) & m).sum()); tot += int(m.sum())
    print('frames %d, opaque pixels %d, identical %.4f%%' % (len(ss.recs), tot, 100.0 * same / tot))


def cmd_rescale(argv):
    hdm, ssm, entry, algo, outp = argv[0], argv[1], int(argv[2]), argv[3], argv[4]
    M = mrg.Mrg(hdm)
    hd = Chara(M.get(entry)); ss = Chara(mrg.Mrg(ssm).get(entry))
    for f in range(len(ss.recs)):
        hd.set_idx(f, rescale(ss, hd, f, algo))
    ents = [M.raw(i) for i in range(M.n)]
    ents[entry] = mrg.pack_entry(bytes(hd.x))
    open(outp, 'wb').write(mrg.build(ents))
    print('wrote', outp, 'entry', entry, 'packed', len(ents[entry]), 'was', len(M.raw(entry)))


if __name__ == '__main__':
    cmds = {'dump': cmd_dump, 'check': cmd_check, 'rescale': cmd_rescale}
    if len(sys.argv) < 2 or sys.argv[1] not in cmds:
        sys.exit(__doc__)
    cmds[sys.argv[1]](sys.argv[2:])
