"""The library listing must be COMPLETE, and must say so when it is not.

GAMESYNC decides what a machine gets by walking the staged library with
FindFirstFile/FindNextFile.  Until 2026-08-31 it called ``gs_dir_size()`` -- a
full recursive walk of a whole title tree, over the same SMB connection -- from
*inside* that loop, so the outer search handle stayed open for minutes of
further redirector traffic per title.

On Win9x that search context does not survive it.  Measured on ``.243``
(Win98SE, Pentium 1, agent 1.78.1): a **46**-title library enumerated as **25**
titles.  ``FindNextFileA`` returned FALSE partway down the alphabet, the loop
ended, and the run finished ``state=done`` with ``titles_total: 25`` -- no error
in the log, nothing in the status, and 21 titles never considered at all.  One
of them (``ShadowWarrior``) is a title the capability gate approves for that
box, so the user's report of "no games on the desktop" was partly this.

This is the project's signature failure shape: the tool reported success and was
believed.  Two invariants keep it fixed, and both are shape, not arithmetic --
no unit test can see them because the calls are Win32 side effects:

  1. **Size the trees only after the directory handle is closed.**  The handle
     now lives for one directory listing instead of the whole sizing pass.
  2. **A truncated listing must be logged.**  Both ways the loop can end early
     (a FindNextFile error, and the GS_MAX_TITLES cap) were silent.

Also pinned here: a gate refusal whose limiting factor is ``disk`` counts as
``titles_skipped``, never ``titles_gated``.  CLAUDE.md is explicit that these
are different facts with different follow-ups -- "did not fit" versus "this
machine cannot run it" -- and rolling them together told an operator that a
Pentium 1 *cannot run Warcraft II*.  On .243, thirteen of the twenty-two
"gated" titles were really just too big for a 604 MB volume.
"""

import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
GAMESYNC = REPO / "agent" / "src" / "gamesync.c"


def _strip_comments(src: str) -> str:
    """Drop /* ... */ and // ... so prose about the bug is not mistaken for it."""
    src = re.sub(r"/\*.*?\*/", " ", src, flags=re.S)
    src = re.sub(r"//[^\n]*", " ", src)
    return src


def _gs_run(code: str) -> str:
    start = code.index("static void gs_run(const char *library)")
    return code[start:]


def _enum_block(code: str) -> str:
    """The text between opening the library search and closing it."""
    body = _gs_run(code)
    open_at = body.index("FindFirstFileA(pat")
    close_at = body.index("FindClose(h)", open_at)
    return body[open_at:close_at]


def test_sizing_does_not_happen_inside_the_find_loop():
    """gs_dir_size() must not run while the library search handle is open."""
    code = _strip_comments(GAMESYNC.read_text(errors="replace"))
    block = _enum_block(code)
    assert "gs_dir_size" not in block, (
        "gs_dir_size() is being called while the library FindFirstFile handle "
        "is still open. On Win9x that invalidates the search context and "
        "FindNextFileA silently truncates the library (46 titles seen as 25 on "
        ".243). Collect the names first, FindClose, THEN size them."
    )


def test_the_names_are_collected_then_sized_afterwards():
    """The sizing pass must exist, after FindClose, over the collected names."""
    code = _strip_comments(GAMESYNC.read_text(errors="replace"))
    body = _gs_run(code)
    close_at = body.index("FindClose(h)")
    after = body[close_at:body.index("_priority.txt", close_at)]
    assert "gs_dir_size" in after, \
        "the per-title sizing pass has gone missing after FindClose"
    assert "sizes[i]" in after and "grand" in after, \
        "the sizing pass must still fill sizes[] and the grand total"


def test_an_early_end_to_the_walk_is_logged():
    """A FindNextFile failure must be reported, not swallowed."""
    code = _strip_comments(GAMESYNC.read_text(errors="replace"))
    body = _gs_run(code)
    assert "ERROR_NO_MORE_FILES" in body, (
        "nothing distinguishes a normal end-of-directory from a redirector "
        "dropping the search - so a truncated library looks healthy"
    )
    assert "STOPPED EARLY" in body, \
        "the truncation must be logged in words an operator will notice"


def test_the_title_cap_is_named_and_logged():
    """Overflowing the titles[] array must not be silent either."""
    src = GAMESYNC.read_text(errors="replace")
    code = _strip_comments(src)
    assert "#define GS_MAX_TITLES" in src, \
        "the cap must be a named constant, not a bare 64 in three places"
    body = _gs_run(code)
    assert "titles[GS_MAX_TITLES][128]" in body and \
           "sizes[GS_MAX_TITLES]" in body, \
        "both arrays must be sized by the same constant"
    assert re.search(r"n\s*>=\s*GS_MAX_TITLES", body), \
        "the loop guard must use the constant"
    assert "CAPPED" in body, \
        "hitting the cap silently drops titles - it must be logged"


def test_a_bigger_published_verdict_file_flags_a_truncated_listing():
    """The verdict file is whole-library, so more rows than titles = truncation."""
    code = _strip_comments(GAMESYNC.read_text(errors="replace"))
    body = _gs_run(code)
    assert re.search(r"g_gate_verdict_n\s*>\s*n", body), (
        "the published file covering MORE titles than were enumerated is a "
        "direct, free detector of a truncated listing - on .243 it would have "
        "read 'covers 46 of 25'"
    )
    assert re.search(r"g_gate_verdict_n\s*<\s*n", body), \
        "the pre-existing partial-publish warning must survive"


