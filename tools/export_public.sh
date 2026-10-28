#!/bin/bash
# Exports the work repo (private) as a new commit on top of the public repo's main (no force push).
#   tools/export_public.sh          dry run: builds the tree, runs the audit, shows the diff, pushes nothing
#   tools/export_public.sh --push   same, then commits and pushes to origin/main of the public repo
# Run in Git Bash (needs git and gh). The work tree must be clean; only tracked files are exported.
# Left out: CLAUDE.md, AGENTS.md, internal docs (TASK-*, SPRITES-RESEARCH), docs images, eden-*.ps1.
# Never changes a repo's visibility. Releases go to the public repo only (see scripts/package-release.sh).
set -eu
PUBLIC=${GH_PUBLIC_REPO:-rashevskyv/guardian-heroes-recomp-public}
cd "$(dirname "$0")/.."
git diff --quiet HEAD -- || { echo "work tree has uncommitted changes to tracked files" >&2; exit 1; }
DEV=$(git rev-parse --short=7 HEAD)
VERSION="0.$(git rev-list --count HEAD).$DEV"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
gh repo clone "$PUBLIC" "$T/pub" -- -q
git -C "$T/pub" rm -rq --ignore-unmatch . > /dev/null
mkdir "$T/tree"
git archive HEAD | tar -x -C "$T/tree"
cd "$T/tree"
rm -rf CLAUDE.md AGENTS.md docs/TASK-* docs/SPRITES-RESEARCH.md
find . -type f \( -iname '*.png' -o -iname '*.jpg' -o -iname '*.jpeg' -o -iname '*.gif' -o -iname '*.webp' \
    -o -iname '*.bmp' -o -iname '*.ppm' -o -iname '*.tga' -o -iname '*.dds' -o -iname 'eden-*.ps1' \) -delete

# Audit: stop on anything that must not be public.
fail=0
big=$(find . -type f -size +800k); [ -z "$big" ] || { echo "AUDIT: files over 800 KB:"; echo "$big"; fail=1; }
bad=$(find . -type f \( -iname '*.xex' -o -iname '*.zip' -o -iname '*.xwb' -o -iname '*.wmv' -o -iname '*.mrg' \
    -o -iname '*.mrgc' -o -iname '*.xprc' -o -iname '*.nro' -o -iname '*.elf' -o -iname '*.bin' -o -iname '*.pkg' \
    -o -iname '*.7z' -o -iname '*.gz' \) ; find . -path './game/*' -o -path './generated/default/*')
[ -z "$bad" ] || { echo "AUDIT: game or build files:"; echo "$bad"; fail=1; }
hits=$(grep -rIlE --exclude=export_public.sh '192\.168\.|Administrator|C:.Users|conker-data|[A-Za-z]:.Emulators|rashevsky\.v@|ghp_|gho_' . || true)
[ -z "$hits" ] || { echo "AUDIT: local paths, LAN IPs or secrets in:"; echo "$hits"; fail=1; }
[ $fail = 0 ] || { echo "audit failed, nothing exported" >&2; exit 1; }
echo "audit ok: $(find . -type f | wc -l) files"

cd "$T/pub"
tar -C "$T/tree" -c . | tar -x
git add -A
git status --short | head -50
if git diff --cached --quiet; then echo "public repo is already up to date"; exit 0; fi
if [ "${1:-}" != --push ]; then echo "dry run: not pushed (use --push)"; exit 0; fi
git commit -q -m "Update from the work repo $DEV (build version $VERSION)"
git push origin HEAD:main
echo "pushed to $PUBLIC"
