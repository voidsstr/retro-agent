"""Regression: HWPROFILE must be able to name the GPU on Windows 98.

WHY A SOURCE TEST AND NOT A REAL ONE. agent/src/hwprofile.c is Win32 and its
Win9x branch is decided entirely by registry layout, so nothing on this Linux
host can execute it; a true-source test would need a fake Win32 registry plus
GDI, SetupAPI and EnumDisplayDevices stubs, which is a larger surface than the
40 lines being protected. What CAN be pinned, and is the thing that actually
broke, is that the Win9x code path exists at all and reads the right keys.

THE BUG THIS ENCODES (found preparing the Pentium-1 Compaq Deskpro, 2026-08-30).
gpu_identify() got the adapter's PCI ids from EnumDisplayDevices' DeviceID
string, then annotated it from the display CLASS key by matching
MatchingDeviceId. Both halves are NT-shaped:

  * On Windows 98 EnumDisplayDevices exists but its DeviceID is frequently
    EMPTY, so parse_ven_dev() gets nothing.
  * The Win9x display class key
    System\\CurrentControlSet\\Services\\Class\\Display\\NNNN has NO
    MatchingDeviceId value at all -- that is an NT invention -- so
    gpu_annotate_from_class() matched nothing either.
  * The old last-resort branch read DriverDesc and stopped, so gpu_ven and
    gpu_dev stayed 0.

Zero is not a harmless "unknown" here. gg_gpu_level_from_pci(0, 0) returns
GG_GPU_UNKNOWN, the gate FAILS OPEN on unknown BY DESIGN, and the outcome is
that the machine with the weakest graphics in the whole fleet is the one
machine whose GPU is never gated -- every Direct3D-only staged title waved onto
a Pentium-1 with a 2D-only VGA. The fail-open default is correct; being unable
to answer on the box that most needs an answer is not.

THE FIX, which this pins: Win9x binds the two halves the other way round from
NT. The PCI instance key carries "Driver" = "Display\\0000", pointing AT the
class key, so the ids are read out of the Enum\\PCI device key's own NAME.
Windows 9x also keeps that enumerator at HKLM\\Enum\\PCI, NOT under
SYSTEM\\CurrentControlSet -- agent/src/video.c's PCISCAN already knew this and
hwprofile.c did not.
"""

import re
from pathlib import Path

import pytest

SRC = Path(__file__).resolve().parents[2] / "agent" / "src" / "hwprofile.c"


@pytest.fixture(scope="module")
def src() -> str:
    assert SRC.is_file(), f"missing {SRC}"
    return SRC.read_text(encoding="utf-8", errors="replace")


def test_the_win9x_pci_walk_exists(src):
    """The whole fix is one function; if it is gone, everything below is
    vacuously true, so assert it first and by name."""
    assert "gpu_ids_from_win9x_enum" in src, (
        "hwprofile.c has no Win9x PCI-id fallback. Without it a Windows 98 box "
        "reports gpu_ven=0, the gate reads GG_GPU_UNKNOWN and fails open, and "
        "every 3D-only title is approved onto a machine that cannot draw a "
        "triangle."
    )
    assert src.index("static int gpu_ids_from_win9x_enum") < src.index(
        "static void gpu_identify"), "must be defined before its only caller"


def test_it_reads_the_win9x_enumerator_root(src):
    r"""HKLM\Enum\PCI, not SYSTEM\CurrentControlSet\Enum\PCI.

    Windows 9x keeps the hardware enumerator at the top of HKLM; only the NT
    family moved it under SYSTEM\CurrentControlSet. Getting this wrong is
    indistinguishable from "the box has no graphics card" -- the same class of
    silent negative CLAUDE.md's case-insensitivity rule is about.
    """
    fn = _win9x_fn(src)
    assert '"Enum\\\\PCI"' in fn, (
        r"the Win9x root HKLM\Enum\PCI is not read; on 98 the NT path "
        r"SYSTEM\CurrentControlSet\Enum\PCI does not exist"
    )
    # video.c is the file that already had this right; keep them agreeing.
    video = (SRC.parent / "video.c").read_text(encoding="utf-8",
                                               errors="replace")
    assert '"Enum\\\\PCI"' in video


