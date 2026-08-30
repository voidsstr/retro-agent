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
    """The two ways a naive counter is true on EVERY run and measures nothing.

    The second one silently defeated the whole gate in v1.73.0-1.74.x and is the
    reason this test is specific about the mechanism rather than just "something
    counts shortcuts". gs_run() begins with gs_sweep_desktop(), which moves
    EVERY .lnk off the desktop into a backup directory. So by the time a title's
    shortcut is written, nothing is ever "already there" - a
    `gs_file_exists(lnk)` check before the write is always false, every shortcut
    counts as new, and the gate is true on every box on every sync while
    reporting itself as working.

    The honest question is whether the SET of desktop icons changed, so the set
    must be sampled BEFORE the sweep and compared at the end.
    """
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

    # The icon set must be snapshotted BEFORE the sweep destroys it.
    run = code.split("static void gs_run(", 1)[1]
    snap = run.index("gs_desk_snapshot()")
    sweep = run.index("gs_sweep_desktop()")
    assert snap < sweep, (
        "gs_desk_snapshot() must run BEFORE gs_sweep_desktop(). The sweep moves "
        "every .lnk off the desktop, so a set sampled after it is empty and "
        "every recreated shortcut looks new - which is exactly how this gate "
        "was defeated silently before."
    )

    # The sweep itself must not count: it removes what we put straight back.
    swp = code.split("static void gs_sweep_desktop(void)", 1)[1].split("\nstatic ", 1)[0]
    assert "gs_desk_note_lnk" not in swp, (
        "gs_sweep_desktop() must not count its own removals as a change - it "
        "removes the very shortcuts this run is about to rewrite"
    )

    # A written shortcut is judged against the snapshot, not against the disk.
    assert "gs_desk_note_lnk_written(" in code, (
        "a written .lnk must be judged against the pre-run icon set"
    )
    assert "gs_file_exists(lnk)" not in code, (
        "judging a shortcut by whether the file exists just before writing it "
        "is always false after the sweep - use the pre-run snapshot"
    )

    # And the net difference must be resolved before the gate reads it.
    settle = run.index("gs_desk_settle_lnks()")
    gate = run.index("gs_desk_changed()")
    assert settle < gate, (
        "gs_desk_settle_lnks() must run before gs_desk_changed() is consulted"
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


def _enclosing_call(code, idx):
    """Return the full text of the call expression containing offset `idx`.

    Walks back to the opening paren of the enclosing call, then forward
    balancing parens while skipping string literals and their escapes, so a
    `(`, `)` or `;` inside a format string cannot end the extraction early.
    """
    # back up to the '(' that opens the enclosing call
    depth = 0
    i = idx
    while i > 0:
        ch = code[i]
        if ch == ")":
            depth += 1
        elif ch == "(":
            if depth == 0:
                break
            depth -= 1
        i -= 1
    start = i
    # forward, balancing, honouring string literals
    depth = 0
    j = start
    in_str = False
    while j < len(code):
        ch = code[j]
        if in_str:
            if ch == "\\":
                j += 2
                continue
            if ch == '"':
                in_str = False
        elif ch == '"':
            in_str = True
        elif ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
            if depth == 0:
                return code[start:j + 1]
        j += 1
    raise AssertionError("unbalanced call expression at offset %d" % idx)


def test_the_gate_reports_what_it_decided_on():
    """A silent gate is an untestable gate.

    gs_desk_changed() decides whether to rebuild the icon layout. If it is ever
    wrong in the "always true" direction the every-boot rebuild returns and
    NOTHING says so. The realistic cause is one file that re-copies on every
    pass - a destination whose mtime never stamps fails the size+mtime resume
    test forever - and from the outside that is indistinguishable from a box
    that genuinely had work to do.

    So the counts the gate decides on must be reported: in the `done:` log line
    an operator already reads, and in GAMESYNC STATUS so a caller can assert on
    them. A steady-state box must show zero.
    """
    code = _strip_comments(GAMESYNC.read_text(errors="replace"))

    assert "gs_desk_files()" in code and "gs_desk_lnks()" in code, (
        "the written-file and shortcut counts must be readable, not private to "
        "the gate"
    )

    # The done: line must carry both counts.
    #
    # Extract the call by BALANCING PARENTHESES, not by scanning to the first
    # `;`. Scanning to `;` works only while the format string happens to contain
    # no semicolon: add one (`"%d file error(s); "`) and the statement is
    # silently truncated at that point, so every assertion below the cut passes
    # against a line that is missing the fields they exist to require. The test
    # then goes green on a change that was never made - which is precisely the
    # failure mode this file exists to prevent, reproduced inside the test.
    idx = code.index("done: %d/%d title(s) copied")
    stmt = _enclosing_call(code, idx)
    assert "file(s) written" in stmt, (
        "the done: line must report how many files were actually written, or a "
        "gate that never suppresses anything is invisible"
    )
    assert "gs_desk_files()" in stmt and "gs_desk_lnks()" in stmt, (
        "the done: line must report the ACTUAL counters, not a recomputed guess"
    )

    # GAMESYNC STATUS must expose them too, so this is machine-checkable.
    assert '\\"files_written\\":%ld' in code, (
        "GAMESYNC STATUS must expose files_written so a caller can assert a "
        "steady-state sync really wrote nothing"
    )
    assert '\\"shortcuts_changed\\":%ld' in code, (
        "GAMESYNC STATUS must expose shortcuts_changed"
    )
