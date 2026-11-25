#!/usr/bin/env python3
"""Decode the raw guest texture dumps of settings.txt "dump_tex=N" (sdmc:/switch/gh/texdump/t<base>_<w>x<h>_<fmt>.bin/.txt)
to PNG on the PC, with the ReXGlue SDK reference layout (GetTiledOffset2D, stride = pitch in blocks aligned to 32, then the
Xenos endian swap per block). Standard library only.
  tools/decode_texdump.py DIR_OR_BIN [...]     writes <name>.png next to each .bin
  tools/decode_texdump.py --selftest           tiles a synthetic image, decodes it, compares
If the picture looks right the console's detile is right and the problem is elsewhere (sampler, upload timing, a shader)."""
import struct, sys, zlib, os, glob, random


def tiled_offset_2d(x, y, pitch, l):
    pitch = (pitch + 31) & ~31
    macro = ((x >> 5) + (y >> 5) * (pitch >> 5)) << (l + 7)
    micro = ((x & 7) + ((y & 0xE) << 2)) << l
    off = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4)
    return ((off & ~0x1FF) << 3) + ((y & 16) << 7) + ((off & 0x1C0) << 2) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (off & 0x3F)


def swap(endian, b):
    if endian == 1:
        return b"".join(b[i + 1:i + 2] + b[i:i + 1] for i in range(0, len(b) - 1, 2))
    if endian == 2:
        return b"".join(b[i:i + 4][::-1] for i in range(0, len(b) - 3, 4))
    if endian == 3:
        return b"".join(b[i + 2:i + 4] + b[i:i + 2] for i in range(0, len(b) - 3, 4))
    return b


def blocks_of(meta, data):
    """The image as a list of rows of blocks (guest bytes swapped to host order)."""
    bw, bh = meta["block_width"], meta["block_height"]
    bpb = meta["bytes_per_block"]
    nx, ny = -(-meta["width"] // bw), -(-meta["height"] // bh)
    stride = (((meta["pitch"] + bw - 1) // bw) + 31) & ~31
    l = {1: 0, 2: 1, 4: 2, 8: 3, 16: 4}[bpb]
    rows = []
    for y in range(ny):
        row = []
        for x in range(nx):
            off = tiled_offset_2d(x, y, stride, l) if meta["tiled"] else (y * stride + x) * bpb
            row.append(swap(meta["endian"], data[off:off + bpb].ljust(bpb, b"\0")))
        rows.append(row)
    return rows


def c565(v):
    return ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)


def dxt_colors(b8, dxt1):
    c0, c1 = struct.unpack("<HH", b8[:4])
    a, b = c565(c0), c565(c1)
    if c0 > c1 or not dxt1:
        pal = [a + (255,), b + (255,), tuple((2 * p + q) // 3 for p, q in zip(a, b)) + (255,), tuple((p + 2 * q) // 3 for p, q in zip(a, b)) + (255,)]
    else:
        pal = [a + (255,), b + (255,), tuple((p + q) // 2 for p, q in zip(a, b)) + (255,), (0, 0, 0, 0)]
    idx = struct.unpack("<I", b8[4:8])[0]
    return [pal[(idx >> (2 * i)) & 3] for i in range(16)]


def decode(meta, data):
    w, h, fmt = meta["width"], meta["height"], meta["format"]
    rows = blocks_of(meta, data)
    img = [[(0, 0, 0, 255)] * w for _ in range(h)]
    for by, row in enumerate(rows):
        for bx, blk in enumerate(row):
            if fmt.startswith("k_DXT"):
                if fmt == "k_DXT1":
                    px = dxt_colors(blk, True)
                elif fmt == "k_DXT2_3":
                    px = dxt_colors(blk[8:], False)
                    al = struct.unpack("<Q", blk[:8])[0]
                    px = [p[:3] + (((al >> (4 * i)) & 15) * 17,) for i, p in enumerate(px)]
                else:
                    px = dxt_colors(blk[8:], False)
                    a0, a1 = blk[0], blk[1]
                    ab = int.from_bytes(blk[2:8], "little")
                    tab = [a0, a1] + ([(( 6 - i) * a0 + i * a1) // 7 for i in range(1, 7)] if a0 > a1 else [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)] + [0, 255])
                    px = [p[:3] + (tab[(ab >> (3 * i)) & 7],) for i, p in enumerate(px)]
                for i, p in enumerate(px):
                    x, y = bx * 4 + i % 4, by * 4 + i // 4
                    if x < w and y < h:
                        img[y][x] = p
            elif fmt == "k_8_8_8_8":
                if bx < w and by < h:
                    img[by][bx] = (blk[2], blk[1], blk[0], blk[3])  # 8in32 swapped: B G R A in memory
            elif fmt == "k_4_4_4_4":
                v = struct.unpack("<H", blk)[0]
                if bx < w and by < h:
                    img[by][bx] = (((v >> 8) & 15) * 17, ((v >> 4) & 15) * 17, (v & 15) * 17, ((v >> 12) & 15) * 17)
            elif fmt == "k_8":
                if bx < w and by < h:
                    img[by][bx] = (blk[0], blk[0], blk[0], 255)
            else:
                raise SystemExit("format %s is not decoded here" % fmt)
    return img


def write_png(path, img):
    h, w = len(img), len(img[0])
    raw = b"".join(b"\0" + bytes(c for p in row for c in p) for row in img)
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def read_meta(txt):
    m = {}
    for line in open(txt):
        k, _, v = line.strip().partition(" ")
        m[k] = v if k in ("format", "base") else int(v)
    return m


def selftest():
    random.seed(1)
    w, h, pitch = 600, 300, 608  # DXT1, pitch not a multiple of 128 texels
    nx, ny = w // 4, h // 4
    stride = (((pitch + 3) // 4) + 31) & ~31
    mem = bytearray(stride * ((ny + 31) & ~31) * 8 + 4096)
    want = {}
    for y in range(ny):
        for x in range(nx):
            c0, c1 = random.randrange(1, 65535) | 0x8000, random.randrange(0, 0x7fff)
            blk = struct.pack("<HHI", c0, c1, random.getrandbits(32))
            want[x, y] = blk
            o = tiled_offset_2d(x, y, stride, 3)
            mem[o:o + 8] = swap(1, blk)
    meta = dict(format="k_DXT1", width=w, height=h, pitch=pitch, block_width=4, block_height=4, bytes_per_block=8, endian=1, tiled=1)
    got = blocks_of(meta, bytes(mem))
    bad = sum(got[y][x] != want[x, y] for x, y in want)
    img = decode(meta, bytes(mem))
    assert len(img) == h and len(img[0]) == w
    print("selftest: %d blocks, %d wrong" % (len(want), bad))
    return 1 if bad else 0


def main(args):
    if args == ["--selftest"]:
        return selftest()
    bins = []
    for a in args:
        bins += sorted(glob.glob(os.path.join(a, "*.bin"))) if os.path.isdir(a) else [a]
    for b in bins:
        meta = read_meta(b[:-4] + ".txt")
        img = decode(meta, open(b, "rb").read())
        write_png(b[:-4] + ".png", img)
        print(b[:-4] + ".png", meta["format"], meta["width"], meta["height"])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
