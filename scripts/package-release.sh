#!/bin/bash
# Packs the release files in WSL/Linux into out/release/: Switch zip, Linux tar.gz, SHA256SUMS.
#   scripts/package-release.sh            (build first: switch/build.sh and tools/wsl-linux.sh build)
# The version comes from git ("0.<commit count>.<sha7>") and must match the NRO's: build at the commit you tag.
set -eu
ROOT=${GH_ROOT:-/root/gh}
GIT_DIR=${GH_GIT_DIR:-/mnt/d/git/dev/gh-recomp}
OUT=$GIT_DIR/out/release
SWITCH=$ROOT/src/out/build/switch
LINUX=$ROOT/src/out/build/linux-amd64-release
SDKOUT=$ROOT/rexglue-sdk/out/linux-amd64
V=0.$(git -c safe.directory="*" -C "$GIT_DIR" rev-list --count HEAD).$(git -c safe.directory="*" -C "$GIT_DIR" rev-parse --short=7 HEAD)
grep -q "\"$V\"" "$SWITCH/gh_version.h" || { echo "Switch build is not $V: $(cat "$SWITCH/gh_version.h")" >&2; exit 1; }
rm -rf "$OUT"; mkdir -p "$OUT/pkg"
cd "$OUT"

# Switch: unzip to the SD root.
S=pkg/switch/switch/gh
mkdir -p $S
cp "$SWITCH/stage/gh.nro" "$SWITCH/stage/deko3d" $S/
cp -r "$SWITCH/stage/shaders" $S/
cp "$GIT_DIR/LICENSE" "$GIT_DIR/README.md" pkg/switch/
(cd pkg/switch && zip -qr "../../gh-$V-switch.zip" .)

# Linux: gh + the two shared libraries, RUNPATH is $ORIGIN.
L=pkg/gh-$V-linux-x86_64
mkdir -p $L
cp "$LINUX/gh" $L/gh
cp "$SDKOUT/librexruntime.so" "$SDKOUT/librexgpu-xenos.so" $L/
strip $L/gh $L/*.so
printf '#!/bin/sh\nd=$(cd "$(dirname "$0")" && pwd)\nexport LD_LIBRARY_PATH="$d${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"\nexec "$d/gh" "$@"\n' > $L/gh.sh
chmod +x $L/gh $L/gh.sh
cp "$GIT_DIR/LICENSE" "$GIT_DIR/README.md" $L/
echo "$V" > $L/VERSION
tar -C pkg -czf "gh-$V-linux-x86_64.tar.gz" "gh-$V-linux-x86_64"

rm -rf pkg
sha256sum gh-$V-* > SHA256SUMS
ls -la; cat SHA256SUMS

# Releases go ONLY to the public repo (the work repo stays private; never change a repo's visibility).
# Publish after tools/export_public.sh --push, only when asked. Tag = build version, notes name the work commit:
echo "gh release create v$V gh-$V-switch.zip gh-$V-linux-x86_64.tar.gz SHA256SUMS -R rashevskyv/guardian-heroes-recomp-public --title \"Guardian Heroes recompilation $V\" --notes \"Built from work commit ${V##*.}.\""
# Windows: build with tools/win-msvc-build.cmd at the same commit, then `scripts/package-windows.sh $V` and add its zip to the release + SHA256SUMS.
