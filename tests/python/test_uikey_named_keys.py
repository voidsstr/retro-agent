"""UIKEY must be able to send PrintScreen and the console key, and must flag
extended keys.

WHAT THIS PROTECTS. The agent's own SCREENSHOT command is a GDI BitBlt of the
screen DC, which returns a PURE BLACK frame for any Direct3D/OpenGL *exclusive
fullscreen* surface. Since the fleet standard is that every staged game runs
fullscreen, that left a whole class of titles unverifiable — a black frame
proves neither success nor failure.

The way out is to press the key the GAME listens on and let the game write its
own screenshot to disk:
  * PrintScreen  — most engines save a screenshot on it
  * ` / ~        — opens the Quake/GoldSrc/Unreal console so a `screenshot`
                   or `snapshot` command can be typed

Neither existed in `named_keys[]` in agent/src/input.c, so `UIKEY PRINTSCREEN`
answered "Unknown key" and a helper .exe had to be uploaded to each box instead.

Two further invariants are pinned here because both are silent when wrong:

1. EXTENDED KEYS. The arrows, the grey navigation cluster and PrintScreen live
   on E0-prefixed scan codes. Without KEYEVENTF_EXTENDEDKEY a game reading the
   keyboard through DirectInput sees the NUMPAD twin of the key, or nothing —
   which looks exactly like UIKEY being ignored rather than like a bug.

2. THE VK_SNAPSHOT SCAN-CODE FALLBACK. MapVirtualKey(VK_SNAPSHOT, 0) returns 0
   on some XP keyboard layouts, and keybd_event documents a special path for a
   zero scan code: it copies the SCREEN to the clipboard instead of delivering
   a keystroke. The game would never see the key and never write its shot, so
   the fix would appear to do nothing at all. input.c must substitute the real
   E0-37 PrintScreen scan code.

Source: agent/src/input.c — named_keys[], lookup_named_key_entry(),
named_key_is_extended(), send_key_press_ex().
"""

import os
import re

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
INPUT_C = os.path.join(REPO, "agent", "src", "input.c")


def _source():
    with open(INPUT_C, "r", encoding="utf-8", errors="replace") as fh:
        return fh.read()


def _named_keys_table(src):
    """Parse named_keys[] into {NAME: (vk, extended)}."""
    m = re.search(r"named_keys\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    assert m, "named_keys[] table not found in agent/src/input.c"
    out = {}
    for name, vk, ext in re.findall(
        r'\{\s*"([^"]+)"\s*,\s*(VK_\w+)\s*,\s*([01])\s*\}', m.group(1)
    ):
        out[name.upper()] = (vk, int(ext))
    assert out, "named_keys[] parsed as empty - has the row format changed?"
    return out


# --- the keys that make fullscreen verification possible at all --------------

@pytest.mark.parametrize(
    "name,vk",
    [
        ("PRINTSCREEN", "VK_SNAPSHOT"),
        ("PRTSC", "VK_SNAPSHOT"),
        ("PRTSCR", "VK_SNAPSHOT"),
        ("SYSRQ", "VK_SNAPSHOT"),
        ("TILDE", "VK_OEM_3"),
        ("BACKQUOTE", "VK_OEM_3"),
        ("GRAVE", "VK_OEM_3"),
        ("CONSOLE", "VK_OEM_3"),
    ],
)
def test_screenshot_and_console_keys_exist(name, vk):
    keys = _named_keys_table(_source())
    assert name in keys, (
        "UIKEY %s must resolve - without it a D3D/OpenGL fullscreen game "
        "cannot be screenshot-verified at all (agent SCREENSHOT returns black)"
        % name
    )
    assert keys[name][0] == vk, "%s must map to %s, got %s" % (name, vk, keys[name][0])


def test_printscreen_is_marked_extended():
    """PrintScreen is an E0 scan code; without the flag DirectInput games miss it."""
    keys = _named_keys_table(_source())
    for name in ("PRINTSCREEN", "PRTSC", "PRTSCR", "SYSRQ"):
        assert keys[name][1] == 1, "%s must be marked extended" % name


# --- the pre-existing keys whose extended flag was missing entirely ----------

@pytest.mark.parametrize(
    "name",
    ["UP", "DOWN", "LEFT", "RIGHT", "HOME", "END",
     "PAGEUP", "PAGEDOWN", "INSERT", "DELETE", "DEL"],
)
def test_navigation_cluster_is_extended(name):
    keys = _named_keys_table(_source())
    assert name in keys, "%s disappeared from named_keys[]" % name
    assert keys[name][1] == 1, (
        "%s is an E0-prefixed key; without KEYEVENTF_EXTENDEDKEY a DirectInput "
        "game sees the numpad twin instead, which looks like UIKEY being ignored"
        % name
    )


@pytest.mark.parametrize("name", ["ENTER", "ESCAPE", "SPACE", "TAB", "F1", "F12"])
def test_plain_keys_are_not_extended(name):
    """The old buggy value would have been to flag everything - guard both ways."""
    keys = _named_keys_table(_source())
    assert keys[name][1] == 0, "%s must NOT be marked extended" % name


# --- the fallback that stops the fix silently doing nothing -----------------

def test_vk_snapshot_zero_scancode_fallback():
    src = _source()
    m = re.search(r"static void send_key_press_ex\(.*?\n\}", src, re.S)
    assert m, "send_key_press_ex() not found in agent/src/input.c"
    body = m.group(0)

    assert "KEYEVENTF_EXTENDEDKEY" in body, (
        "send_key_press_ex() must apply KEYEVENTF_EXTENDEDKEY for extended keys"
    )
    assert re.search(r"sc\s*==\s*0\s*&&\s*vk\s*==\s*VK_SNAPSHOT", body), (
        "send_key_press_ex() must special-case a zero scan code for VK_SNAPSHOT: "
        "keybd_event treats scan code 0 as 'copy the screen to the clipboard' "
        "rather than delivering a keystroke, so the game never sees the key"
    )
    assert "0x37" in body, (
        "the VK_SNAPSHOT fallback must use the real E0-37 PrintScreen scan code"
    )


def test_uikey_handler_uses_the_extended_aware_path():
    """A table full of correct flags is useless if the caller drops them."""
    src = _source()
    assert "send_key_press_ex(vk, named_key_is_extended(last_part))" in src, (
        "the UIKEY handler must pass the key's extended flag through to "
        "send_key_press_ex(); calling send_key_press() discards it"
    )
