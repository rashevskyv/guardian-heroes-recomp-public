#!/bin/bash
# Packs the Windows x86-64 build (tools/win-msvc-build.cmd) into out/release/gh-<version>-windows-x86_64.zip (WSL).
#   scripts/package-windows.sh <version>      e.g. 0.45.20bf26b; the build's built_commit.txt must equal the sha in it
# App-local MSVC runtime DLLs (VS Build Tools redistributables) go into the zip: no redistributable install is needed.
# Checks the zip holds no game data. Run `sha256sum` afterwards and add the line to SHA256SUMS.
set -eu
V=${1:?version, e.g. 0.45.20bf26b}
BUILD=${GH_WIN_BUILD:-/mnt/c/proj/gh/build-rel}
GIT_DIR=${GH_GIT_DIR:-/mnt/d/git/dev/gh-recomp}
CRT=$(ls -d "/mnt/c/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/Redist/MSVC/"*/x64/Microsoft.VC143.CRT | head -1)
[ "$(tr -d '\r\n ' < "$BUILD/built_commit.txt")" = "${V##*.}" ] || { echo "Windows build is not $V (built_commit.txt: $(cat "$BUILD/built_commit.txt"))" >&2; exit 1; }
OUT=$GIT_DIR/out/release
D=$OUT/pkg-win/gh-$V-windows-x86_64
rm -rf "$OUT/pkg-win"; mkdir -p "$D"
cp "$BUILD/gh.exe" "$BUILD/rexruntime.dll" "$BUILD/rexgpu-xenos.dll" "$D/"
for n in msvcp140.dll msvcp140_atomic_wait.dll vcruntime140.dll vcruntime140_1.dll; do cp "$CRT/$n" "$D/"; done
printf '@echo off\r\nrem Put the game folder (default.xex + media) in "game" next to gh.exe, or pass --game_data_root=<folder>\r\ncd /d "%%~dp0"\r\ngh.exe %%*\r\n' > "$D/gh.bat"
cp "$GIT_DIR/LICENSE" "$D/LICENSE"
cp "$GIT_DIR/README.md" "$D/README.md"
echo "$V" > "$D/VERSION"
bad=$(find "$OUT/pkg-win" -type f \( -iname '*.xex' -o -iname '*.zip' -o -iname '*.xwb' -o -iname '*.wmv' -o -iname '*.mrg' -o -iname '*.xprc' \)) || true
[ -z "$bad" ] || { echo "game data in the package: $bad" >&2; exit 1; }
(cd "$OUT/pkg-win" && rm -f "../gh-$V-windows-x86_64.zip" && zip -qr "../gh-$V-windows-x86_64.zip" .)
rm -rf "$OUT/pkg-win"
ls -la "$OUT/gh-$V-windows-x86_64.zip"; (cd "$OUT" && sha256sum "gh-$V-windows-x86_64.zip")
