"""The 3dfx Control Panel's desktop shortcut survives GAMESYNC (agent 1.93.3).

push_3dfxctl.py puts a "3dfx Control Panel" shortcut on the All Users desktop,
and every GAMESYNC ends by sweeping each desktop .lnk it did not write or claim
itself - so on .124 the shortcut was gone after two quiet syncs (2026-09-29).
The agent now places and claims it exactly like "Retro Agent"/"Retro Chat",
through gs_tool_shortcut(), which does nothing when the exe is absent - so a box
the panel was never pushed to gets no icon.
"""
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
GS = (REPO / "agent" / "src" / "gamesync.c").read_text(encoding="latin-1")
PUSH = (REPO / "scripts" / "3dfx" / "3dfxctl" / "push_3dfxctl.py").read_text()


def _body(name):
    i = GS.index("\nvoid " + name + "(")
    return GS[i:GS.index("\n}\n", i)]


def test_the_agent_places_and_claims_the_panel_shortcut():
    body = _body("gs_place_tool_shortcuts")
    # agent 1.98.1: the path is no longer a literal. It is resolved beside the
    # running agent first and falls back to C:\RETRO_AGENT (toolpath.h), because
    # .186 runs the stack from D:\RETRO_AGENT and the literal gave that box no
    # Retro Chat icon at all. The icon must come from the SAME directory as the
    # exe that won, or the pair could be split across two drives.
    assert re.search(r'gs_tool_shortcut\(gs_tool_leaf\("3dfxctl\.exe", fxexe, sizeof\(fxexe\)\),\s*'
                     r'"3dfx Control Panel",\s*'
                     r'gs_tool_sibling\(fxexe, "3dfxlogo\.ico", fxico, sizeof\(fxico\)\)\);', body)


def test_the_panel_path_is_not_hard_coded_again():
    r"""The regression this replaced: a literal C:\RETRO_AGENT target."""
    body = _body("gs_place_tool_shortcuts")
    assert "C:\\\\RETRO_AGENT" not in body, (
        "gs_place_tool_shortcuts() names a fixed C:\\RETRO_AGENT path again - "
        "a box installed on another volume (D: on .186) silently gets no icon, "
        "because gs_tool_shortcut() treats a missing exe as 'not installed here'. "
        "Resolve it with gs_tool_leaf() instead."
    )
    assert "TP_FIXED_DIR" in GS, "the C:\\RETRO_AGENT fallback must still exist"


def test_same_name_and_path_as_the_push_tool():
    # a different name would leave two icons; a different path would never match
    assert re.search(r'LNK_NAME = "3dfx Control Panel"', PUSH)
    assert 'BOXDIR = r"C:\\RETRO_AGENT"' in PUSH and 'TARGET = BOXDIR + r"\\3dfxctl.exe"' in PUSH


def test_absent_exe_means_no_shortcut():
    i = GS.index("static void gs_tool_shortcut(")
    body = GS[i:GS.index("\n}\n", i)]
    assert body.index("if (!gs_file_exists(exe))") < body.index("gs_make_shortcut(")


def test_an_existing_shortcut_must_also_name_the_logo():
    i = GS.index("static void gs_tool_shortcut(")
    body = GS[i:GS.index("\n}\n", i)]
    assert "gs_lnk_points_at(lnk, exe) && (!icon || gs_lnk_has_icon(lnk, icon))" in body
    assert "gs_make_shortcut(exe, workdir, lnk, name, icon)" in body


def test_the_panel_is_copied_before_the_shortcuts_on_a_managed_box_only():
    i = GS.index("DWORD WINAPI gamesync_thread(")
    body = GS[i:GS.index("\n}\n", i)]
    guard = body.index("if (!host_manages_this_box())")
    ensure = body.index("fxpanel_ensure();")
    place = body.index("gs_place_tool_shortcuts();", ensure)
    assert guard < ensure < place


def test_the_agent_deploys_the_files_the_push_tool_publishes():
    src = (REPO / "agent" / "src" / "fxpanel.c").read_text()
    assert '{ "3dfxctl.exe", "3dfxlogo.ico" }' in src
    assert 'Utility\\\\Retro Automation\\\\3dfx' in src
    assert '"3dfxlogo.ico"' in PUSH or "3dfxlogo.ico" in PUSH
