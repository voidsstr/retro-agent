#!/usr/bin/env python3
"""DOOM 3 — the three findings that were bought on hardware, 2026-08-31.

WHY THIS EXISTS. DOOM 3 was blocked for weeks on what looked like one problem
("it needs a CD key") and was really two, one of which nobody had measured:

  1. **RETAIL 1.0 IS SAFEDISC-WRAPPED AND WILL NOT START WITHOUT THE DISC.**
     Measured on .123 with no disc and no key: a modal *"Cannot locate the
     DVD-ROM"*, before any key prompt. The wrapper is visible in the PE —
     sections `stxt774` / `stxt371` and the `BoG_ *90.0&!!  Yy>` marker, whose
     three following dwords read **3 / 0x14 / 0x16 = SafeDisc 3.20.022**.
     A CD key alone would never have unblocked this title.

  2. **THE OFFICIAL id 1.3 UPDATE SHIPS AN EXE WITH NO WRAPPER AT ALL.** The
     patch's `Doom3.exe` has six ordinary sections including `.reloc`, no
     `stxt*`, no `BoG_`, no `secdrv` string, and no "Cannot locate" string.
     Verified end to end on .133: retail installed from the owner's three disc
     images, patched with the genuine `DOOM 3 UPDATE 1.3.exe`, image unmounted,
     and the game reached its main menu with every optical drive empty. So the
     staged tree needs **no mounter and no `disc_mount` capability** — which is
     why `requires.json` deliberately declares neither, and why shipping the
     retail exe by accident would silently re-break the title on every box.

  3. **id Tech 4 IS THE OPPOSITE OF id Tech 3 ABOUT THE COMMAND LINE.** Every
     id Tech 3 title in this library had to have `seta r_mode` DELETED from its
     staged autoexec.cfg, because that cfg is exec'd after Com_StartupVariable
     and before R_Init and therefore BEAT the command line. DOOM 3 re-applies
     startup variables a second time, *after* exec'ing DoomConfig.cfg — id's own
     source comments it "re-override anything from the config files with command
     line args" — so here the command line wins and there is no fleetres.cfg to
     write. Getting this backwards would produce a launcher that looks right and
     silently does nothing.

The source-side tests run on the dev host in milliseconds. The share-side ones
SKIP LOUDLY when the library is not mounted, because a silent skip would let the
one binary this title's licence-cleanliness depends on rot unnoticed.

NOTE ON SECRETS: nothing here asserts the CD key's value. `base/doomkey` is
checked for SHAPE only — the vault (`fleet-gamekey-doom3`) is the record.
"""
import hashlib
import importlib.util
import os
import re
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
STAGER = os.path.join(REPO, 'scripts', 'fleet', 'stage-fleetres.py')
LIB = '/mnt/retro-share/Files/Games-Library'
TREE = os.path.join(LIB, 'Doom3')

# The two binaries that must never be confused. Both are 5,832,704 bytes, which
# is exactly why the size is not evidence and the digest is.
OFFICIAL_1_3_MD5 = '7cd77c22b38c223ef1047083e374875a'   # from DOOM 3 UPDATE 1.3.exe
TNT_CRACK_MD5 = '362672e5e25f1ece5410750eb6192e7b'      # share PATCH\DOOM3.EXE - BLOCKED


def _load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


sf = _load('stage_fleetres_d3', STAGER)


# ---------------------------------------------------------------------------
# 1. id Tech 4 takes the command line — no staged cfg to fight with
# ---------------------------------------------------------------------------

def test_doom3_is_in_the_titles_table():
    assert 'Doom3' in sf.TITLES, (
        "the per-box resolution recipe is what makes this title fullscreen at "
        "the right size on eight different monitors; without it the launcher "
        "is never generated")


def test_doom3_writes_no_fleetres_cfg():
    """The id Tech 3 titles here strip a latched `seta r_mode` and exec a
    launcher-written fleetres.cfg instead. DOOM 3 must NOT: its command line
    already wins, and a cfg would be a second place for the answer to live."""
    spec = sf.TITLES['Doom3']
    assert 'cfg_strip' not in spec
    assert 'cfg_exec' not in spec
    for name in spec['new']:
        text = sf.NEW_DOOM3.format(title='t', exe='DOOM3.exe', block=sf.CALL,
                                   args=sf.doom3_args())
        assert 'fleetres.cfg' not in text, name


def test_doom3_args_use_the_idtech4_cvar_names():
    """id Tech 4 spells these r_customWidth / r_customHeight / r_aspectRatio.
    id Tech 3's lower-case r_customwidth is a DIFFERENT cvar name and setting it
    here is a silent no-op — the engine takes its default and renders small."""
    a = sf.doom3_args()
    assert '+set r_mode -1' in a
    assert '+set r_customWidth %FR_W%' in a
    assert '+set r_customHeight %FR_H%' in a
    assert '+set r_aspectRatio %FR_D3AR%' in a
    assert '+set r_fullscreen 1' in a


def test_doom3_declares_no_hard_coded_resolution():
    """One staged tree, eight monitors: a literal is wrong somewhere by
    construction."""
    a = sf.doom3_args()
    assert not re.search(r'\b(640|800|1024|1280|1600|1920)\b', a), a


