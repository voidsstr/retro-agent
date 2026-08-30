"""Desktop shortcuts must show the GAME's artwork, not a generic .bat icon.

WHY THIS EXISTS. `gs_make_shortcut()` set the icon from the shortcut's own
target. That is correct for an `.exe`, which carries its icons in its PE
resources — and gives nothing for a `.bat`, which has none, so Windows falls
back to the generic batch-file icon.

That became the common case rather than an edge one. Most staged titles moved to
a `Play <Game>.bat` launcher to mount a disc image, generate a per-box network
serial, force fullscreen or delete a stale `Running.ini` — so the fleet's
desktops filled with rows of identical gear icons and you could not tell the
games apart at a glance.

The fix resolves real artwork, in a deliberate order:
  1. an explicit THIRD tab-separated field in `launch.txt` — the library wins,
     because only it can know which artwork goes with which of a title's
     several launchers (Red Alert 2 ships both the game and Yuri's Revenge, and
     no heuristic can tell those apart)
  2. an `.ico` shipped in the title's own directory
  3. the first `.exe` the `.bat` itself names that exists on disk — the game's
     own executable, which is exactly the artwork wanted
  4. weakest, and last on purpose: any `.exe` in the directory, skipping
     `setup*`/`unins*`

Source: agent/src/gamesync.c — gs_resolve_icon(), gs_bat_names_exe(),
gs_shortcut_from_line(), gs_make_shortcut().
"""

import os
import re

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "agent", "src", "gamesync.c")


