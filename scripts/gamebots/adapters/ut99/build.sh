#!/bin/bash
# build.sh -- regenerate GBSchema.uc from schema.py and compile the GameBots
# package against a UT99 (OldUnreal 469e) install.
#
# Usage:
#   ./build.sh <ut99-install-root> [ucc-home-dir]
#
#   <ut99-install-root>  A UT99 tree with System/ + System64/ (the compiled
#                        .u lands in <root>/System/GameBots.u). NEVER pass the
#                        live fleet server's tree -- use a SEPARATE instance;
#                        see the README ("Never touch the live ut99-server").
#   [ucc-home-dir]       HOME to run ucc under (default: a throwaway dir next
#                        to this script). Read on: this matters more than it
#                        looks like it should.
#
# --- three things about this toolchain that will otherwise cost an hour ---
#
# 1. ucc's config/preference directory is $HOME/.utpg/System/, derived from
#    the *process* HOME env var, not argv, not cwd -- confirmed empirically
#    (`ucc help` from an arbitrary cwd prints "using preference directory:
#    $HOME/.utpg/System/"). A UnrealTournament.ini must already exist there
#    (this script does not generate one; see the README's "one-time setup").
#
# 2. **`ucc make`'s compiled output goes into `$HOME/.utpg/System/`, NOT into
#    `<install-root>/System/`**, even though `Paths=../System/*.u` in that
#    same ini points at the install root for READING packages. Compiling
#    writes to the preference dir; the game only looks in the install tree.
#    So after every `ucc make`, the .u has to be copied over by hand -- this
#    script does that copy for you, but if you ever run `ucc make` directly,
#    do not skip it.
#
# 3. **`ucc make` treats a package as already built if a `.u` for it is
#    ANYWHERE on the search path** (`Paths=../System/*.u` -- which includes
#    the install root, not just the preference dir), and skips recompiling it
#    entirely -- even with `-all` ("clean rebuild"), even when the source
#    changed. So after the very first successful build, every SUBSEQUENT one
#    would silently do nothing (no "Parsing"/"Compiling" lines at all) unless
#    the install root's copy is ALSO removed first, which is why this script
#    removes both copies before every run, not just the preference-dir one.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL_ROOT="${1:?Usage: build.sh <ut99-install-root> [ucc-home-dir]}"
UCC_HOME="${2:-$HERE/build/ucc-home}"   # build/ is gitignored (see .gitignore)

if [ ! -d "$INSTALL_ROOT/System64" ]; then
    echo "error: $INSTALL_ROOT does not look like a UT99 install (no System64/)" >&2
    exit 1
fi
if [ ! -f "$UCC_HOME/.utpg/System/UnrealTournament.ini" ]; then
    echo "error: $UCC_HOME/.utpg/System/UnrealTournament.ini not found." >&2
    echo "       ucc needs a system ini there before it will compile anything." >&2
    echo "       See the README's 'one-time setup' section." >&2
    exit 1
fi

echo "==> regenerating GBSchema.uc from schema.py"
python3 "$HERE/gen_gbschema.py" > "$HERE/GameBots/Classes/GBSchema.uc"

echo "==> staging GameBots/Classes into $INSTALL_ROOT"
mkdir -p "$INSTALL_ROOT/GameBots/Classes"
cp "$HERE"/GameBots/Classes/*.uc "$INSTALL_ROOT/GameBots/Classes/"

echo "==> ucc make (HOME=$UCC_HOME)"
# Both copies, or a second build silently does nothing -- see point 3 above.
rm -f "$UCC_HOME/.utpg/System/GameBots.u"
rm -f "$INSTALL_ROOT/System/GameBots.u"
( cd "$INSTALL_ROOT/System64" && HOME="$UCC_HOME" ./ucc-bin-amd64 make )

if [ ! -f "$UCC_HOME/.utpg/System/GameBots.u" ]; then
    echo "error: ucc make did not produce GameBots.u -- check its output above" >&2
    echo "       (a silent no-op here means point 3 above bit us again)" >&2
    exit 1
fi

echo "==> copying compiled GameBots.u into $INSTALL_ROOT/System/"
cp "$UCC_HOME/.utpg/System/GameBots.u" "$INSTALL_ROOT/System/GameBots.u"

echo "==> done: $INSTALL_ROOT/System/GameBots.u"
echo "    Launch with: ...?game=Botpack.DeathMatchPlus?mutator=GameBots.GBMutator"
echo "    Off by default -- set bEnabled=True in \$HOME/.utpg/System/GameBots.ini"
echo "    or 'mutate gb_enable 1' from the server console once it's running."
