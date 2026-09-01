"""A 256-colour screen must be photographed in ITS colours, not the shell's.

THE FAILURE THIS GUARDS, measured on .133 (2026-08-31)
------------------------------------------------------
`SCREENSHOT 0` of StarCraft, Jedi Knight and Warcraft II - all of which run in
a 640x480x8 exclusive fullscreen mode - came back the right size, from a live
process, with the frame's geometry plainly visible, and with every colour
WRONG: confetti. The predecessor session recorded a `verified` cell on one of
those frames, which is how far the frames get before anyone notices.

Cause: on an 8-bpp display `CreateCompatibleBitmap` makes an 8-bpp DDB, so
`BitBlt` copies palette INDICES and `GetDIBits` is the step that must turn them
into RGB. It does that with the palette selected into the HDC it is handed -
and a fresh `CreateCompatibleDC` carries only the 20 static system colours. The
game's own 256 entries were never consulted.

Fix (agent 1.79.1, `screen_palette_for_capture` in agent/src/screen.c): read
the live hardware palette with `GetSystemPaletteEntries`, `CreatePalette`, then
`SelectPalette` + `RealizePalette` into the memory DC before `GetDIBits`.

WHY THE GUARD IS PART OF THE CONTRACT
-------------------------------------
The helper must return NULL on any display deeper than 8 bpp - the normal fleet
case - so the 16/32-bpp path is byte-for-byte what it was. A palette forced on
a true-colour capture would be a far worse regression than the bug, and it
would show up on every box at once.

This is a source assertion because the behaviour is pure Win32 GDI: it cannot
be exercised on the Linux dev host, and the honest thing to pin is that BOTH
capture paths - the still `SCREENSHOT` and the `SCREENDIFF` tile diff - go
through the helper. A diff that compares noise against noise is the same bug
wearing different clothes.
"""

import pathlib
import re

import pytest

REPO = pathlib.Path(__file__).resolve().parents[2]
SCREEN_C = REPO / "agent" / "src" / "screen.c"


@pytest.fixture(scope="module")
def src():
    assert SCREEN_C.exists(), f"missing {SCREEN_C}"
    return SCREEN_C.read_text(encoding="utf-8", errors="replace")


def _body(src, func):
    """The text of one function, from its opening brace to the next
    column-0 closing brace. Crude, and sufficient: this file's functions are
    all top-level and brace-aligned."""
    m = re.search(r"^[A-Za-z_].*\b%s\s*\(" % re.escape(func), src, re.M)
    assert m, f"{func} not found in screen.c"
    start = src.index("{", m.start())
    end = src.index("\n}", start)
    return src[start:end]


def test_helper_exists(src):
    assert "screen_palette_for_capture" in src, (
        "the 8-bpp palette translation helper is gone - every 256-colour "
        "fullscreen title will photograph as coloured noise again"
    )


def test_helper_reads_the_live_hardware_palette(src):
    body = _body(src, "screen_palette_for_capture")
    assert "GetSystemPaletteEntries" in body, (
        "the palette must come from the SCREEN DC's live system palette; "
        "anything else is a guess at what the game set"
    )
    assert "CreatePalette" in body


def test_helper_refuses_anything_deeper_than_8bpp(src):
    """The guard IS the contract - see the module docstring."""
    body = _body(src, "screen_palette_for_capture")
    assert "BITSPIXEL" in body and "PLANES" in body, (
        "no depth guard: a palette selected on a 16/32-bpp capture would "
        "regress every box on the fleet at once"
    )
    assert re.search(r">\s*8\s*\)?\s*\n?\s*return NULL;", body), (
        "the >8 bpp early-out must return NULL so the true-colour path is "
        "left exactly as it was"
    )
    assert "RC_PALETTE" in body, (
        "a non-palettised device has no system palette to read"
    )


def test_helper_clears_peflags(src):
    """PC_* bits come back from GetSystemPaletteEntries on some drivers and
    make the logical palette MAPPED rather than copied - which reintroduces
    the very translation error this exists to remove."""
    body = _body(src, "screen_palette_for_capture")
    assert "peFlags = 0" in body


@pytest.mark.parametrize("func", ["handle_screenshot", "screendiff_core"])
def test_both_capture_paths_use_it(src, func):
    body = _body(src, func)
    assert "screen_palette_for_capture" in body, (
        f"{func} does not translate the palette; SCREENSHOT and SCREENDIFF "
        "must not disagree about what a pixel means"
    )
    assert "SelectPalette" in body and "RealizePalette" in body, (
        f"{func} builds a palette and never selects/realizes it - GetDIBits "
        "would still use the DC's default 20 static colours"
    )
    # created on every call, so it must be released on every exit
    assert body.count("CreatePalette") == 0, "the helper owns creation"
    assert body.count("DeleteObject(hPal)") >= 2, (
        f"{func} leaks the HPALETTE on at least one exit path; SCREENSHOT is "
        "called in a loop by the click-shot lab, so a leak is a real one"
    )
