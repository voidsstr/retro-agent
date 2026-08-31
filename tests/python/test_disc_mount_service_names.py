"""The disc-mount probe matches SERVICE names, not product names.

THE DEFECT, measured on .246 on 2026-08-31. `caps_detect()` listed the string
`"WinCDEmu"`, but WinCDEmu's driver is registered as **`BazisVirtualCDBus`** --
only its `DisplayName` reads "WinCDEmu Virtual Bus Driver". So the probe never
matched, and `HWPROFILE` reported `disc_mount:false` on the very box that had
just proved the Serious Sam disc check *using WinCDEmu*.

WHY IT IS NOT COSMETIC. This capability **suppresses shortcuts**: the gate
withholds every disc-needing launcher on a box it believes has no mounter. So
one wrong string meant

  * a machine that could run four disc titles silently received none of their
    shortcuts, and
  * any sweep that launched one directly recorded a **`failed`** cell against a
    title that was never given its disc -- a false failure that then tells the
    next agent not to bother.

Four such cells were found in the database, all from a sweep that bypassed the
gate by running `Play <Game>.bat` directly.

THE RULE: verify the SERVICE name on a box that has the product --

    reg query HKLM\\SYSTEM\\CurrentControlSet\\services /f "<product>" /d /s

-- rather than assuming it matches the product name.

Source-only; no agent build and no fleet needed.
"""
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "agent", "src", "hwprofile.c")


def _mount_svc_list():
    with open(SRC, encoding="utf-8", errors="replace") as f:
        src = f.read()
    m = re.search(r"mount_svc\[\]\s*=\s*\{(.*?)\}", src, re.S)
    assert m, "mount_svc[] is gone from hwprofile.c"
    return [s for s in re.findall(r'"([^"]+)"', m.group(1))]


def test_wincdemu_is_matched_by_its_real_driver_name():
    names = _mount_svc_list()
    assert "BazisVirtualCDBus" in names, (
        "WinCDEmu's driver is registered as BazisVirtualCDBus. Without it the "
        "probe reports disc_mount:false on a box that mounts images fine, the "
        "gate then SUPPRESSES every disc-needing shortcut there, and any sweep "
        "that launches one anyway records a false `failed`.")


def test_the_old_product_name_is_kept_too():
    """Removing it would be an unforced regression on any box that did use it."""
    assert "WinCDEmu" in _mount_svc_list(), (
        "the literal 'WinCDEmu' was dropped; keep it - older builds registered "
        "under that name and it costs one string to stay compatible")


def test_the_daemon_tools_names_survive():
    """The fleet's five DAEMON Tools boxes depend on these."""
    names = _mount_svc_list()
    for svc in ("d347bus", "d347prt", "sptd"):
        assert svc in names, "%s is gone; five fleet boxes detect via d347bus" % svc


def test_the_comment_records_how_to_verify_a_service_name():
    """The trap is general, so the fix has to teach the method, not just patch
    the one string."""
    with open(SRC, encoding="utf-8", errors="replace") as f:
        src = f.read()
    assert "reg query" in src and "services" in src, (
        "hwprofile.c no longer explains how to look up a mounter's real "
        "SERVICE name; the next person adds a product name and it silently "
        "never matches")
