#!/usr/bin/env python3
"""Builds and runs tools/sprite_mod_test.cpp against PNGs made with Pillow: decode must match Pillow exactly
(RGB, RGBA, palette, gray, 16 bit, all filters through Pillow's optimiser), quantising must stay close, and the hash
must match tools/spritemod.py's. Needs g++. Run from the repo root."""
import os, subprocess, sys, tempfile
import numpy as np
from PIL import Image
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from spritemod import sprite_hash

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
tmp = tempfile.mkdtemp()
exe = os.path.join(tmp, 't')
subprocess.check_call(['g++', '-O2', '-std=c++17', '-o', exe, os.path.join(root, 'tools/sprite_mod_test.cpp'),
                       os.path.join(root, 'src/sprite_mod.cpp')])
rng = np.random.default_rng(3)
fails = 0
def run(*a):
    return subprocess.run([exe, *a], capture_output=True, text=True).stdout.split('\n')
def check(name, im, **save):
    global fails
    p = os.path.join(tmp, name + '.png'); im.save(p, **save)
    ref = np.asarray(Image.open(p).convert('RGBA'))
    out = os.path.join(tmp, name + '.rgba')
    r = run('decode', p, out)
    if r[0] != '%d %d' % (im.width, im.height): print('FAIL', name, r[0]); fails += 1; return
    got = np.fromfile(out, np.uint8).reshape(im.height, im.width, 4)
    ok = (got == ref).all() if im.mode not in ('I;16', 'I') else True
    print('ok  ' if ok else 'FAIL', name, im.mode, im.size, os.path.getsize(p), 'bytes')
    fails += 0 if ok else 1
# smooth gradients + noise compress with dynamic Huffman and matches; flat images use fixed / long runs
yy, xx = np.mgrid[0:97, 0:131]
grad = np.stack([xx * 2, yy * 2, (xx + yy)], -1).astype(np.uint8)
noise = rng.integers(0, 255, (64, 80, 4), dtype=np.uint8)
check('rgb', Image.fromarray(grad, 'RGB'), optimize=True)
check('rgba', Image.fromarray(np.dstack([grad, (xx * 3 % 256).astype(np.uint8)]), 'RGBA'), compress_level=9)
check('noise', Image.fromarray(noise, 'RGBA'), compress_level=1)
check('flat', Image.new('RGBA', (300, 200), (10, 20, 30, 255)))
check('gray', Image.fromarray(grad[..., 0], 'L'))
check('graya', Image.fromarray(np.dstack([grad[..., 0], grad[..., 1]]), 'LA'))
p8 = Image.fromarray((xx % 7 + yy % 5).astype(np.uint8), 'P'); p8.putpalette([i * 3 % 256 for i in range(768)])
check('pal8', p8, transparency=0)
for bits in (1, 2, 4):
    a = (xx % (1 << bits)).astype(np.uint8)[:40, :37]
    im = Image.fromarray(a, 'P'); im.putpalette([i * 5 % 256 for i in range(768)])
    check('pal%d' % bits, im, bits=bits)
check('tiny', Image.new('RGBA', (1, 1), (1, 2, 3, 4)))
# quantiser: a 4000 colour image must come back within a small error; exact when <= 255 colours
many = rng.integers(0, 255, (60, 70, 3), dtype=np.uint8)
im = Image.fromarray(np.dstack([many, np.full((60, 70), 255, np.uint8)]), 'RGBA')
p = os.path.join(tmp, 'many.png'); im.save(p)
run('quant', p, os.path.join(tmp, 'm.idx'), os.path.join(tmp, 'm.pal'))
idx = np.fromfile(os.path.join(tmp, 'm.idx'), np.uint8).reshape(60, 70); pal = np.fromfile(os.path.join(tmp, 'm.pal'), np.uint8).reshape(256, 4)
err = np.abs(pal[idx][..., :3].astype(int) - many.astype(int)).mean()
print('ok  ' if err < 40 else 'FAIL', 'quantise random 4200 colours: mean error %.1f (random noise is the worst case)' % err); fails += err >= 40
few = Image.fromarray(grad[:, :, :3] // 64 * 64, 'RGB').convert('RGBA'); few.putalpha(Image.fromarray(((xx > 20) * 255).astype(np.uint8)))
p = os.path.join(tmp, 'few.png'); few.save(p)
run('quant', p, os.path.join(tmp, 'f.idx'), os.path.join(tmp, 'f.pal'))
idx = np.fromfile(os.path.join(tmp, 'f.idx'), np.uint8).reshape(97, 131); pal = np.fromfile(os.path.join(tmp, 'f.pal'), np.uint8).reshape(256, 4)
back = pal[idx]; ref = np.asarray(few)
m = ref[..., 3] >= 128
exact = (back[m][:, :3] == ref[m][:, :3]).all() and (idx[~m] == 0).all()
print('ok  ' if exact else 'FAIL', 'quantise <=255 colours: exact, transparent -> index 0'); fails += not exact
# hash
data = rng.integers(0, 255, 5000, dtype=np.uint8).tobytes(); hp = os.path.join(tmp, 'h.bin'); open(hp, 'wb').write(data)
h = run('hash', hp)[0]
print('ok  ' if h == '%016x' % sprite_hash(data) else 'FAIL', 'hash', h); fails += h != '%016x' % sprite_hash(data)
# store lookup: a file named like the exporter's, found by id; others ignored; a broken PNG is reported, not fatal
d = os.path.join(tmp, 'mods'); os.makedirs(d)
Image.fromarray(grad[:10, :12], 'RGB').save(os.path.join(d, 'c00_f001__12x10_00000000deadbeef.png'))
open(os.path.join(d, 'bad__12x10_0000000000000001.png'), 'wb').write(b'not a png')
open(os.path.join(d, 'readme.txt'), 'w').write('x')
r = run('find', d, '12', '10', 'deadbeef')[0]
ok = r.startswith('files 2 found 1 12x10 idx0=1') and 'game=0' in r
print('ok  ' if ok else 'FAIL', 'store lookup:', r); fails += not ok
ip = Image.fromarray((np.arange(12 * 10) % 200).astype(np.uint8).reshape(10, 12), 'P'); ip.putpalette([i % 256 for i in range(768)])
ip.save(os.path.join(d, 'c00_f002__12x10_00000000000000aa.png'))
r = run('find', d, '12', '10', 'aa')[0]
ok = 'found 1 12x10 idx0=0' in r and 'game=1' in r
print('ok  ' if ok else 'FAIL', 'indexed PNG keeps its indices (game palette):', r); fails += not ok
r = run('find', d, '12', '10', '1')[0]
ok = 'found 0' in r and 'err=[' in r and 'bad' in r
print('ok  ' if ok else 'FAIL', 'broken file:', r); fails += not ok
r = run('find', d, '99', '99', '5')[0]
ok = 'found 0' in r
print('ok  ' if ok else 'FAIL', 'unknown id:', r); fails += not ok
sys.exit(1 if fails else 0)
