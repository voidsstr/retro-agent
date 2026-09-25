#!/bin/bash
# Regenerate patches/mesafx-voodoo2-icd.patch from the fork clone's working tree.
#
# The patch is the ONLY tracked copy of our ICD source changes (the clone under
# build/ is gitignored), so every modified or added SOURCE file must be in it.
# The file list used to be typed by hand and twice nearly dropped a new file
# (fxicd.c, imports.h); it is now derived from git status. Build outputs
# (.o/.a/.dll/.res) are excluded.
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
GL="${GL_TREE:-$HERE/build/retro3dfx-gl}"
[ -d "$GL/.git" ] || GL="/home/voidsstr/development/retro-agent/voodoo-cleanroom/build/retro3dfx-gl"
OUT="${1:-$HERE/patches/mesafx-voodoo2-icd.patch}"
cd "$GL"
# new source files must be intent-to-add or git diff omits them
{ git ls-files --others --exclude-standard -- src | grep -E '\.(c|h|def|S|s)$' || true; } | xargs -r git add -N
mapfile -t FILES < <(git status --porcelain | awk '{print $2}' \
    | grep -Ev '\.(o|a|dll|res|exe|lib)$' | sort)
git diff -- "${FILES[@]}" > "$OUT"
echo "wrote $OUT: $(grep -c '^diff --git' "$OUT") files"