def test_aspect_ratio_is_computed_per_box_not_stamped():
    """r_aspectRatio 0=4:3, 1=16:9, 2=16:10 and the engine derives horizontal
    FOV from it, so a 16:9 panel driven at the right pixel count with the
    default 0 is still stretched. Three of this fleet's monitors are 4:3 tubes
    and four are 16:9 LCDs, so it cannot be a constant."""
    text = sf.NEW_DOOM3.format(title='t', exe='DOOM3.exe', block=sf.CALL,
                               args=sf.doom3_args())
    assert 'set FR_D3AR=0' in text
    assert '%FR_W%*9' in text and '%FR_H%*16' in text
    assert '%FR_W%*10' in text
    assert 'set FR_D3AR=1' in text and 'set FR_D3AR=2' in text
    # and the launcher must actually consume what it computed
    assert '%FR_D3AR%' in text


def test_host_launcher_sets_the_map_before_spawning_the_server():
    """`+spawnServer` acts on si_map as it stands at that moment, so a host
    launcher that spawns first drops the host at the menu instead of into a
    game — which looks exactly like the LAN not working."""
    exe, extra = sf.TITLES['Doom3']['new']['Host DOOM 3 - LAN.bat']
    assert exe == 'DOOM3.exe'
    assert '+set si_map' in extra
    assert extra.rstrip().endswith('+spawnServer')
    assert extra.index('+set si_map') < extra.index('+spawnServer')


def test_both_launchers_carry_the_resolution_arguments():
    """The LAN host is a full game client too — it must not come up at the
    engine default while only the single-player shortcut is fixed."""
    for name, (exe, extra) in sf.TITLES['Doom3']['new'].items():
        args = sf.doom3_args(extra or '')
        assert '+set r_fullscreen 1' in args, name
        assert '%FR_W%' in args and '%FR_H%' in args, name


# ---------------------------------------------------------------------------
# 2. the share: provenance and per-box hygiene
# ---------------------------------------------------------------------------

def _skip_if_no_share():
    if not os.path.isdir(LIB):
        pytest.skip('%s not mounted - the STAGED DOOM 3 TREE WAS NOT CHECKED; '
                    'run this on a host with the share mounted before any '
                    'imaging run' % LIB)
    if not os.path.isdir(TREE):
        pytest.skip('%s not present - DOOM 3 is not staged here' % TREE)


def _md5(path):
    h = hashlib.md5()
    with open(path, 'rb') as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def test_staged_exe_is_the_official_patch_not_the_scene_crack():
    """The share also carries PATCH\\DOOM3.EXE, a TNT scene crack of the SAME
    BYTE SIZE. Size is therefore not evidence; the digest is. Shipping the
    crack would put warez in the library while looking identical in a listing."""
    _skip_if_no_share()
    exe = os.path.join(TREE, 'DOOM3.exe')
    assert os.path.isfile(exe), 'DOOM3.exe missing from the staged tree'
    got = _md5(exe)
    assert got != TNT_CRACK_MD5, (
        'the staged DOOM3.exe IS the TNT scene crack - replace it with the '
        'exe from the official id 1.3 update')
    assert got == OFFICIAL_1_3_MD5, (
        'the staged DOOM3.exe is neither the official 1.3 exe nor the known '
        'crack (md5 %s) - identify it before shipping it' % got)


def test_staged_exe_carries_no_safedisc_wrapper():
    """Independent of the digest, because a future official patch would change
    the digest and must still be wrapper-free. Retail 1.0 fails all three of
    these and refuses to start without the disc."""
    _skip_if_no_share()
    with open(os.path.join(TREE, 'DOOM3.exe'), 'rb') as fh:
        blob = fh.read()
    assert b'stxt774' not in blob
    assert b'stxt371' not in blob
    assert b'BoG_ *90.0&!!' not in blob


def test_requires_json_declares_no_disc_mount():
    """The whole point of patching to 1.3 is that no disc is needed. Declaring
    disc_mount would suppress the shortcut on .123 and .246, which have no
    mounter, for a requirement this title does not have."""
    _skip_if_no_share()
    import json
    with open(os.path.join(TREE, 'requires.json')) as fh:
        req = json.load(fh)
    assert 'disc_mount' not in req.get('requires_capabilities', [])
    for sc in req.get('shortcuts', {}).values():
        assert 'disc_mount' not in sc.get('requires_capabilities', [])


def test_no_per_box_config_is_shipped():
    """DoomConfig.cfg is written by the engine on exit and holds THAT box's
    resolution, video settings and binds. A copy captured on one machine would
    carry a dual-PIII's 640x480 onto a 1080p box, and the launcher's command
    line would then be arguing with a file that has no business being there.
    config.spec is the same shape: its presence tells the engine it has already
    detected the machine's spec, so shipping one freezes every box on whichever
    machine wrote it."""
    _skip_if_no_share()
    for junk in ('base/DoomConfig.cfg', 'base/config.spec'):
        p = os.path.join(TREE, junk.replace('/', os.sep))
        assert not os.path.exists(p), (
            '%s is per-box state and must not be staged' % junk)


def test_doomkey_is_present_and_shaped_like_a_key():
    """SHAPE ONLY - the value belongs in the vault (fleet-gamekey-doom3) and
    must never be asserted in the repo. The installer writes the 18-character
    key, then a blank line, then two comment lines; the game rewrites the same
    file on exit, so only the first line is load-bearing."""
    _skip_if_no_share()
    p = os.path.join(TREE, 'base', 'doomkey')
    assert os.path.isfile(p), (
        'base/doomkey is missing - DOOM 3 prompts for a key at every start '
        'without it, and that dialog is not automatable')
    with open(p, 'rb') as fh:
        first = fh.readline().strip().decode('ascii', 'replace')
    assert re.fullmatch(r'[A-Z0-9]{18}', first), (
        'doomkey first line is not 18 upper-case alphanumerics; the installer '
        'CD Key dialog is five fields of 4/4/4/4/2')
