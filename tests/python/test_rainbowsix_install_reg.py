"""Rainbow Six's registry seed must not reintroduce GOG's XP-fatal defect.

WHY THIS EXISTS
---------------
The GOG build of Rainbow Six ships `regs.cmd`, and every one of its 40
`REG ADD` lines ends with `/reg:32`. **Windows XP's reg.exe has no /reg:32** -
it arrived with Vista - so on XP every line fails and not one value is written,
while the errors scroll past unread.

That is not cosmetic. 33 of the 34 asset-path defaults compiled into
RainbowSix.exe point at `\\data2\\` - the CD - and only `\\data\\journals`
defaults to disk. Without the registry values the game finds essentially
nothing, which presents as a broken install rather than as a missing switch.

So the fleet applies a generated install.reg with `regedit /s`, which avoids
reg.exe entirely and behaves identically on XP and Win7. These tests pin the
three things that make it work.
"""
import os
import re

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
REG = os.path.join(REPO, "provisioning", "rainbowsix", "install.reg")
BAT = os.path.join(REPO, "provisioning", "rainbowsix", "Play Rainbow Six.bat")


def _reg():
    with open(REG, encoding="ascii") as f:
        return f.read()


def _reg_active():
    """The file WITHOUT its comment header.

    The header deliberately explains the /reg:32 defect and quotes it, so a
    naive substring search over the whole file finds the very string the
    active lines must not contain.
    """
    return "\n".join(l for l in _reg().splitlines()
                      if not l.lstrip().startswith(";"))


def test_the_seed_exists_and_is_regedit4():
    assert os.path.isfile(REG)
    assert _reg().startswith("REGEDIT4"), (
        "Win9x-era regedit needs the REGEDIT4 dialect, not 'Windows Registry "
        "Editor Version 5.00'")


def test_it_never_uses_reg_exe_or_the_vista_only_switch():
    """The whole reason this file exists."""
    body = _reg_active()
    assert "/reg:32" not in body, (
        "/reg:32 does not exist on Windows XP - that is the defect this file "
        "was written to avoid")
    assert "REG ADD" not in body.upper()
    # ...and the header must still EXPLAIN it, or the next person deletes the
    # workaround as noise
    assert "/reg:32" in _reg(), "the comment header explaining the defect is gone"


def test_installtype_full_is_present_because_it_gates_the_cd_check():
    body = _reg_active()
    assert re.search(r'"InstallType"\s*=\s*"Full"', body), (
        "InstallType is compared at VA 0x40A17A and an EQUAL result SKIPS the "
        "CD check. Without it the game asks for a disc that does not exist - "
        "there is no Rainbow Six 1 disc image on the share at all.")
    assert re.search(r'"CDPath"\s*=', body)


def test_app_paths_is_seeded_because_a_file_copy_deploy_has_no_installer():
    body = _reg_active()
    assert "App Paths\\\\RainbowSix.exe" in body or \
           "App Paths\\RainbowSix.exe" in body, (
        "regs.cmd READS App Paths\\RainbowSix.exe\\Path back to build its own "
        "{app}; GAMESYNC copies files and runs no installer, so nothing else "
        "would ever create it")


def test_every_asset_path_points_at_the_deploy_dir_not_the_cd():
    body = _reg_active()
    paths = re.findall(r'"(\w*Path)"="([^"]+)"', body)
    assert len(paths) >= 30, "expected the full asset-path table, got %d" % len(paths)
    for name, value in paths:
        if name == "CDPath":
            continue
        assert value.lower().startswith("c:\\\\games\\\\rainbowsix"), (
            "%s still points at %r - if any asset path is left at its compiled "
            "\\data2\\ default the game looks on the CD for it" % (name, value))


def test_fullscreen_is_set_because_there_is_no_switch_for_it():
    body = _reg_active()
    assert re.search(r'"FullScreen"=dword:0*1', body), (
        "the fleet's testing rule is fullscreen, and RainbowSix.exe has no "
        "resolution or windowed switch at all - the whole image contains only "
        "-server and -client - so fullscreen can only be set here")


def test_the_launcher_warns_against_the_shipped_regs_cmd():
    with open(BAT, encoding="ascii") as f:
        bat = f.read()
    assert "regs.cmd" in bat, (
        "the vendor's regs.cmd stays in the tree as provenance; the launcher "
        "must say why it must not be run")
    assert "/reg:32" in bat
    # and it must not try to pass a resolution the exe cannot take
    assert "-w " not in bat and "FR_W" not in bat
