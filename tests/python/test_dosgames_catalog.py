"""Regression: DOS Game Manager host-side tooling (scripts/dosgames/).

Locks in the survey classification and catalog-generation behavior the DOS
utility depends on (verified end-to-end in DOSBox-X 2026-07-28), plus the
.PRV preview-tile format DOSGAME.EXE blits in mode 13h.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DG = os.path.abspath(os.path.join(HERE, "..", "..", "scripts", "dosgames"))


def _load(name):
    spec = importlib.util.spec_from_file_location(name, os.path.join(DG, name + ".py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_classify_installer_vs_ready():
    survey = _load("survey_share")
    # installer at root wins over exes (the dominant share pattern)
    r = survey.classify(["install.exe", "game.exe", "data.dat"])
    assert r["kind"] == "installer"
    # plain exe = ready-to-run
    r = survey.classify(["keen4e.exe", "egagraph.ck4"])
    assert r["kind"] == "ready-to-run"
    # cd image + installer
    r = survey.classify(["game.iso", "install.exe"])
    assert r["kind"] == "cd-image+installer"
    # setsound alone is a config tool but still flags installer-kind per
    # survey convention (INSTALLER_NAMES) — document that choice
    r = survey.classify(["setsound.exe", "duke3d.exe"])
    assert r["kind"] == "installer"


def test_gen_catalog_helpers():
    gc = _load("gen_catalog")
    # stem8: DOS-dir-safe, 8 chars, sanitized
    assert gc.stem8("keen4_shareware.zip") == "KEEN4_SH"
    assert gc.stem8("Redneck Rampage (1997)(Xatrix).zip") == "REDNECK_"
    # title: strips (year)(publisher), underscores to spaces
    assert gc.title_of("Doom (1993)(Id Software).zip") == "Doom"
    assert gc.title_of("keen4_shareware.zip") == "keen4 shareware"
    # main exe pick skips installers
    exe = gc.main_exe({"exes_shallow": ["install.exe", "doom.exe"]})
    assert exe == "DOOM.EXE"


def test_prv_tile_format():
    """A .PRV is exactly 768 palette bytes + 64000 (320x200) pixels."""
    tiles = os.path.join(DG, "data", "tiles")
    if not os.path.isdir(tiles):
        return
    prvs = [f for f in os.listdir(tiles) if f.upper().endswith(".PRV")]
    for f in prvs:
        size = os.path.getsize(os.path.join(tiles, f))
        assert size == 768 + 64000, "%s wrong size %d" % (f, size)
        base = os.path.splitext(f)[0]
        assert len(base) <= 8, "tile name must be 8.3: %s" % f


def test_no_blank_tiles_shipped():
    """A game that never drew within the render wait yields an all-black
    capture; gen_tiles.py rejects those (a written blank tile would also
    make reruns skip the game forever)."""
    tiles = os.path.join(DG, "data", "tiles")
    if not os.path.isdir(tiles):
        return
    blank = []
    for f in os.listdir(tiles):
        if not f.upper().endswith(".PRV"):
            continue
        raw = open(os.path.join(tiles, f), "rb").read()
        pixels = raw[768:]
        if len(set(pixels)) < 6:
            blank.append(f)
    assert not blank, "blank preview tiles shipped: %s" % blank


def test_gen_tiles_has_the_blank_gate():
    src = open(os.path.join(DG, "gen_tiles.py"), encoding="utf-8").read()
    assert "getextrema" in src and "return False" in src, (
        "gen_tiles.py must reject all-black captures before writing a tile")
