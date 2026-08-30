"""Source invariants for the desktop Auto Arrange fleet default (agent v1.73.0).

The arithmetic is covered by tests/native/test_icon_autoarrange.c. What that
test CANNOT see is the shape of the C source itself, and every way this feature
has broken has been a shape problem rather than a maths problem:

  * a **blind** ``PostMessage(..., FCIDM_SHVIEW_AUTOARRANGE, ...)``. It is a
    TOGGLE. Fired without first reading LVS_AUTOARRANGE it turns the setting
    OFF on a box that already had it ON - and logs that it turned it on. That
    is this project's recurring "reported success and was believed" failure
    shape, and it is invisible to a unit test because the message is a
    cross-process side effect.

  * the apply call sited **below an early return**. retrowall_apply_startup()
    returns early on the two paths that are NORMAL for a fleet box (a fleet
    wallpaper was applied; no rotation is staged). A call placed after them
    runs on almost no machine, logs nothing, and looks installed. The theme and
    the screensaver were already caught by exactly this once.

  * the switch **defaulting to off**. The user asked for auto-arrange
    everywhere; a box that has never heard of HKLM\\Software\\RetroAgent\\
    IconAutoArrange must get auto-arrange, not the legacy bay.

  * the legacy arrangers being **re-staged**. scripts/retro-wallpaper/
    arrange_icons.c clears LVS_AUTOARRANGE by design, so one run of it undoes
    the fleet setting. deploy_rotation.py must remove it, never upload it.
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
GAMESYNC = REPO / "agent" / "src" / "gamesync.c"
RETROWALL = REPO / "agent" / "src" / "retrowall.c"
DEPLOY = REPO / "scripts" / "retro-wallpaper" / "deploy_rotation.py"


def _strip_comments(src: str) -> str:
    """Drop /* ... */ and // ... so prose about the bug is not mistaken for it."""
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    return src


def test_autoarrange_toggle_is_never_posted_blindly():
    """Every FCIDM_SHVIEW_AUTOARRANGE post must be guarded by a style read.

    The guard differs by direction and both are legal:
      * turning it ON  - post only when the bit is CLEAR  (gs_apply_autoarrange)
      * turning it OFF - post only when the bit is SET    (gs_arrange_bay)
    What is never legal is a post that is not inside an ``if`` testing
    LVS_AUTOARRANGE.
    """
    code = _strip_comments(GAMESYNC.read_text(errors="replace"))
    lines = code.splitlines()
    posts = [i for i, ln in enumerate(lines) if "FCIDM_SHVIEW_AUTOARRANGE" in ln
             and ("PostMessage" in ln or "SendMessage" in ln)]
    assert posts, "expected the shell toggle to still be used (guarded)"

    for i in posts:
        # Look back a short window for the guarding read of the style bit.
        window = "\n".join(lines[max(0, i - 12):i])
        assert "LVS_AUTOARRANGE" in window and "GetWindowLong" in window, (
            "gamesync.c line %d posts FCIDM_SHVIEW_AUTOARRANGE without first "
            "reading LVS_AUTOARRANGE. It is a TOGGLE: an unguarded post flips "
            "the setting the wrong way on any box already in the target state, "
            "while logging success." % (i + 1)
        )


def test_the_style_fallback_sets_the_bit_it_must_not_xor():
    """gs_apply_autoarrange must OR the bit in; an XOR is the toggle bug again."""
    code = _strip_comments(GAMESYNC.read_text(errors="replace"))
    fn = code.split("gs_apply_autoarrange", 1)[1].split("\nstatic ", 1)[0]
    assert "style | LVS_AUTOARRANGE" in fn, (
        "gs_apply_autoarrange() must SET the style bit (style | LVS_AUTOARRANGE)"
    )
    assert "^ LVS_AUTOARRANGE" not in fn, (
        "gs_apply_autoarrange() must not XOR the style bit - it runs on every "
        "agent startup, so an XOR would turn auto-arrange back off on boot 2"
    )
    # It must read the bit back rather than trusting SetWindowLong.
    assert fn.count("GetWindowLongA") >= 2, (
        "gs_apply_autoarrange() must re-read GWL_STYLE after writing it - "
        "the shell toggle silently failed on .143 and .171"
    )


def test_autoarrange_is_applied_above_retrowall_early_returns():
    """The apply call must not sit behind retrowall_apply_startup's returns."""
    code = _strip_comments(RETROWALL.read_text(errors="replace"))
    start = code.index("retrowall_apply_startup")
    body = code[start:]
    call = body.index("gs_desktop_icons_apply()")
    before = body[:call]
    assert "return" not in before, (
        "gs_desktop_icons_apply() is placed after an early return in "
        "retrowall_apply_startup(). Both of those returns are the NORMAL path "
        "on a fleet box, so the call would run on almost no machine while "
        "looking installed."
    )


def test_the_icon_layout_switch_defaults_to_autoarrange():
    """Key absent must mean auto-arrange - that is what the user asked for."""
    code = _strip_comments(GAMESYNC.read_text(errors="replace"))
    fn = code.split("gs_want_autoarrange", 1)[1].split("\nstatic ", 1)[0]
    assert re.search(r"DWORD\s+val\s*=\s*1\b", fn), (
        "gs_want_autoarrange() must initialise its value to 1 so a box with no "
        "IconAutoArrange value gets auto-arrange"
    )
    # A failed read must fall back to 1, not leave a garbage/zero value.
    assert re.search(r"!=\s*ERROR_SUCCESS[^;]*\|\|[^;]*\)\s*\n?\s*val\s*=\s*1",
                     fn) or "val = 1;" in fn, (
        "a failed registry read must fall back to auto-arrange, not to the bay"
    )
    assert "return val != 0;" in fn, (
        "any non-zero IconAutoArrange means auto-arrange"
    )


