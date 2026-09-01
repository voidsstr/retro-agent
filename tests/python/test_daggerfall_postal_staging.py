#!/usr/bin/env python3
"""Daggerfall and Postal - the two GOG builds staged on 2026-09-01, and the
three traps each of them walked into.

Both were installed in the BUILD VM (never on a fleet box), captured to its
payload disk, and copied into the library byte-for-byte:

    Daggerfall  1633 files / 564,064,944 B   +6 fleet files = 1639 / 564,104,029
    Postal       580 files / 498,909,129 B

DAGGERFALL - runs, and its two facts are about WHO PAYS WHAT
------------------------------------------------------------
1. **GOG's own DOSBox conf waits for a keypress.** `dosbox_daggerfall_single.conf`
   draws a four-item menu and blocks on `choice /c1234`. A staged shortcut has
   nobody at the keyboard, so the game would never start - and an agent-driven
   test would photograph the menu and score the title as running. Shadow Warrior
   and Redneck Rampage have each paid for this already. Ours,
   `dosbox_daggerfall_fleet.conf`, carries the same two mount lines and goes
   straight to `fall.exe z.cfg`; GOG's is left in the tree, unreferenced.

2. **A cost the WRAPPER pays belongs on the shortcut, not the title.** FALL.EXE
   is a real DOS binary (a CauseWay LE image), so the 1996 game's own floor -
   486DX2/66, 8 MB - is the TITLE floor, and DOSBox's ~400 MHz host cost sits on
   `Play Daggerfall.bat`. Put the emulator's number at title level and the whole
   tree stops being copied to the one machine that could run the game natively.
   That is rule 6 in scripts/gamegate/SCHEMA.md and it is exactly what kept the
   DOS half of this library off the Pentium 1 for weeks.

3. **It is nonetheless WITHHELD from DOSGAME.TXT, with the reason recorded.**
   Z.CFG hardcodes `path C:\\arena2\\`, true only inside DOSBox where
   `mount C ".."` makes the tree root C:; and FALL.EXE takes its config as
   argv[1], which `<8.3 launcher><TAB><title>` cannot express. "We looked and it
   cannot work yet" and "we never looked" are different facts.

   VERIFIED ON HARDWARE: `.143` (AMD Athlon 1000, GeForce 6800, XP SP3) reached
   the Bethesda intro and then the Load/New/Exit menu, fullscreen at 640x480.

POSTAL - staged, gated, and NOT verified, which is three different things
-------------------------------------------------------------------------
GOG's "Postal Classic and Uncut" is Running With Scissors' 2018 SDL2 rebuild,
and **SSE2 is a real floor that the 1997 game never had**. Measured with objdump
against the staged binary: 868 `cvttsd2si`, 699 `movdqu`, 474 `mulsd`, 394
`addsd`, 301 `subsd`, 244 `comisd`, 131 `movdqa`, 125 `movapd`, 52 `xorpd`, 47
`ucomisd`, 45 `andpd`, 22 `pshufd` - against THREE `cpuid` sites, i.e. the CRT's
feature word and no dispatch to fall back on. An SSE2 instruction on a pre-SSE2
CPU is `#UD`, not "slow".

That rules it out on both boxes that were powered on: `.143` is an AMD Athlon
(fpu mmx cmov 3dnow - no SSE at all) and `.133` a dual Pentium III (fpu mmx cmov
sse - no SSE2), both read with HWPROFILE. The gate agrees - the published
verdict files say `no Postal cpu_features CPU lacks sse2` for both - so the
title is correctly never copied there. **It has therefore not been verified on
hardware, and requires.json says so in as many words.** Staged, gated and
unverified are three states and the library only lies when they render the same.
"""
import json
import os
import re

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
LIB = "/mnt/retro-share/Files/Games-Library"
DOSNATIVE = os.path.join(REPO, "scripts", "fleet", "stage-dosnative.py")
FLEETRES = os.path.join(REPO, "scripts", "fleet", "stage-fleetres.py")


def _read(path):
    with open(path, "rb") as fh:
        return fh.read().decode("latin1")


def _need_lib(what):
    if not os.path.isdir(LIB):
        pytest.skip("SKIPPED LOUDLY: %s is not mounted - %s NOT verified"
                    % (LIB, what))


# --------------------------------------------------------------------------
# Source-side: these run everywhere, with no share and no hardware.
# --------------------------------------------------------------------------
def test_daggerfall_is_withheld_from_dosgame_txt_with_a_reason():
    body = _read(DOSNATIVE)
    assert '"Daggerfall"' in body, (
        "Daggerfall is not in stage-dosnative.py at all. It carries a real DOS "
        "binary (FALL.EXE), so it must appear in DECLARE or in WITHHELD - "
        "silence reads as 'nobody looked'")
    withheld = body.split("WITHHELD = {", 1)[1]
    assert '"Daggerfall"' in withheld.split("\ndef ", 1)[0], (
        "Daggerfall must be in WITHHELD, not DECLARE: Z.CFG's 'path C:\\arena2\\' "
        "is only true inside DOSBox and FALL.EXE needs an argv[1] that "
        "DOSGAME.TXT cannot carry")
    for clue in ("Z.CFG", "arena2", "argv[1]"):
        assert clue in withheld, (
            "the WITHHELD reason no longer names %r - a withheld title without "
            "its blocker is indistinguishable from an oversight" % clue)


