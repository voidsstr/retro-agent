"""Regression: FLEETRES.EXE must be executable by a genuine Pentium 1.

WHY THIS ONE BINARY GETS ITS OWN TEST. FLEETRES.EXE is not a tool somebody runs
occasionally -- it is staged into 32 game trees and `call`ed by the FIRST LINE
of every "Play <Game>.bat". If it will not start, the entire staged library
fails at launch on that box, every title, with an error naming our helper
rather than the game.

THE DEFECT (found 2026-08-30, preparing the Pentium-1 Compaq Deskpro). The
documented build line was

    i686-w64-mingw32-gcc -O2 -s -o FLEETRES.EXE fleetres.c -ladvapi32 -luser32

which produces a binary carrying **78 CMOV instructions**. CMOV is a Pentium
PRO instruction; a genuine Pentium (P54C/P55C) raises STATUS_ILLEGAL_INSTRUCTION
0xC000001D on the first one. Nothing on an XP box can show you this -- every
other machine in the fleet is i686 or later and executes CMOV happily -- so it
is invisible until the one machine it breaks is switched on.

  * 25 of the 78 were in our own code, from gcc's default i686 baseline.
  * 51 were inside MINGW'S OWN printf (__mingw_pformat, __pformat_*, __gdtoa),
    which ships prebuilt for i686.
  * 2 remain in the fixed binary and are DEAD: _mark_section_writable and
    __GetPEImageBase are libgcc pseudo-relocator helpers, never called when the
    image has no runtime pseudo-relocs.

THE PART WORTH REMEMBERING: none of this was new knowledge. agent/Makefile has
carried the entire recipe -- and the note that it "surfaced on a Compaq Deskpro
2000 (Pentium 1)" -- since the agent was made P5-safe. FLEETRES.EXE was written
later and simply did not inherit it. So this test pins BOTH files: the flags in
provisioning/fleetres/build.sh, and the fact that agent/Makefile still has them
too, because the day someone "cleans up" the Makefile is the day the reference
recipe disappears.

Verified on hardware (.240, Athlon 64, XP SP3, 2026-08-30): the P5-safe build's
`-cmd` and `-info` output is byte-for-byte identical to the old binary's, and
the binary halves in size (59,392 -> 30,208 bytes) because mingw's pformat goes.
"""

import re
import shutil
import subprocess
from pathlib import Path

import pytest

REPO = Path(__file__).resolve().parents[2]
FR = REPO / "provisioning" / "fleetres"
EXE = FR / "FLEETRES.EXE"
BUILD = FR / "build.sh"
MAKEFILE = REPO / "agent" / "Makefile"

# The two libgcc pseudo-relocator helpers that are present but unreachable.
DEAD_CMOV_MAX = 2


def _objdump():
    return shutil.which("objdump")


def test_the_build_script_carries_both_halves_of_the_fix():
    """-march alone is NOT enough, and that is the trap.

    -march=i586 takes our own code from 25 CMOVs to zero and leaves 53,
    because mingw's ANSI stdio is a prebuilt i686 object. Someone fixing this
    from first principles reaches for -march, measures "much better", and
    ships a binary that still dies on the first printf.
    """
    assert BUILD.is_file(), f"missing {BUILD}"
    txt = BUILD.read_text(encoding="utf-8")
    assert "-march=i586" in txt, (
        "build.sh does not pin -march=i586; gcc's default i686 baseline emits "
        "CMOV, which a genuine Pentium cannot execute"
    )
    assert "__USE_MINGW_ANSI_STDIO=0" in txt, (
        "build.sh does not set -D__USE_MINGW_ANSI_STDIO=0. Without it "
        "__mingw_pformat/__gdtoa are linked in, and those are prebuilt for "
        "i686 and carry ~51 CMOVs on the printf path -- so the binary still "
        "dies on a Pentium 1 even with -march=i586"
    )


def test_the_build_script_refuses_a_regressed_binary():
    """A tolerated failure must SAY it is a failure (CLAUDE.md).

    A build that quietly regressed this would be copied into 32 game trees and
    only surface on the one box nobody tests on.
    """
    txt = BUILD.read_text(encoding="utf-8")
    assert "BUILD FAILED" in txt and "cmov" in txt.lower(), (
        "build.sh does not self-check the CMOV count; a regression would ship "
        "silently to every staged tree"
    )
    assert "exit 1" in txt


def test_the_agent_makefile_still_carries_the_reference_recipe():
    """agent/Makefile is where this was worked out; keep it there.

    If the flags are ever dropped from the Makefile the agent itself stops
    booting on that box, and the written explanation this fix was recovered
    from disappears with them.
    """
    mk = MAKEFILE.read_text(encoding="utf-8")
    assert "-march=i586" in mk
    assert "__USE_MINGW_ANSI_STDIO=0" in mk
    assert "CMOV" in mk, (
        "agent/Makefile no longer explains WHY these flags are there; that "
        "comment is the only record of how the Pentium-1 crash was diagnosed"
    )


@pytest.mark.skipif(_objdump() is None, reason="objdump not installed")
def test_the_shipped_binary_is_p5_safe():
    """The actual artifact, not just the recipe.

    build.sh being right proves nothing about the FLEETRES.EXE that is in git
    and gets copied to the share -- and it is the artifact that reaches the
    box.
    """
    assert EXE.is_file(), f"missing {EXE}"
    out = subprocess.run(
        [_objdump(), "-d", "-M", "intel", "--no-show-raw-insn", str(EXE)],
        capture_output=True, text=True, timeout=300, errors="replace")
    assert out.returncode == 0, out.stderr[:300]
    hits = [ln.strip() for ln in out.stdout.splitlines()
            if re.search(r"\bcmov[a-z]{1,4}\b", ln)]
    assert len(hits) <= DEAD_CMOV_MAX, (
        f"FLEETRES.EXE contains {len(hits)} CMOV instructions (at most "
        f"{DEAD_CMOV_MAX} dead ones are allowed). CMOV is Pentium Pro and "
        f"later; a genuine Pentium 1 raises 0xC000001D on the first one, and "
        f"this binary is called by the first line of every staged game's "
        f"launcher. Rebuild with provisioning/fleetres/build.sh.\n  "
        + "\n  ".join(hits[:8])
    )


@pytest.mark.skipif(_objdump() is None, reason="objdump not installed")
def test_the_shipped_binary_has_no_sse_at_all():
    """A Pentium 1 has no SSE either, and unlike CMOV there is no dead-code
    excuse to make for it."""
    out = subprocess.run(
        [_objdump(), "-d", "-M", "intel", "--no-show-raw-insn", str(EXE)],
        capture_output=True, text=True, timeout=300, errors="replace")
    hits = [ln.strip() for ln in out.stdout.splitlines()
            if re.search(r"\bxmm\d", ln)]
    assert not hits, (
        "FLEETRES.EXE references XMM registers; a Pentium 1 has no SSE.\n  "
        + "\n  ".join(hits[:8])
    )
