"""The box-ownership check must classify honestly, offline.

It exists because several agents drive one fleet and invalidated each other's
tests -- a title purged 11 minutes into someone else's verify, a game launched
on top of a connected client. Every agent connects from the same host, so the
box's own log is the only evidence two people are present.

The risk in a tool like this is a comfortable answer: saying "quiet" because
the parse failed is far worse than saying nothing, since it actively invites
the collision it exists to prevent.
"""
import importlib.util
import os

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(REPO, "scripts", "fleet", "box-owner.py")

spec = importlib.util.spec_from_file_location("box_owner", SRC)
bo = importlib.util.module_from_spec(spec)
spec.loader.exec_module(bo)


def test_polling_chatter_is_not_activity():
    """The chat daemon long-polls forever; that is not somebody working."""
    for quiet in ['[10:00:01][MAIN ] CMD: "PROMPT_WAIT 30000" (17 bytes)',
                  '[10:00:01][MAIN ] CMD: "STATUS_WAIT 0 30000" (19 bytes)',
                  '[10:00:01][MAIN ] CMD: "LOG_WAIT 0 30000" (16 bytes)',
                  '[10:00:01][MAIN ] CMD: "PING" (4 bytes)',
                  '[10:00:01][MAIN ] CMD: "GAMESYNC STATUS" (15 bytes)']:
        assert bo.NOISE.search(quiet), "%r must be filtered as noise" % quiet


def test_looking_is_not_changing():
    """Read-only commands must not read as somebody mutating the box."""
    for benign in ['[10:00:01][MAIN ] CMD: "SYSINFO" (7 bytes)',
                   '[10:00:01][MAIN ] CMD: "WINLIST" (7 bytes)',
                   '[10:00:01][MAIN ] CMD: "PROCLIST" (8 bytes)',
                   '[10:00:01][MAIN ] CMD: "DOWNLOAD C:\\x.ini" (17 bytes)']:
        assert not bo.MUTATING.search(benign), "%r is not a mutation" % benign


def test_the_dangerous_commands_are_all_caught():
    """These are the ones that invalidate another agent's test."""
    for bad in ["GAMESYNC START", "EXEC cmd /c rd /s /q C:\\Games\\X",
                "EXECW 600 setup.exe", "LAUNCH game.exe",
                "UPLOAD C:\\x", "DELETE C:\\x", "MKDIR C:\\x",
                "FILECOPY a|b", "REGWRITE HKLM a b REG_DWORD 1",
                "REGDELETE HKLM a", "UICLICK 1 2", "UIKEY TAB",
                "CLICKSHOT 1 2", "REBOOT", "RESTART", "QUIT", "DRVUPDATE x"]:
        line = '[10:00:01][MAIN ] CMD: "%s" (9 bytes)' % bad
        assert bo.MUTATING.search(line), "%r must count as mutating" % bad


def test_timestamp_parsing_and_its_absence():
    assert bo._secs("[01:02:03][MAIN ] CMD: \"PING\"") == 3723
    assert bo._secs("no timestamp here") is None, (
        "an unparseable line must return None, not 0 -- 0 would place it at "
        "midnight and silently drop it from every window"
    )


def test_the_agent_itself_is_never_reported_as_a_game():
    """Reporting retro_agent.exe as a running game would make every box BUSY."""
    for own in ("retro_agent.exe", "retro_chat.exe", "explorer.exe",
                "rotate_wall.exe", "arrange_icons.exe"):
        assert own in bo.NOT_A_GAME


def test_a_real_game_is_not_filtered_out():
    for game in ("sof2mp.exe", "quake3.exe", "halo.exe", "maxpayne.exe",
                 "game.exe", "sin.exe", "bf1942.exe"):
        assert game not in bo.NOT_A_GAME, (
            "%s is a game we launch; filtering it would hide a live test" % game
        )
        assert bo.GAMEISH.search(game)