def _src():
    with open(SRC, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def _fn(text, signature):
    i = text.index(signature)
    j = text.index("\n}\n", i) + len("\n}\n")
    return text[i:j]


def test_make_shortcut_takes_an_icon_and_prefers_it():
    body = _fn(_src(), "static int gs_make_shortcut(")
    assert "const char *icon" in body, (
        "gs_make_shortcut() must accept an explicit icon; deriving it from the "
        "target gives a .bat the generic batch icon"
    )
    assert re.search(r"SetIconLocation\(sl,\s*\(icon\s*&&\s*icon\[0\]\)\s*\?\s*icon\s*:\s*target",
                     body), (
        "the explicit icon must win when present, falling back to the target "
        "only when nothing better was found"
    )


def test_an_exe_target_is_left_alone():
    """Resolving an icon for an .exe is pointless work — it has its own."""
    body = _fn(_src(), "static int gs_resolve_icon(")
    assert re.search(r'_stricmp\(ext,\s*"\.exe"\)\s*==\s*0', body), (
        "gs_resolve_icon() must short-circuit for .exe targets"
    )


def test_the_bat_exe_outranks_the_ico_sweep():
    """A shipped .ico is a WEAKER signal than the exe the launcher runs.

    Learned from the staged trees: Thief 2's only icon is `support.ico` — a
    HELP icon — and Tiberian Sun ships `NOTES.ICO` beside `SUN.ICO`. An
    unfiltered .ico rule confidently picks the wrong artwork, and a wrong icon
    is worse than a dull one because it actively misleads.
    """
    body = _fn(_src(), "static int gs_resolve_icon(")
    i_bat = body.index("gs_bat_names_exe")
    i_ico = body.index('"%s\\\\*.ico"')
    i_any = body.index('"%s\\\\*.exe"')
    assert i_bat < i_ico < i_any, (
        "order must be: the exe the .bat names, THEN a shipped .ico, then the "
        "weak any-exe guess"
    )


@pytest.mark.parametrize("junk", ["support", "notes", "readme", "help",
                                  "manual", "unins", "setup"])
def test_the_ico_sweep_skips_non_game_artwork(junk):
    body = _fn(_src(), "static int gs_resolve_icon(")
    assert '"%s"' % junk in body, (
        "the .ico sweep must skip %s*.ico — Thief 2 (support.ico) and Tiberian "
        "Sun (NOTES.ICO) both ship exactly this kind of decoy" % junk
    )


def test_the_weak_fallback_skips_setup_and_uninstallers():
    body = _fn(_src(), "static int gs_resolve_icon(")
    assert '"unins"' in body and '"setup"' in body, (
        "the any-exe fallback must skip uninstallers and setup programs — they "
        "sit beside the game and would otherwise win on name length"
    )


@pytest.mark.parametrize("tool", ["dosbox", "reg.exe", "taskkill.exe",
                                  "attrib.exe", "daemon.exe", "batchmnt"])
def test_the_bat_scanner_ignores_tooling(tool):
    """The emulator and the shell utilities are not the game.

    DOSBox matters as much as cmd.exe: every DOS title's launcher names it
    first, so without this System Shock 1's shortcut claimed to be DOSBox. The
    mount launchers likewise name daemon.exe/batchmnt, and the RA2 launchers
    name reg.exe for the per-box serial.
    """
    body = _fn(_src(), "static int gs_bat_names_exe(")
    assert '"%s"' % tool in body, (
        "the .bat scanner must skip %s, or it wins over the game's own exe" % tool
    )


def test_the_bat_scanner_ignores_the_shell_and_comments():
    body = _fn(_src(), "static int gs_bat_names_exe(")
    assert '"cmd.exe"' in body and '"start.exe"' in body, (
        "cmd.exe/start.exe are the shell, not the game — they appear in almost "
        "every staged launcher and would always win"
    )
    assert '"rem"' in body, (
        "a commented-out exe must not beat the real one; our staged launchers "
        "carry long rem blocks that name executables"
    )


def test_launch_txt_supports_an_explicit_icon_field():
    body = _fn(_src(), "static void gs_shortcut_from_line(")
    assert "icon_rel" in body, "launch.txt must accept a third icon field"
    assert body.index("icon_rel") < body.index("gs_resolve_icon"), (
        "the explicit field must be read before falling back to resolution"
    )
    assert "is not there - resolving" in body, (
        "a launch.txt naming a missing icon must fall back and SAY so, not "
        "silently produce a blank icon"
    )


def test_tool_shortcuts_still_compile_against_the_new_signature():
    """Retro Agent / Retro Chat target .exe files and need no icon."""
    src = _src()
    assert "gs_make_shortcut(exe, workdir, lnk, name, NULL)" in src, (
        "the tool-shortcut caller must pass the new argument"
    )


@pytest.mark.parametrize("fn", ["gs_resolve_icon", "gs_bat_names_exe"])
def test_helpers_are_bounded(fn):
    """These run on a fresh image against arbitrary staged trees."""
    body = _fn(_src(), "static int %s(" % fn)
    assert "lstrcpynA" in body or "_snprintf" in body, (
        "%s must use bounded copies" % fn
    )
    assert "strcpy(" not in body and "sprintf(" not in body, (
        "%s must not use unbounded string functions" % fn
    )

def test_the_bat_scanner_looks_one_level_down():
    """An Unreal-engine launcher `cd`s into System\\ before naming its exe.

    `Play Unreal Tournament.bat` does `cd /d "%~dp0System"` and then
    `start "" "UnrealTournament.exe"` — so the name is relative to the directory
    it changed to, not to the title root. Both Unreal titles came back with
    generic icons on two boxes for exactly this reason: the exe is real, it just
    is not where the scanner looked.

    One level is deliberate: it covers every staged layout (Unreal's System\\,
    the id-engine game dirs) without walking a 6 GB tree on a Pentium III.
    """
    src = _src()
    assert "gs_find_in_subdir" in src, (
        "the .bat scanner must look one level down; an exe named after a `cd` "
        "is not in the title root"
    )
    body = _fn(src, "static int gs_bat_names_exe(")
    assert "gs_find_in_subdir" in body, (
        "gs_bat_names_exe() must fall back to the subdirectory search"
    )
    # and it must be a FALLBACK - the root is still the first place to look
    assert body.index('"%s\\\\%s"') < body.index("gs_find_in_subdir"), (
        "the title root must be tried before descending"
    )


def test_the_subdir_search_is_bounded_to_one_level():
    body = _fn(_src(), "static int gs_find_in_subdir(")
    assert "FindFirstFileA" in body and body.count("FindFirstFileA") == 1, (
        "one level only — a recursive walk would cost minutes on a P3 over SMB1"
    )
    assert "FILE_ATTRIBUTE_DIRECTORY" in body, "must only descend into directories"
    assert "'.'" in body, "must skip . and .. or it recurses on itself"
