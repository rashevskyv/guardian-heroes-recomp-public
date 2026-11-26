#!/usr/bin/env python3
"""Makes a mod folder from a folder of PNG files (docs/SPRITE-REPLACEMENT.md, "Make a mod folder").

  python3 -I tools/make_sprite_mod.py <png folder> <out folder> --id hd-heroes --name "HD Heroes" --author "Me"
          [--version 1.0] [--description "..."] [--license CC-BY-4.0] [--kind sprites|textures] [--order 0]

Writes <out folder>/<id>/mod.toml and copies the PNGs to <out folder>/<id>/sprites/ (or textures/).
Copy <out folder>/<id>/ to sdmc:/switch/gh/mods/. The source folder is not changed.
"""
import argparse, os, re, shutil, sys


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('src')
    ap.add_argument('out')
    ap.add_argument('--id', required=True)
    ap.add_argument('--name', required=True)
    ap.add_argument('--author', default='')
    ap.add_argument('--version', default='1.0')
    ap.add_argument('--description', default='')
    ap.add_argument('--license', default='')
    ap.add_argument('--kind', default='sprites', choices=['sprites', 'textures'])
    ap.add_argument('--order', type=int, default=0)
    a = ap.parse_args()
    if not re.fullmatch(r'[A-Za-z0-9_-]{1,31}', a.id) or a.id.startswith('legacy-'):
        sys.exit('the id must be 1-31 characters of a-z, 0-9, _ and -, and not start with legacy-')
    pngs = sorted(f for f in os.listdir(a.src) if f.lower().endswith('.png'))
    if not pngs:
        sys.exit('no PNG files in ' + a.src)
    bad = [f for f in pngs if not re.search(r'(^|__)\d+x\d+_[0-9a-f]{16}\.png$', f)]
    if bad:
        print('warning: %d file(s) do not end with <w>x<h>_<hash>.png and will not be used, e.g. %s' % (len(bad), bad[0]))
    dst = os.path.join(a.out, a.id, a.kind)
    os.makedirs(dst, exist_ok=True)
    for f in pngs:
        shutil.copy2(os.path.join(a.src, f), os.path.join(dst, f))
    q = lambda v: '"' + v.replace('"', "'") + '"'
    with open(os.path.join(a.out, a.id, 'mod.toml'), 'w', encoding='utf-8', newline='\n') as fp:
        fp.write('id = %s\nname = %s\nversion = %s\nauthor = %s\ndescription = %s\nkind = %s\nlicense = %s\norder = %d\n' % (
            q(a.id), q(a.name), q(a.version), q(a.author), q(a.description), q(a.kind), q(a.license), a.order))
    print('%s: mod.toml and %d file(s) in %s/' % (os.path.join(a.out, a.id), len(pngs), a.kind))


if __name__ == '__main__':
    main()
