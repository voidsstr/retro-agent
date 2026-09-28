"""Source invariants: the desktop's shell bag is RESOLVED, and its VIEW is checked.

FOUND 2026-09-28 on ADMIN-PC (192.168.1.195, Windows 7): 8 of 117 desktop icons
visible, in one row, while ICONARRANGE answered
``"autoarrange":true,"icons":119,"fflags":545`` the whole time. The desktop was
in List view - ``HKCU\\...\\Shell\\Bags\\4\\Desktop`` held Mode=3,
LogicalViewMode=4, IconSize=16 - and BagMRU's NodeSlot was 4.

Two defects in agent/src/gamesync.c, each invisible to the other's report:

* ``GS_DESKTOP_BAG`` was a hardcoded ``Shell\\Bags\\1\\Desktop``. The desktop's
  bag is whichever slot ``Shell\\BagMRU``'s NodeSlot names - 1 on the XP boxes
  measured (.124, .110), 4 on .195 - so every FFlags write on .195 went into a
  key nothing reads, and ICONARRANGE reported that key back as the truth.
* ``gs_autoarrange_cmd()`` posted 0x7051 on every NT box. That is "Auto
  Arrange" on XP and **"List"** on Windows 7 (read from .195's
  shell32.dll.mui), so the agent itself switched the desktop to List view on
  every boot that found auto-arrange clear, then logged "shell toggle did not
  take" and reported success.

The arithmetic (slot choice, FFlags, the command table, the view verdicts) is
pinned by ``tests/native/test_desktop_bag_view.c`` against the measured values.
What that cannot see is the SHAPE of the C: a path literal creeping back, a
write that is not read back, the view repair running after the layout, or the
report dropping the fields that would have shown the fault. Each test here
also runs its detector against the OLD source shape, so a detector that can
only ever pass is itself caught.
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
GAMESYNC = REPO / "agent" / "src" / "gamesync.c"
DESKVIEW = REPO / "agent" / "shared" / "deskview.h"

# The line this fix removed, verbatim - the detectors below must flag it.
OLD_BAG_DEFINE = ('#define GS_DESKTOP_BAG   '
                  '"Software\\\\Microsoft\\\\Windows\\\\Shell\\\\Bags\\\\1\\\\Desktop"')
OLD_ICONARRANGE_READ = ('if (RegOpenKeyExA(HKEY_CURRENT_USER, GS_DESKTOP_BAG, 0, '
                        'KEY_QUERY_VALUE,')


def _strip_comments(src: str) -> str:
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    return src


def _code():
    return _strip_comments(GAMESYNC.read_text(errors="replace"))


def _fn(code: str, signature: str) -> str:
    """Body of the function whose definition starts with `signature`."""
    i = code.index(signature)
    j = code.index("{", i)
    depth = 0
    for k in range(j, len(code)):
        if code[k] == "{":
            depth += 1
        elif code[k] == "}":
            depth -= 1
            if depth == 0:
                return code[j:k + 1]
    raise AssertionError("unbalanced body for %s" % signature)


# A string literal naming one FIXED numbered bag, e.g. "...Bags\\1\\Desktop".
_FIXED_BAG_LITERAL = re.compile(r'"[^"\n]*Bags\\\\\d+\\\\Desktop[^"\n]*"')


def _fixed_bag_literals(code: str):
    return _FIXED_BAG_LITERAL.findall(code)


def test_the_detector_flags_the_old_hardcoded_bag():
    """Guard the guard: the old define must be caught by the same regex."""
    assert _fixed_bag_literals(OLD_BAG_DEFINE), (
        "the fixed-bag detector no longer matches the line this fix removed - "
        "it would pass a regression"
    )
    assert "GS_DESKTOP_BAG" in OLD_BAG_DEFINE


def test_no_hardcoded_desktop_bag_path():
    """No 'Bags\\<n>\\Desktop' literal, and no GS_DESKTOP_BAG, in the code."""
    code = _code()
    hits = _fixed_bag_literals(code)
    assert not hits, (
        "gamesync.c names a fixed shell bag %r. The desktop's bag is the slot "
        "Shell\\BagMRU's NodeSlot names - 4 on .195 - and a fixed Bags\\1 wrote "
        "to a key nothing reads while ICONARRANGE reported it as the truth."
        % hits
    )
    assert "GS_DESKTOP_BAG" not in code, (
        "GS_DESKTOP_BAG is back - resolve the bag with gs_desktop_bag()"
    )


def test_the_bag_is_resolved_from_bagmru_nodeslot():
    code = _code()
    body = _fn(code, "static void gs_desktop_bag(gs_bag_t *b)")
    assert "DV_BAGMRU_KEY" in body and "DV_BAGMRU_VALUE" in body, (
        "gs_desktop_bag() must read Shell\\BagMRU's NodeSlot"
    )
    assert "dv_bag_slot(" in body and "dv_bag_path(" in body, (
        "gs_desktop_bag() must use deskview.h's slot choice and path builder, "
        "which the native test pins to the measured values"
    )
    hdr = DESKVIEW.read_text(errors="replace")
    assert re.search(r'#define\s+DV_BAGMRU_KEY\s+'
                     r'"Software\\\\Microsoft\\\\Windows\\\\Shell\\\\BagMRU"', hdr), (
        "the desktop's NodeSlot is on Shell\\BagMRU on XP and Win7 alike - XP's "
        "ShellNoRoam\\BagMRU root carries no NodeSlot (measured on .124 and .110)"
    )
    assert re.search(r'#define\s+DV_BAGMRU_VALUE\s+"NodeSlot"', hdr)


def test_every_bag_user_resolves_the_slot():
    """The writer, the view repair and the report must all use the same bag."""
    code = _code()
    for sig in ("static void gs_bag_autoarrange(int on)",
                "static int gs_desktop_view_fix(HWND defview, HWND lv, gs_view_t *out)",
                "static void gs_desktop_view_read(HWND lv, gs_view_t *r)"):
        assert "gs_desktop_bag(" in _fn(code, sig), (
            "%s must resolve the bag slot itself" % sig)
    report = _fn(code, "void handle_iconarrange(SOCKET sock, const char *args)")
    assert "view.bag.path" in report, (
        "ICONARRANGE must read FFlags from the RESOLVED bag - on .195 it reported "
        "545 from a Bags\\1 nothing reads"
    )
    assert OLD_ICONARRANGE_READ not in report
    assert OLD_ICONARRANGE_READ.replace("GS_DESKTOP_BAG", "view.bag.path") in report


def test_every_bag_write_is_read_back():
    """RegSetValueExA's return is not evidence - the value must be re-read."""
    code = _code()
    helper = _fn(code, "static int gs_reg_set_dword_verified(")
    assert "RegSetValueExA" in helper and "gs_reg_dword(" in helper, (
        "the verified writer must re-read what it wrote"
    )
    for sig in ("static void gs_bag_autoarrange(int on)",
                "static int gs_desktop_view_fix(HWND defview, HWND lv, gs_view_t *out)"):
        body = _fn(code, sig)
        assert "RegSetValueExA" not in body, (
            "%s writes the bag without reading it back - use "
            "gs_reg_set_dword_verified()" % sig)
        assert "gs_reg_set_dword_verified(" in body, sig
    view = _fn(code, "static int gs_desktop_view_fix(HWND defview, HWND lv, gs_view_t *out)")
    for name in ('"Mode"', '"LogicalViewMode"', '"IconSize"'):
        assert name in view, "the persisted view repair must cover %s" % name


