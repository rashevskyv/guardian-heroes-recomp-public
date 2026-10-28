#!/bin/bash
# Builds the Switch NRO in WSL. Paths follow CLAUDE.md "Build layout on this PC":
#   sources /root/gh/src (rsync'd from the git checkout), SDK /root/gh/rexglue-sdk (stuntdouble's
#   patches/sdk + switch/sdk-edits.py applied), codegen output generated/default from the Linux step,
#   NVK /root/cbfd/nvk-sdk-real/lib/libvulkan.a, devkitPro /opt/devkitpro.
#   switch/build.sh [ninja targets...]
# Every run reconfigures with the version from git ("0.<commit count>.<sha7>", the NACP's display
# version, hbmenu shows it) and writes gh_version.h (the same string + the build time, the first
# log line), so a log and an NRO can always be matched to a commit.
set -eu
ROOT=${GH_ROOT:-/root/gh}
SRC=$ROOT/src
SDK=${REXSDK_DIR:-$ROOT/rexglue-sdk}
NVK_SDK=${NVK_SDK:-/root/cbfd/nvk-sdk-real}
GIT_DIR=${GH_GIT_DIR:-/mnt/d/git/dev/gh-recomp}
export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro}
export PATH=$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH
BUILD=$SRC/out/build/switch

mkdir -p "$SRC"
rsync -a --delete --exclude /out --exclude /game --exclude /generated/default --exclude /.git "$GIT_DIR/" "$SRC/"
[ -f "$NVK_SDK/lib/libvulkan.a" ] || { echo "no NVK: $NVK_SDK/lib/libvulkan.a (cbfd-switch switch/build-nvk.sh)"; exit 1; }
[ -f "$SRC/generated/default/sources.cmake" ] || { echo "no codegen output: run the Linux build first"; exit 1; }

python3 "$SRC/switch/sdk-edits.py" "$SDK"
python3 "$SRC/patches/sdk-edits-gh.py" "$SDK"

COUNT=$(git -c safe.directory="*" -C "$GIT_DIR" rev-list --count HEAD 2>/dev/null || echo 0)
SHA=$(git -c safe.directory="*" -C "$GIT_DIR" rev-parse --short=7 HEAD 2>/dev/null || echo unknown)
DIRTY=$(git -c safe.directory="*" -C "$GIT_DIR" status --porcelain -uno 2>/dev/null | grep -q . && echo "+" || true)
VERSION="0.$COUNT.$SHA$DIRTY"
STAMP=$(date -u +%Y-%m-%dT%H:%MZ)
mkdir -p "$BUILD"
printf '#define GH_VERSION "%s"\n#define GH_BUILD_UTC "%s"\n' "$VERSION" "$STAMP" > "$BUILD/gh_version.h.new"
if ! cmp -s "$BUILD/gh_version.h.new" "$BUILD/gh_version.h" 2>/dev/null; then
    mv "$BUILD/gh_version.h.new" "$BUILD/gh_version.h"
else
    rm -f "$BUILD/gh_version.h.new"
fi
echo "version $VERSION, built $STAMP"

cmake -S "$SRC" -B "$BUILD" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DREXSDK_DIR="$SDK" -DNVK_SDK="$NVK_SDK" -DGH_VERSION="$VERSION" \
    -DREXGLUE_ENABLE_TRACY=OFF -DREXGLUE_ENABLE_PERF_COUNTERS=ON > "$BUILD/configure.log" 2>&1 \
    || { tail -20 "$BUILD/configure.log"; exit 1; }
# A failed build must not leave the previous NRO behind for deploy.sh.
rm -f "$BUILD/gh.nro"
cmake --build "$BUILD" --parallel "${GH_JOBS:-12}" ${1:+--target "$@"}
ls -la "$BUILD"/gh.nro "$BUILD"/gh.elf 2>/dev/null || true
sha1sum "$BUILD"/gh.nro 2>/dev/null || true
# What hbmenu will show; the NACP version must be this build's (nx_create_nro ignored it until 0.55).
python3 "$SRC/tools/nro_nacp.py" "$BUILD/gh.nro" "$VERSION"

# --- Native renderer staging (docs/native-renderer-plan.md M3): the present shaders and the game's
# shaders as DKSH next to the NRO in $BUILD/stage/ (switch/eden-deko3d.ps1 copies it to Eden's SD,
# deploy.sh to the console). The game's shaders come from the trace backend's --dump_shaders
# directory ($GH_SHADER_DUMPS, default /root/gh/shaders_trace) through tools/xenos2glsl.py + uam.
STAGE=$BUILD/stage
rm -rf "$STAGE"
mkdir -p "$STAGE/shaders"
for s in vert frag; do
    uam -s $s -o "$STAGE/shaders/present.$s.dksh" "$SRC/src/gpu/shaders/present.$s"
done
SHADER_DUMPS=${GH_SHADER_DUMPS:-$ROOT/shaders_trace}
if [ -d "$SHADER_DUMPS" ]; then
    python3 "$SRC/tools/xenos2glsl.py" --all "$SHADER_DUMPS" --out-dir "$BUILD/glsl" --check | tail -1
    for f in "$BUILD"/glsl/*.glsl.dksh; do
        b=$(basename "$f" .glsl.dksh)
        cp "$f" "$STAGE/shaders/$b.dksh"
    done
fi
cp "$BUILD/gh.nro" "$STAGE/gh.nro"
touch "$STAGE/deko3d"
echo "stage: $STAGE ($(ls "$STAGE/shaders" | wc -l) shaders)"
