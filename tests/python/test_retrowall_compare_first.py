"""retrowall re-applies only what differs (agent 1.85.0).

Until 1.85.0 every agent start re-applied the whole fleet desktop: 29 colour
writes + SetSysColors (every window repaints) + explicit WM_SYSCOLORCHANGE and
WM_THEMECHANGED broadcasts + SetSystemVisualStyle; two screensaver
SPIF_SENDWININICHANGE broadcasts; a wallpaper reload + broadcast; and on XP a
cmd.exe + taskkill spawn for a rotate_wall.exe that had long been renamed
.superseded. tests/native/test_rwcompare.c proves the comparisons; this pins
that retrowall.c makes them BEFORE the expensive call, and that the documented
ordering rule (apply calls above retrowall_apply_startup's early returns) still
holds - see also test_icon_autoarrange_source.py.
"""
import re
from pathlib import Path

SRC = Path(__file__).resolve().parents[2] / "agent" / "src" / "retrowall.c"


def code_only(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def body(src, name):
    m = re.search(r"^[A-Za-z_][\w \*]*\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{",
                  src, re.M | re.S)
    assert m, f"{name} not found"
    i = src.index("{", m.start())
    depth = 0
    for j in range(i, len(src)):
        if src[j] == "{":
            depth += 1
        elif src[j] == "}":
            depth -= 1
            if depth == 0:
                return src[i:j + 1]
    raise AssertionError(name)


def rw():
    return code_only(SRC.read_text(errors="replace"))


def test_theme_colours_are_read_before_they_are_set():
    b = body(rw(), "apply_hacker_theme")
    assert b.index("GetSysColor(") < b.index("SetSysColors(")
    assert "GetSysColorBrush(" in b, "unsupported indexes must be excluded"
    guard = b.rindex("if (rw_colors_need_apply(", 0, b.index("SetSysColors("))
    assert b.index("SetSysColors(") - guard < 120, "SetSysColors must be conditional"
    assert "hkcu_set_sz(" not in b, "colour/theme registry writes go through hkcu_ensure_sz"


def test_the_visual_style_and_broadcasts_only_happen_on_a_change():
    b = body(rw(), "apply_hacker_theme")
    assert b.index("IsThemeActive") < b.index("fn(L\"\", L\"\", L\"\", 0)")
    t = b.index("WM_THEMECHANGED")
    assert "if (style_changed)" in b[t - 80:t]
    c = b.index("WM_SYSCOLORCHANGE")
    assert "if (colors_live || style_changed)" in b[c - 120:c]


def test_the_themes_service_start_type_is_read_before_it_is_rewritten():
    b = body(rw(), "stop_and_disable_themes")
    assert b.index('"Start"') < b.index("ntdyn_ChangeServiceConfigA(")
    assert "if (!disabled &&" in b


def test_the_screensaver_is_asked_before_it_is_set():
    b = body(rw(), "set_starfield_screensaver")
    for get, setv in (("SPI_GETSCREENSAVEACTIVE", "SPI_SETSCREENSAVEACTIVE"),
                      ("SPI_GETSCREENSAVETIMEOUT", "SPI_SETSCREENSAVETIMEOUT")):
        assert b.index(get) < b.index(setv)
    assert "if (sets & RW_SS_SET_ACTIVE)" in b and "if (sets & RW_SS_SET_TIMEOUT)" in b
    assert "hkcu_set_sz(" not in b


def test_the_wallpaper_is_compared_like_the_keeper_does():
    b = body(rw(), "apply_fleet_wallpaper")
    spi = b.index("SPI_SETDESKWALLPAPER")
    assert b.index('hkcu_get_sz(DESKTOP_KEY, "Wallpaper"') < spi
    assert "if (same && !style)" in b[:spi]
    # the keeper must still know the fleet wallpaper even when nothing was applied
    assert b.index("safe_strncpy(g_fleet_wall, best") < b.index("if (same && !style)")


def test_no_taskkill_spawn_and_a_basename_toolhelp_kill():
    s = rw()
    assert "taskkill" not in s, "the cmd.exe + taskkill spawn is back"
    k = body(s, "kill_by_image")
    assert "CreateToolhelp32Snapshot" in k and "TerminateProcess" in k
    assert "rw_image_is(pe.szExeFile" in k, "9x's szExeFile is a full path"
    stop = body(s, "stop_wallpaper_rotation")
    assert 'kill_by_image("rotate_wall.exe")' in stop


def test_the_apply_calls_still_sit_above_the_early_returns():
    b = body(rw(), "retrowall_apply_startup")
    first_return = b.index("return;", b.index("host_policy_skip"))
    for call in ("apply_hacker_theme()", "set_starfield_screensaver()",
                 "gs_desktop_icons_apply()"):
        assert b.index(call) < b.index("if (apply_fleet_wallpaper())"), call
    assert b.index("g_rw_changed = 0") < b.index("apply_hacker_theme()")
    assert first_return < b.index("g_rw_changed = 0")