def test_deploy_rotation_never_stages_the_legacy_arranger():
    """arrange_icons.exe clears LVS_AUTOARRANGE - it must only ever be removed."""
    src = DEPLOY.read_text(errors="replace")
    for line in src.splitlines():
        code = line.split("#", 1)[0]
        if "upload_file" in code and "arrange_icons" in code:
            raise AssertionError(
                "deploy_rotation.py uploads arrange_icons.exe: %s\n"
                "That tool explicitly clears LVS_AUTOARRANGE, so a single run "
                "of it turns the fleet-wide auto-arrange setting back off."
                % line.strip()
            )
    assert "arrange_icons.exe" in src, (
        "deploy_rotation.py should still REMOVE any stale arrange_icons.exe"
    )


def test_legacy_arrangers_carry_a_superseded_banner():
    """Source-only tools that fight the fleet default must say so up front."""
    wall = REPO / "scripts" / "retro-wallpaper"
    for name in ("arrange_icons.c", "arrange_icons_ll.c"):
        head = (wall / name).read_text(errors="replace")[:2000]
        assert "SUPERSEDED" in head, (
            "%s clears LVS_AUTOARRANGE and would undo the fleet default; its "
            "header must say so, or someone will helpfully redeploy it" % name
        )


def test_gamesync_arranges_only_when_the_desktop_changed():
    """The unconditional arrange at the end of gs_run() must not come back.

    GAMESYNC runs at startup, so an unconditional call rebuilt the icon layout
    on every boot of every machine - reported by the user as "the retro agent
    is rebuilding icons all the time". The call must be guarded by
    gs_desk_changed().
    """
    code = _strip_comments(GAMESYNC.read_text(errors="replace"))
    lines = code.splitlines()
    calls = [i for i, ln in enumerate(lines)
             if re.search(r"\bgs_desktop_icons_apply\s*\(\s*\)\s*;", ln)]
    assert calls, "gs_run() must still arrange when something changed"
    for i in calls:
        window = "\n".join(lines[max(0, i - 8):i])
        assert "gs_desk_changed()" in window, (
            "gamesync.c line %d calls gs_desktop_icons_apply() without a "
            "gs_desk_changed() guard. GAMESYNC runs at startup, so an "
            "unguarded call rebuilds the icon layout on every boot." % (i + 1)
        )


def test_the_change_counters_are_reset_per_run():
    """Without a reset, one deploy makes every later sync look like a change."""
    code = _strip_comments(GAMESYNC.read_text(errors="replace"))
    assert "gs_desk_reset()" in code, (
        "gs_run() must reset the change counters at the top, or the gate leaks "
        "across syncs and the every-boot rebuild comes straight back"
    )


def test_a_skipped_file_and_a_rewritten_lnk_are_not_counted_as_changes():
    """The two ways a naive counter would be true on every run."""
    code = _strip_comments(GAMESYNC.read_text(errors="replace"))

    # The file counter must sit on the real-write path, i.e. AFTER the resume
    # early-out, which is the `return 1` inside gs_copy_file's skip test.
    copy_fn = code.split("static int gs_copy_file", 1)[1].split("\nstatic ", 1)[0]
    early = copy_fn.index("return 1;")
    assert "gs_desk_note_file()" not in copy_fn[:early], (
        "a file skipped by the size+mtime resume test must NOT count as a "
        "change - it is the normal case on a provisioned box"
    )
    assert "gs_desk_note_file()" in copy_fn[early:], (
        "gs_copy_file() must count a real write"
    )

    # The .lnk counter must be conditional on the link not already existing.
    sc = code.split("gs_shortcut_from_line", 1)[1].split("\nstatic ", 1)[0]
    assert "gs_file_exists(lnk)" in sc, (
        "gs_shortcut_from_line() must check whether the .lnk was already there "
        "- it rewrites the link on every pass, so counting writes measures "
        "nothing"
    )
    assert re.search(r"if\s*\(\s*!\s*was_there\s*\)", sc), (
        "only a .lnk that was NOT already on the desktop is a change"
    )


def test_iconarrange_always_forces_a_full_pass():
    """A manual request is a deliberate act and the gate must not refuse it."""
    code = _strip_comments(GAMESYNC.read_text(errors="replace"))
    fn = code.split("void handle_iconarrange", 1)[1].split("\nvoid ", 1)[0]
    assert "gs_desktop_icons_apply_ex(1)" in fn, (
        "ICONARRANGE must force a full pass - the user named 'fixing issues' "
        "as a legitimate reason to re-arrange a desktop the agent thinks is fine"
    )
    assert re.search(r"gs_apply_autoarrange\(defview,\s*lv,\s*1\)", fn), (
        "the explicit 'auto' mode must force too"
    )


def test_an_already_on_desktop_is_not_re_packed_every_startup():
    """LVM_ARRANGE on an already-arranged desktop is churn, not maintenance."""
    code = _strip_comments(GAMESYNC.read_text(errors="replace"))
    fn = code.split("gs_apply_autoarrange", 1)[1].split("\nstatic ", 1)[0]
    assert "LVM_ARRANGE_" in fn, "the re-pack must still exist for a real change"
    m = re.search(r"else if \(changed \|\| force\)", fn)
    assert m, (
        "LVM_ARRANGE must be sent only when the setting was just changed or the "
        "pass was forced. Sending it unconditionally re-packs the desktop on "
        "every agent startup, which is exactly the churn the user reported - "
        "and it achieves nothing, because with auto-arrange on the shell is "
        "already keeping the desktop packed by itself."
    )