def test_the_view_repair_is_a_conditional_set():
    """A settled desktop must see no post and no write."""
    code = _code()
    body = _fn(code, "static int gs_desktop_view_fix(HWND defview, HWND lv, gs_view_t *out)")
    first_check = body.index("if (!dv_live_is_icon_view(")
    post = body.index("PostMessageA(defview, WM_COMMAND, cmd, 0)")
    assert first_check < post, (
        "the shell's icon-view command must be posted only when the live view "
        "is NOT already icon view"
    )
    assert "dv_iconview_cmd(" in body and "if (defview && cmd)" in body, (
        "only a MEASURED icon-view command id may be posted (0 = none known)"
    )
    assert "dv_bag_view_repair(" in body, (
        "the persisted values must be decided by dv_bag_view_repair(), which "
        "returns 0 - write nothing - for a bag already in icon view"
    )
    assert "LVM_SETVIEW_" in body and "DV_LVS_TYPEMASK" in body, (
        "when the shell command does not take, set the view directly"
    )
    # The repair re-reads the live state after acting, rather than trusting it.
    assert body.count("GetWindowLongA(lv, GWL_STYLE)") >= 3


def test_the_view_is_repaired_before_the_layout():
    """Auto-arrange means nothing in List view; the view must come first."""
    code = _code()
    for sig in ("void gs_desktop_icons_apply_ex(int force)",
                "void handle_iconarrange(SOCKET sock, const char *args)"):
        body = _fn(code, sig)
        v = body.index("gs_desktop_view_fix(")
        for later in ("gs_apply_autoarrange(", "gs_arrange_bay(",
                      "gs_desktop_icons_apply_ex("):
            if later in body:
                assert v < body.index(later), (
                    "%s: gs_desktop_view_fix() must run before %s" % (sig, later))


def test_a_zero_command_id_is_never_posted():
    """An unmeasured Windows gets id 0 - and must get no WM_COMMAND at all."""
    code = _code()
    lines = code.splitlines()
    for i, ln in enumerate(lines):
        if "PostMessageA" in ln and "gs_autoarrange_cmd()" in ln:
            window = "\n".join(lines[max(0, i - 3):i])
            assert re.search(r"if \(defview && gs_autoarrange_cmd\(\)\)", window), (
                "gamesync.c line %d posts gs_autoarrange_cmd() without checking it "
                "is non-zero - an unmeasured Windows would get WM_COMMAND 0, and a "
                "guessed id is what put .195 into List view" % (i + 1))


def test_iconarrange_reports_where_and_what_view():
    """The fields that would have shown the .195 fault must be in the reply."""
    body = _fn(_code(), "void handle_iconarrange(SOCKET sock, const char *args)")
    for key in ("bag", "bag_slot", "bag_slot_source", "bag_exists", "view",
                "lv_style_type", "lv_view", "bag_mode", "bag_logical_view_mode",
                "bag_icon_size", "view_repaired", "bag_view_repaired",
                "view_still_wrong",
                # the pre-existing fields stay, for existing callers
                "autoarrange", "fflags", "fflags_autoarrange", "icons", "screen"):
        assert '\\"%s\\":' % key in body, "ICONARRANGE must report %r" % key
    assert "gs_json_escape(view.bag.path" in body, (
        "the bag path is full of backslashes - it must be JSON-escaped"
    )
    # Post-condition, not intention: the report re-reads after the pass.
    assert body.index("gs_desktop_view_read(") > body.index("gs_desktop_view_fix(")