def test_both_titles_are_known_to_the_fleetres_stager():
    """Without an entry they get no FLEETRES payload, and `--check` reports
    the library as fully staged while two titles have no per-box resolution."""
    body = _read(FLEETRES)
    for title in ("Daggerfall", "Postal"):
        assert '"%s": {' % title in body, (
            "%s has no entry in stage-fleetres.py TITLES, so FLEETRES.EXE and "
            "FLEETRES.BAT are never staged into it" % title)


# --------------------------------------------------------------------------
# Share-side: SKIP LOUDLY when the library is not mounted.
# --------------------------------------------------------------------------
def test_the_daggerfall_launcher_never_uses_gogs_keypress_conf():
    _need_lib("the Daggerfall launcher")
    bat = os.path.join(LIB, "Daggerfall", "Play Daggerfall.bat")
    if not os.path.isfile(bat):
        pytest.skip("SKIPPED LOUDLY: %s absent - NOT verified" % bat)
    body = _read(bat)
    assert "dosbox_daggerfall_fleet.conf" in body
    assert "dosbox_daggerfall_single.conf" not in re.sub(
        r"(?m)^\s*rem .*$", "", body), (
        "the launcher passes GOG's _single.conf, whose [autoexec] blocks on "
        "`choice /c1234` - on a fleet box the game never starts and a "
        "screenshot shows the menu")


def test_the_fleet_conf_blocks_on_nothing():
    _need_lib("the Daggerfall fleet conf")
    conf = os.path.join(LIB, "Daggerfall", "dosbox_daggerfall_fleet.conf")
    if not os.path.isfile(conf):
        pytest.skip("SKIPPED LOUDLY: %s absent - NOT verified" % conf)
    body = _read(conf)
    data = [l for l in body.splitlines()
            if l.strip() and not l.lstrip().startswith("#")]
    for blocker in ("choice", "pause"):
        assert not any(re.search(r"\b%s\b" % blocker, l, re.I) for l in data), (
            "the fleet conf contains `%s`, which waits for a keypress nobody "
            "is there to give" % blocker)
    assert any("fall.exe" in l.lower() for l in data), (
        "the fleet conf no longer starts the game")
    assert any("z.cfg" in l.lower() for l in data), (
        "fall.exe is started without z.cfg, so it does not know where arena2\\ "
        "is - GOG's own autoexec passes it")


def test_daggerfall_puts_the_emulator_cost_on_the_shortcut_not_the_title():
    _need_lib("Daggerfall's requirements")
    path = os.path.join(LIB, "Daggerfall", "requires.json")
    if not os.path.isfile(path):
        pytest.skip("SKIPPED LOUDLY: %s absent - NOT verified" % path)
    with open(path, encoding="utf-8") as fh:
        req = json.load(fh)
    assert req["min_cpu_mhz"] <= 100, (
        "the TITLE-level floor is %s MHz. That decides whether the tree is "
        "COPIED AT ALL, and FALL.EXE is a native DOS binary a Pentium 1 runs "
        "at full speed - the emulator's cost belongs on the shortcut"
        % req["min_cpu_mhz"])
    sc = req.get("shortcuts", {}).get("Play Daggerfall.bat", {})
    assert sc.get("min_cpu_mhz", 0) >= 300, (
        "the DOSBox shortcut declares no host-CPU floor, so it would be offered "
        "on a machine that cannot drive the emulator")


def test_postal_declares_the_sse2_floor_it_really_has():
    _need_lib("Postal's requirements")
    path = os.path.join(LIB, "Postal", "requires.json")
    if not os.path.isfile(path):
        pytest.skip("SKIPPED LOUDLY: %s absent - NOT verified" % path)
    with open(path, encoding="utf-8") as fh:
        req = json.load(fh)
    assert "sse2" in [f.lower() for f in req.get("cpu_features", [])], (
        "Postal.exe is a 2018 MSVC /arch:SSE2 build - 868 cvttsd2si, 474 mulsd, "
        "699 movdqu, 3 cpuid sites and no dispatch. Without this floor the gate "
        "copies it to every pre-SSE2 box, where it dies on #UD before main()")
    assert req.get("min_os") == "winxp", (
        "SDL2 dropped Win9x and Postal.exe's PE subsystem version is 5.1")
    assert "UNVERIFIED" in req.get("notes", "").upper(), (
        "requires.json must say the title is unverified on hardware - no "
        "SSE2 box was powered on when it was staged, and 'staged' must never "
        "render the same as 'proven'")


def test_postal_does_not_wear_gogs_branding_as_its_icon():
    _need_lib("Postal's icon")
    path = os.path.join(LIB, "Postal", "launch.txt")
    if not os.path.isfile(path):
        pytest.skip("SKIPPED LOUDLY: %s absent - NOT verified" % path)
    rows = [l for l in _read(path).splitlines()
            if l.strip() and not l.strip().startswith("#")]
    assert rows, "Postal/launch.txt has no data line"
    icon = rows[0].split("\t")[2].strip() if rows[0].count("\t") >= 2 else ""
    assert icon, "Postal's shortcut has no explicit icon, so it auto-resolves"
    assert os.path.basename(icon).lower() not in ("gog.ico", "support.ico"), (
        "the shortcut would wear GOG's own branding rather than the game's. "
        "Postal.exe really does carry RT_ICON and RT_GROUP_ICON resources")
