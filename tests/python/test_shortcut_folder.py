"""ShortcutFolder: one folder on the desktop, not 150 loose icons (agent 1.99.0).

THE ASK (2026-11-08). "Have gamesync run for all the retro games to this
computer and place all the shortcuts into a directory on the desktop as opposed
to all of the shortcuts to the games themselves" - sent from ``.139``, the
Windows 11 copier host. A fleet box's desktop IS the game menu, so 150 loose
icons are correct there; somebody's daily-driver desktop is not ours to fill.

WHY THIS IS MORE THAN A PATH CHANGE, which is what this file pins. Three other
mechanisms in ``gs_run()`` are about the DESKTOP, and once our icons are not on
it they must not act on it:

* **the sweep** (``agent/shared/deskset.h``) MOVES every .lnk/.pif/.url a run
  did not put back into ``C:\\retro-desktop-backup``. On a machine whose desktop
  belongs to a person - exactly the kind that asks for a folder - that is
  vandalism, and it is silent: the files are in a folder, not deleted, so
  nobody notices until they look for a shortcut. So with a folder configured
  the run samples and sweeps THE FOLDER, and ``g_gs_desk_user`` is never set.
* **auto arrange** re-packs the desktop. Not ours to re-pack either.
* **the wallpapers** (26 MB into ``C:\\retro-wall``) are cosmetic and
  ``retrowall`` never applies them in the games lane, so staging them would
  only spend the disk the library needs.

The name validation itself is compiled for real by
``tests/native/test_shortcutdir.c``. What a unit test cannot see is WHERE in
``gs_run()`` each of these is decided - which is the part that would break
quietly - so that is what this file asserts.
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "agent" / "src"
GAMESYNC = SRC / "gamesync.c"
SHORTCUTDIR = REPO / "agent" / "shared" / "shortcutdir.h"


def _code() -> str:
    src = GAMESYNC.read_text(errors="replace")
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    return re.sub(r"//[^\n]*", " ", src)


def _body(code: str, signature: str) -> str:
    start = code.index(signature)
    while True:
        brace, semi = code.find("{", start), code.find(";", start)
        if brace != -1 and (semi == -1 or brace < semi):
            break
        start = code.index(signature, start + 1)
    depth = 0
    for j in range(brace, len(code)):
        if code[j] == "{":
            depth += 1
        elif code[j] == "}":
            depth -= 1
            if depth == 0:
                return code[brace:j + 1]
    raise AssertionError(signature)


def test_the_decision_module_is_the_one_that_is_compiled():
    """gamesync.c uses shortcutdir.h rather than its own copy of the rules."""
    code = _code()
    assert '#include "../shared/shortcutdir.h"' in GAMESYNC.read_text(errors="replace")
    assert "scd_name_ok" in code and "scd_join" in code


def test_every_shortcut_writer_asks_for_the_link_dir():
    """BOTH writers - the games and the operator's tools.

    "all the shortcuts" was the ask, and a tools-only exception would leave
    Retro Agent and Retro Chat icons on a Windows 11 desktop, which is the same
    complaint in miniature.
    """
    code = _code()
    for sig in ("static void gs_tool_shortcut(const char *exe",
                "static void gs_shortcut_from_line("):
        body = _body(code, sig)
        assert "gs_link_dir(" in body, f"{sig} does not use gs_link_dir()"
        assert "gs_desktop_dir(" not in body, (
            f"{sig} still builds its path from the desktop directly, so a "
            f"configured ShortcutFolder would not hold its shortcuts"
        )


def test_a_folder_scopes_the_sample_to_the_folder():
    """The sweep can only remove what the snapshot sampled, so scoping the
    snapshot is what makes touching either desktop impossible."""
    body = _body(_code(), "static void gs_desk_snapshot(void)")
    assert "gs_links_in_folder()" in body, (
        "gs_desk_snapshot() does not ask whether the shortcuts live in a "
        "folder, so it samples the user's desktop and the end-of-run sweep "
        "moves their own shortcuts into C:\\retro-desktop-backup"
    )
    # The folder branch must RETURN before the two desktop scans.
    folder_at = body.index("gs_links_in_folder()")
    user_at = body.index("gs_user_desktop_dir(")
    assert folder_at < user_at
    assert "return" in body[folder_at:user_at]


def test_the_desktop_layout_is_left_alone_when_the_icons_are_in_a_folder():
    body = _body(_code(), "static void gs_run(const char *library)")
    m = re.search(r"if \(gs_desk_changed\(\)[^)]*\)", body)
    assert m, "the icon-rebuild gate moved - re-check this assertion"
    assert "gs_links_in_folder()" in m.group(0), (
        "gs_run() auto-arranges the desktop even when every shortcut it wrote "
        "went into a folder"
    )


def test_the_wallpapers_are_not_staged_outside_full_management():
    body = _body(_code(), "static void gs_run(const char *library)")
    m = re.search(r"if \(host_manages_this_box\(\)\)\s*gs_stage_wallpapers\(", body)
    assert m, (
        "gs_run() stages 26 MB of fleet wallpapers unconditionally - on a "
        "games-lane box retrowall never applies them, so that is pure disk"
    )


def test_a_path_in_the_value_cannot_reach_the_sweep():
    """The sweep DELETES, and it follows gs_link_dir(). A value holding a path
    must be refused by the module gamesync.c actually calls - this is the
    source-side half of tests/native/test_shortcutdir.c."""
    h = SHORTCUTDIR.read_text(errors="replace")
    for ch in ("'\\\\'", "'/'", "':'"):
        assert ch in h, f"shortcutdir.h does not refuse {ch} in a folder name"
    body = _body(_code(), "static int gs_link_dir(char *out, DWORD cch)")
    assert "scd_join(" in body, "gs_link_dir() builds its path without scd_join()"


def test_a_refused_or_absent_value_falls_back_to_the_desktop():
    """...and loudly. A refusal that quietly meant "the desktop" is how 150
    icons reappear on the box that asked for a folder."""
    body = _body(_code(), "static int gs_shortcut_folder(char *out, DWORD cch)")
    assert "ShortcutFolder" in body and "REG_SZ" in body
    assert "log_msg" in body, "a refused ShortcutFolder value says nothing"
    link = _body(_code(), "static int gs_link_dir(char *out, DWORD cch)")
    assert link.count("gs_desktop_dir(") >= 1, "no desktop fallback"
