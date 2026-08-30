"""Regression: keep UnrealGold on the Voodoo 2 instead of the software rasterizer.

Encodes the hardware-verified fixes from 192.168.1.171 (2026-08-30).  Each
assertion checks BOTH the fixed value and the old-buggy value it replaced, so a
regression cannot pass by accident.

Verified on the box: with these settings the Unreal log shows ``grSstOpen``
twice against the real ``Glide 2.56.00.0459`` and **zero** ``Bound to SoftDrv``
/ ``Bound to D3DDrv`` lines across a full session.  Before the fix it showed
``WM_KILLFOCUS`` -> ``EndFullscreen`` -> ``Shutting down Glide`` ->
``Bound to SoftDrv.dll``.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
MOD_PATH = os.path.join(ROOT, "scripts", "voodoo2", "fix_glide_games.py")


def _mod():
    sys.path.insert(0, ROOT)
    spec = importlib.util.spec_from_file_location("fix_glide_games", MOD_PATH)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


# The stock Unreal Gold ini as found on .171, verbatim in the parts that matter.
STOCK_INI = (
    "[URL]\r\n"
    "Protocol=unreal\r\n"
    "\r\n"
    "[Engine.Engine]\r\n"
    "GameRenderDevice=D3DDrv.D3DRenderDevice\r\n"
    "AudioDevice=Galaxy.GalaxyAudioSubsystem\r\n"
    "WindowedRenderDevice=SoftDrv.SoftwareRenderDevice\r\n"
    "RenderDevice=D3DDrv.D3DRenderDevice\r\n"
    "\r\n"
    "[WinDrv.WindowsClient]\r\n"
    "WindowedViewportX=800\r\n"
    "WindowedViewportY=600\r\n"
    "FullscreenViewportX=1024\r\n"
    "FullscreenViewportY=768\r\n"
    "FullscreenColorBits=32\r\n"
    "CaptureMouse=True\r\n"
)


def _kv(text):
    out = {}
    for line in text.splitlines():
        if "=" in line and not line.startswith("["):
            k, _, v = line.partition("=")
            out[k] = v
    return out


def test_windowed_render_device_is_glide_not_softdrv():
    """The fix that actually matters.

    Unreal's splash steals the foreground a few seconds after the viewport has
    gone fullscreen on Glide; the resulting WM_KILLFOCUS makes Unreal switch to
    WindowedRenderDevice.  A Voodoo 2 cannot render windowed, so the stock
    SoftDrv value stranded the whole session on the software rasterizer.
    """
    fixed, _ = _mod().fix_unreal_ini(STOCK_INI)
    kv = _kv(fixed)
    assert kv["WindowedRenderDevice"] == "GlideDrv.GlideRenderDevice"
    # old-buggy value must be gone
    assert kv["WindowedRenderDevice"] != "SoftDrv.SoftwareRenderDevice"
    assert "SoftDrv" not in fixed


def test_all_three_render_devices_point_at_glide():
    kv = _kv(_mod().fix_unreal_ini(STOCK_INI)[0])
    for key in ("GameRenderDevice", "RenderDevice", "WindowedRenderDevice"):
        assert kv[key] == "GlideDrv.GlideRenderDevice", key
    assert "D3DDrv" not in _mod().fix_unreal_ini(STOCK_INI)[0]


def test_mode_is_one_a_voodoo2_can_actually_scan_out():
    """16bpp only, and 640x480 -- a 4MB FBI refuses 800x600 with 3 colour
    buffers (observed as ``Resolution 8 failed``), and 1024x768 needs SLI."""
    kv = _kv(_mod().fix_unreal_ini(STOCK_INI)[0])
    assert kv["FullscreenColorBits"] == "16"
    assert kv["FullscreenColorBits"] != "32"      # Voodoo 2 has no 32bpp mode
    assert kv["FullscreenViewportX"] == "640"
    assert kv["FullscreenViewportY"] == "480"
    assert kv["FullscreenViewportX"] != "1024"    # single card cannot do it


def test_each_rule_only_matches_its_own_whole_key():
    """``RenderDevice`` is a substring of ``GameRenderDevice`` /
    ``WindowedRenderDevice``, so the rules must be anchored to a line start.

    The three real keys currently share one value, which would mask an
    unanchored rule -- so this uses a foreign ``*RenderDevice=`` key to show the
    rule stays inside the key it names.
    """
    ini = STOCK_INI + "MyRenderDevice=KeepMe\r\n"
    fixed, _ = _mod().fix_unreal_ini(ini)
    assert "MyRenderDevice=KeepMe" in fixed, "unanchored rule rewrote a foreign key"
    assert "GameRenderDevice=GlideDrv.GlideRenderDevice" in fixed
    # exactly one line each, no duplicates introduced
    assert fixed.count("\nGameRenderDevice=") == 1
    assert fixed.count("\nRenderDevice=") == 1


def test_unrelated_lines_and_crlf_are_preserved():
    fixed, _ = _mod().fix_unreal_ini(STOCK_INI)
    assert "Protocol=unreal" in fixed
    assert "AudioDevice=Galaxy.GalaxyAudioSubsystem" in fixed
    assert "CaptureMouse=True" in fixed
    assert "WindowedViewportX=800" in fixed      # windowed size is NOT touched
    assert "\r\n" in fixed
    assert "\n\n" not in fixed.replace("\r\n", "\n\n").replace("\n\n", "\r\n")
    assert len(fixed.splitlines()) == len(STOCK_INI.splitlines())


def test_is_idempotent():
    m = _mod()
    once, first_changes = m.fix_unreal_ini(STOCK_INI)
    twice, second_changes = m.fix_unreal_ini(once)
    assert once == twice
    assert first_changes and not second_changes


def test_glide2x_is_identified_by_size():
    """The wrapper and the real driver are told apart by size, not by name.

    Both are called ``glide2x.dll``; the game-local one wins at load time.  On
    .171 the 1,310,720-byte nGlide shadowed the 226,304-byte real 3dfx Glide.
    """
    m = _mod()
    assert m.classify_glide2x(1_310_720) == "nglide-wrapper"
    assert m.classify_glide2x(226_304) == "real-3dfx"
    assert m.classify_glide2x(12_345) == "unknown"
    assert m.NGLIDE_GLIDE2X_SIZE != m.REAL_GLIDE2X_SIZE


if __name__ == "__main__":
    import traceback

    fails = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"  ok   {name}")
            except Exception:
                fails += 1
                print(f"  FAIL {name}")
                traceback.print_exc()
    print("FAILURES:", fails)
    raise SystemExit(1 if fails else 0)
