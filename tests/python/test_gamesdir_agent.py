"""GamesDir (agent 1.93.0): GAMESYNC installs titles on the disk the box
names, and REFUSES - never falls back to C: - when that disk is not there.

.243's C: is 1.2 GB with 333 MB free; its games now go to a 72 GB E:. After a
power loss E: is missing until the CMOS 1Bh reboot (agent 1.92.0), and a
fallback would pour the library onto the small C: the setting protects.
tests/native/test_gamesdir.c tests the path validator itself."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
GS = (ROOT / "agent" / "src" / "gamesync.c").read_text()
GR = (ROOT / "agent" / "src" / "gameres.c").read_text()


def body(src, name):
    m = re.search(r"^[A-Za-z_][\w \*]*\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{", src, re.M | re.S)
    assert m, name
    i = src.index("{", m.start())
    d = 0
    for j in range(i, len(src)):
        d += {"{": 1, "}": -1}.get(src[j], 0)
        if d == 0:
            return src[i:j + 1]
    raise AssertionError(name)


def test_a_configured_but_unusable_gamesdir_refuses_instead_of_falling_back():
    r = body(GS, "gs_dest_resolve")
    assert '"GamesDir"' in r and "gamesdir_normalise(" in r
    assert "GetDriveTypeA(root) != DRIVE_FIXED" in r
    assert r.count("return -1;") == 2 and "if (!have) return 0;" in r
    run = body(GS, "gs_run")
    chk = run.index("gs_dest_resolve(g_gs_dest, sizeof(g_gs_dest), g_gs_dest_root, why, sizeof(why)) < 0")
    seg = run[chk:run.index("gs_set_msg(\"enumerating library\")")]
    assert "g_gs.state = GS_FAILED;" in seg and "return;" in seg and "NOT SYNCING" in seg
    assert chk < run.index('gs_set_msg("enumerating library")')


def test_every_copy_path_uses_the_resolved_folder_and_its_drive():
    assert not re.search(r"\bGS_DEST\b", GS), "the old hard-coded C:\\Games constant must be gone"
    run = body(GS, "gs_run")
    assert run.count("g_gs_dest,") >= 2 and "gs_mkdir_p(g_gs_dest)" in run
    assert "gs_free_bytes(g_gs_dest_root)" in run and "gs_free_margin_for(g_gs_dest_root)" in run
    assert 'gs_free_bytes("C:\\\\")' not in run, "game copies are measured on the games drive"
    # the driver payload and DRVSTORE stay on C: - that is where they live
    assert GS.count('gs_free_bytes("C:\\\\")') >= 3


def test_gameres_and_status_follow_it():
    assert "gs_games_dir(root, sizeof(root))" in GR and '"C:\\\\Games\\\\%s"' not in GR
    assert '\\"dest\\":\\"%s\\"' in GS


def test_the_title_table_has_room_for_the_win9x_dos_titles():
    m = re.search(r"#define GS_MAX_TITLES\s+(\d+)", GS)
    assert m and int(m.group(1)) >= 256