def test_it_binds_through_the_driver_value_not_matchingdeviceid(src):
    r"""On 9x the PCI instance points AT the class key via "Driver".

    NT points the other way (class key -> MatchingDeviceId -> hardware id), and
    hwprofile.c's NT annotator searches in that direction. A Win9x fallback
    that also looked for MatchingDeviceId would find nothing, which is exactly
    the bug it replaces.
    """
    fn = _win9x_fn(src)
    assert '"Driver"' in fn, 'the Win9x binding value "Driver" is not read'
    assert "Display\\\\" in fn, (
        r'the binding must be recognised as "Display\NNNN"')
    # CODE only: the comment above the class-key read legitimately explains
    # that MatchingDeviceId is what is NOT available here, and a naive text
    # search would fail on its own explanation.
    assert "MatchingDeviceId" not in _strip_comments(fn), (
        "MatchingDeviceId is an NT value and does not exist on Win9x; looking "
        "for it here reintroduces the original failure"
    )


def test_the_binding_match_is_case_insensitive(src):
    """A Windows registry value compared from a Linux-built binary.

    CLAUDE.md: a case-sensitive comparison against a Windows string is how this
    project has repeatedly concluded something is absent when it is right
    there. Win98 writes these keys in mixed case ("Display\\0000") and the
    device key names in lower case ("ven_5333&dev_8901").
    """
    fn = _win9x_fn(src)
    assert "_strnicmp" in fn and "strncmp(binding" not in fn, (
        "the Driver binding must be compared case-insensitively (_strnicmp)"
    )
    # parse_ven_dev upper-cases its input before searching, which is what makes
    # a lower-case Win98 key name match. Pin that, since the fallback relies on
    # it rather than doing its own case handling.
    pv = src[src.index("static int parse_ven_dev"):]
    pv = pv[:pv.index("\n}\n")]
    assert "- 32" in pv and 'strstr(up, "VEN_")' in pv, (
        "parse_ven_dev must upper-case before searching, or a Win98 "
        "'ven_5333&dev_8901' key name yields no ids"
    )


def test_it_is_only_reached_when_the_nt_path_gave_nothing(src):
    """It must not override a good answer.

    EnumDisplayDevices(attached-to-desktop) is the source of truth wherever it
    works -- picking the first display-class registry key instead is what makes
    VIDEODIAG report hardware that was removed months ago. So the fallback is
    guarded on BOTH "we have no vendor id yet" and "this is not NT".
    """
    call = src.index("gpu_ids_from_win9x_enum(g)")
    guard = _guard_above(src, call)
    assert "!g->ven" in guard and "!is_nt" in guard, (
        f"fallback guard is {guard!r}; it must fire only when the NT path "
        f"produced no vendor id AND the box is Win9x"
    )


def test_the_name_only_lookup_no_longer_shadows_the_ids(src):
    """The old fallback read DriverDesc under `if (!g->ven && !is_nt)`.

    Leaving it on that condition means it runs first, fills in nothing useful,
    and the ids stay zero. It is now conditioned on the NAME being empty, so it
    can still supply a description without standing in for the ids.
    """
    i = src.index("Display\\\\0000")
    guard = _guard_above(src, i)
    assert "!g->name[0]" in guard, (
        f"the DriverDesc-only lookup is guarded by {guard!r}; it must key off "
        f"the missing NAME, not the missing vendor id"
    )


# --------------------------------------------------------------------------

def _win9x_fn(src: str) -> str:
    """The whole body of gpu_ids_from_win9x_enum, by brace matching.

    Not by searching for the next "\n}\n": this function contains nested
    blocks and a naive scan clips it in half, which silently turns every
    assertion below into a test of the first twenty lines.
    """
    start = src.index("static int gpu_ids_from_win9x_enum")
    i = src.index("{", start)
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[start:j + 1]
    raise AssertionError("unterminated gpu_ids_from_win9x_enum")


def _guard_above(src: str, pos: int) -> str:
    """The nearest `if (...)` above `pos` that tests a gpu_t field.

    The nearest `if` of any kind is usually the RegOpenKeyExA call itself, so
    this looks specifically for the branch that decides whether the block runs.
    """
    head = src[:pos]
    for m in reversed(list(re.finditer(r"if \([^\n]*", head))):
        if "g->" in m.group(0) or "is_nt" in m.group(0):
            return m.group(0)
    raise AssertionError(f"no gpu guard found above offset {pos}")


def _strip_comments(c: str) -> str:
    """Remove /* ... */ and // ... so an assertion tests code, not prose."""
    out, i, n = [], 0, len(c)
    while i < n:
        if c.startswith("/*", i):
            i = c.find("*/", i)
            i = n if i < 0 else i + 2
        elif c.startswith("//", i):
            j = c.find("\n", i)
            i = n if j < 0 else j
        else:
            out.append(c[i])
            i += 1
    return "".join(out)