def test_a_disk_refusal_defers_to_the_real_room_check():
    """`disk` is not a capability verdict, and it is not final.

    The gate compares the title's DECLARED ``disk_mb`` against a ``free_mb``
    sampled before the run and gives no credit for a copy already installed.
    GAMESYNC's own room check twenty lines below asks the same question with
    the tree's real size, the current free space, and the space an existing
    install is about to give back -- so a disk verdict must fall through to it
    and be counted as ``skipped_titles``, never ``gated_titles``.
    """
    code = _strip_comments(GAMESYNC.read_text(errors="replace"))
    assert "gs_gate_limited_by_disk" in code, \
        "the disk-versus-capability distinction must be made explicitly"

    body = _gs_run(code)
    at = body.index("gs_gate_allows_title(library")
    block = body[at:at + 900]
    assert "gs_gate_limited_by_disk(why)" in block, (
        "the gate call site must let a disk verdict through to the room check; "
        "on .240 a FarCry that was installed AND verified read deploy=gated, "
        "and on .243 the operator was told a Pentium 1 cannot RUN Warcraft II"
    )
    assert "gated_titles++" in block, \
        "a genuine capability refusal must still count as gated"
    assert "skipped_titles++" not in block, \
        "the gate must not decide disk; the room check owns that counter"

    # ...and the room check, which does own it, is what bumps the counter.
    room_at = body.index("GS_FREE_MARGIN", at)
    assert "skipped_titles++" in body[room_at:room_at + 900], \
        "the real room check must be what increments skipped_titles"


def test_a_disk_refusal_does_not_suppress_a_SHORTCUT_either():
    """The disk defer had TWO call sites and only one of them got the fix.

    ``gs_gate_allows_title`` learned to let a ``disk`` verdict fall through to
    the real room check.  ``gs_gate_allows_shortcut`` did not, and it runs
    AFTER the tree is on the volume and after ``gs_file_exists(target)`` has
    confirmed the launcher itself is there -- so a ``disk`` refusal there took
    the desktop icon off a game that was installed and working.

    Measured on .240 on 2026-08-31, on a Far Cry that the compat matrix
    records as ``runs=verified``::

        FarCry: SHORTCUT SUPPRESSED "Far Cry" (Play Far Cry.bat)
                - disk: not enough free disk (have 1492 MB, needs 3700)

    and ``titles_gated`` read 0, so the summary line said nothing was gated at
    all.  ``disk_mb`` answers "is this copy worth the bandwidth", never "can
    this machine run it", and it must not decide a shortcut.
    """
    code = _strip_comments(GAMESYNC.read_text(errors="replace"))
    at = code.index("static int gs_gate_allows_shortcut(")
    body = code[at:code.index("static void gs_shortcut_from_line(", at)]
    assert "gs_gate_limited_by_disk" in body, (
        "the per-shortcut gate must exempt a disk-limited verdict, the same "
        "way the per-title gate does"
    )
    assert re.search(r"GG_V_NO\s*&&\s*!gs_gate_limited_by_disk", body), (
        "the exemption belongs on the GG_V_NO branch: a genuine hardware "
        "refusal must still suppress the shortcut"
    )
    # A missing CAPABILITY is a different fact and must still suppress.
    assert "missing_caps" in body,         "a missing capability must still suppress the shortcut"


def test_the_room_check_still_credits_an_installed_tree():
    """The credit block is what makes deferring to the room check correct."""
    code = _strip_comments(GAMESYNC.read_text(errors="replace"))
    body = _gs_run(code)
    at = body.index("gs_gate_allows_title(library")
    room = body[at:body.index("gs_copy_tree", at)]
    assert "gs_dir_size(have" in room and "freeb += existing" in room, (
        "an already-installed title is being UPDATED, not added: without this "
        "credit a large title can never be patched once the disk fills, which "
        "is the UnrealTournament-436 incident"
    )


def test_an_installed_title_gets_its_icons_back_even_when_not_copied():
    """The sweep takes every icon off the desktop; only the copy branch puts any back.

    So a title that is INSTALLED and playable but gated or skipped on this run
    loses its shortcuts permanently.  Measured on `.243` 2026-08-31: the engine
    index found ``c:\\games\\HexenII`` on the box at 14:25 and an hour later the
    desktop carried Quake and nothing else -- the games were there, the icons
    were in ``C:\\retro-desktop-backup``.  That is the whole of the user's report.

    Restoring them is safe for a gated title too: ``gs_shortcut_from_line()``
    re-asks the gate per shortcut and ``gg_req_parse_shortcut()`` overlays the
    shortcut's requirements on the title's, so a title-level hard NO still
    suppresses every icon.
    """
    code = _strip_comments(GAMESYNC.read_text(errors="replace"))
    assert "gs_restore_shortcuts_if_installed" in code, (
        "a title that is installed but not copied this run must still get its "
        "desktop shortcuts back - the sweep removed them"
    )

    # the helper must actually build the shortcuts, so the PER-SHORTCUT gate
    # runs again and suppresses what genuinely cannot run.
    at = code.index("static void gs_restore_shortcuts_if_installed")
    helper = code[at:at + 700]
    assert "gs_file_exists" in helper, \
        "it must only restore icons for a tree that is really on the disk"
    assert "gs_make_game_shortcut" in helper, (
        "it must go through gs_make_game_shortcut so gs_gate_allows_shortcut() "
        "re-applies per shortcut"
    )

    body = _gs_run(code)
    calls = body.count("gs_restore_shortcuts_if_installed(titles[i])")
    assert calls == 2, (
        "both ways a title can be passed over must restore its icons - the "
        "capability gate and the disk room check - found %d call(s)" % calls
    )
    assert "gs_make_game_shortcut(dst, titles[i])" in body, \
        "the copy branch must still make shortcuts the normal way"
