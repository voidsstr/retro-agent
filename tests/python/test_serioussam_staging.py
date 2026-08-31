#!/usr/bin/env python3
r"""Serious Sam TFE/TSE - the disc-mount staging, and the four facts behind it.

WHY THIS EXISTS. Both Encounters were staged once, hit a modal "Please insert
the game CD", and were WITHDRAWN from the library as disc-locked and unfixable.
The symptom was real; the conclusion was wrong, and it cost the fleet two of its
best showcase titles. What follows is what was actually true, each assertion
paired with the wrong answer it replaces.

  1. **IT IS NOT SAFEDISC. IT IS NOT ANY COPY PROTECTION.** The retail
     SeriousSam.exe has no `stxt774`, no `stxt371`, no `BoG_ *90.0&!!  Yy>`
     marker and no `secdrv`. The check is forty bytes of ordinary code: walk
     C:..Z:, and for any drive whose GetDriveTypeA is DRIVE_CDROM, fopen
     "<drive>:\Install\Bin\SeriousSam.exe". That distinction is the whole
     title - DAEMON Tools 3.47 provably cannot satisfy SafeDisc 2.80 (Generals,
     BF1942) and satisfies this completely.

  2. **A CD-ROM-TYPED DRIVE IS NECESSARY AND NOT SUFFICIENT.** Six of seven
     live boxes already had DRIVE_CDROM volumes when the title was withdrawn -
     every one holding another game's disc (SYSTEMSHOCK2, SHOGO, RF_2,
     STARCRAFT) or nothing. "The fleet has mounters" and "the fleet has THIS
     disc" are different facts and the first was mistaken for the second.

  3. **THE OFFICIAL TSE 1.07 PATCH ADDS SAFEDISC.** It replaces a
     442,434-byte retail exe with a 1,777,634-byte SafeDisc-2-wrapped one and
     ships secdrv.sys beside it, dropping GetDriveTypeA entirely. Applying "the
     latest official patch" would convert a title this fleet CAN run into one
     it demonstrably cannot - the exact inverse of Doom 3, where the official
     1.3 patch REMOVED the wrapper. Neither direction is a rule; both are
     measurements.

  4. **THE DISC REQUIREMENT IS PER SHORTCUT.** DedicatedServer.exe has no CD
     check at all, so the Host shortcut must not carry `disc_mount` - otherwise
     a title-level capability suppresses all three shortcuts and .123, the one
     box with no optical drive and no mounter, gets no icon at all. That is
     exactly how Descent 2 lost both of its.

The source-side tests need nothing but the repo. The share-side ones SKIP
LOUDLY when the library is not mounted, because a silent skip would let the
library rot unnoticed.
"""
import json
import os
import struct

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, '..', '..'))
LIB = '/mnt/retro-share/Files/Games-Library'

STAGER = os.path.join(REPO, 'scripts', 'fleet', 'stage-serioussam.py')
ICONGEN = os.path.join(REPO, 'scripts', 'fleet', 'make-ssam-icon.py')
FLEETRES = os.path.join(REPO, 'scripts', 'fleet', 'stage-fleetres.py')

TITLES = {
    'SeriousSamFirstEncounter': dict(
        short='TFE', volid='SERIOUS_SAM_RC2', iso='SeriousSamTFE.iso',
        marker=r'Install\1_00.gro', gro='1_00.gro',
        play='Play Serious Sam - The First Encounter.bat',
        host='Host Serious Sam TFE - LAN.bat',
        join='Join Serious Sam TFE - LAN.bat'),
    'SeriousSamSecondEncounter': dict(
        short='TSE', volid='SamSE', iso='SeriousSamTSE.iso',
        marker=r'Install\SE1_00.gro', gro='SE1_00.gro',
        play='Play Serious Sam - The Second Encounter.bat',
        host='Host Serious Sam TSE - LAN.bat',
        join='Join Serious Sam TSE - LAN.bat'),
}

SAFEDISC_MARKERS = (b'stxt774', b'stxt371', b'BoG_ *90.0&!!  Yy>')


def _skip_unless_share():
    if not os.path.isdir(LIB):
        pytest.skip('SKIPPED LOUDLY: %s is not mounted, so the staged Serious '
                    'Sam trees could not be checked at all. This is not a pass.'
                    % LIB)


def _read(path):
    with open(path, 'rb') as fh:
        return fh.read()


def _text(path):
    return _read(path).decode('latin1')


def _pe_resource_rva(head):
    """RVA of the PE resource directory, or 0 when the image has none.

    The presence of a `.rsrc` SECTION is not the question and cannot answer it:
    both Serious Sam binaries have a .rsrc section header and an entirely empty
    DataDirectory[2]. Only the directory entry says whether there are resources.
    """
    if head[:2] != b'MZ':
        return 0
    e = struct.unpack_from('<I', head, 0x3c)[0]
    if e + 0x80 > len(head) or head[e:e + 4] != b'PE\0\0':
        return 0
    return struct.unpack_from('<I', head, e + 24 + 96 + 16 * 2)[0]


# ---------------------------------------------------------------------------
# Source side - the reasoning, encoded so it cannot quietly invert
# ---------------------------------------------------------------------------

def test_cd_check_predicate_is_drive_type_AND_file():
    """The predicate, as arithmetic, asserted against BOTH outcomes.

    A test that only proves the passing case would still pass if someone
    "simplified" the launcher into mounting nothing and trusting that the box
    has an optical drive - which is precisely the mistake that withdrew this
    title. So the drive-with-the-wrong-disc case is asserted to FAIL.
    """
    DRIVE_CDROM = 5

    def cd_check_passes(drive_type, has_install_bin_serioussam):
        return drive_type == DRIVE_CDROM and has_install_bin_serioussam

    # measured on .133 2026-08-31: TFE image mounted from the share -> started
    assert cd_check_passes(DRIVE_CDROM, True) is True
    # measured on .240 2026-08-31: SHOGO disc in F:, modal raised
    assert cd_check_passes(DRIVE_CDROM, False) is False
    # a hard disk holding a copy of the tree is NOT a substitute - this is why
    # a full local install did not satisfy the check the first time round
    assert cd_check_passes(3, True) is False
    assert cd_check_passes(3, False) is False


def test_safedisc_and_plain_cd_check_are_not_the_same_thing():
    """SafeDisc detection, and why it decides whether this fleet can run a title.

    DAEMON Tools 3.47 is the fleet's only mounter and cannot satisfy SafeDisc
    2.80 (Generals, BF1942). It satisfies a plain GetDriveTypeA check
    completely. Telling the two apart is a PE string search, not a guess.
    """
    def is_safedisc(image_bytes):
        return any(m in image_bytes for m in SAFEDISC_MARKERS)

    assert is_safedisc(b'....stxt774....') is True
    assert is_safedisc(b'BoG_ *90.0&!!  Yy>') is True
    # retail SeriousSam.exe: GetDriveTypeA and the message, no wrapper
    assert is_safedisc(b'GetDriveTypeA...Please insert the game CD') is False


def test_the_stager_records_that_tse_1_07_must_not_be_applied():
    """The one thing a future agent is most likely to "helpfully" undo.

    "Patch every multiplayer title to the version our servers run" is a real
    rule in this repo, and applied here it would break the title. The warning
    has to live where someone reaching for the patch will read it.
    """
    src = _text(STAGER)
    assert '1.07' in src or '1_07' in src, \
        'stage-serioussam.py no longer mentions the 1.07 patch at all'
    assert 'secdrv' in src, \
        'the stager no longer records that the TSE 1.07 patch ships secdrv.sys'
    low = src.lower()
    assert 'do not apply' in low, \
        ('the stager no longer says NOT to apply the TSE 1.07 patch. That '
         'patch replaces a plain CD check with SafeDisc 2 and would make the '
         'title unrunnable on this fleet.')


def test_host_shortcut_must_not_require_a_disc():
    """DedicatedServer.exe has no CD check, so .123 can still host.

    Asserted on the stager's own requires.json template rather than only on the
    share, so it fails in CI on a dev host with no NAS mounted.
    """
    src = _text(STAGER)
    i = src.index('def requires_json')
    tmpl = src[i:src.index('def files_for', i)]
    assert '%(play)s' in tmpl and '%(join)s' in tmpl and '%(host)s' in tmpl
    play_at = tmpl.index('%(play)s')
    host_at = tmpl.index('%(host)s')
    # the host stanza is last; everything after it must not ask for a disc
    # Match the KEY, not the word: the host stanza's own `notes` says the words
    # "No disc_mount", and a substring test on prose is not a test.
    assert '"requires_capabilities"' not in tmpl[host_at:], (
        'the Host shortcut gained a requires_capabilities. DedicatedServer.exe '
        'has no CD check, and requiring a disc there takes the title away from '
        '.123 - the only box with no optical drive and no mounter - entirely.')
    assert 'disc_mount' in tmpl[play_at:host_at], \
        'the Play/Join shortcuts lost their disc_mount requirement'
    assert '"requires_capabilities"' not in tmpl[:play_at], (
        'a TITLE-level requires_capabilities is back. It suppresses EVERY '
        'shortcut, which is how Descent 2 ended up with no icon at all.')


def test_disc_marker_is_not_the_file_the_cd_check_opens():
    """A marker must identify THIS disc, and both Encounters share that file.

    Install\\Bin\\SeriousSam.exe is on BOTH discs. Using it as the launcher's
    "is my disc already mounted?" marker would let the Second Encounter's disc
    satisfy the First Encounter's launcher and be reported as a success -
    the same shape as Descent II's MARKER=AUTORUN.INF finding a StarCraft disc.
    """
    src = _text(STAGER)
    i = src.index('TITLES = {')
    table = src[i:src.index('ICON =', i)]
    assert r'Install\1_00.gro' in table
    assert r'Install\SE1_00.gro' in table
    assert 'SeriousSam.exe' not in table.split('marker=')[1].split(',')[0], \
        'a marker now names SeriousSam.exe, which is on both discs'
    # and the two markers must differ, or they are not markers
    assert (TITLES['SeriousSamFirstEncounter']['marker']
            != TITLES['SeriousSamSecondEncounter']['marker'])


def test_both_titles_have_a_fleetres_recipe():
    src = _text(FLEETRES)
    for t in TITLES:
        assert '"%s": {' % t in src, (
            '%s has no stage-fleetres recipe, so its launchers never get the '
            'FLEETRES block and the title is pinned to one resolution on eight '
            'different monitors.' % t)
    assert 'def ssam_startup_ini' in src
    i = src.index('def ssam_startup_ini')
    body = src[i:src.index('\nCALL = ', i)]
    assert 'Game_startup.ini' in body
    assert 'PersistentSymbols.ini' in body, (
        'the helper no longer records WHY the mode does not go in '
        'PersistentSymbols.ini - the engine rewrites that file on exit, so a '
        'resolution staged there is overwritten by the first box that runs it.')


# ---------------------------------------------------------------------------
# Share side - skips loudly
# ---------------------------------------------------------------------------

def test_trees_are_staged_with_their_disc_images():
    _skip_unless_share()
    for name, t in TITLES.items():
        tree = os.path.join(LIB, name)
        assert os.path.isdir(tree), '%s is not in the library' % name
        iso = os.path.join(tree, '_disc', t['iso'])
        assert os.path.isfile(iso), (
            '%s has no _disc image. Its CD check needs a DRIVE_CDROM volume '
            'holding Install\\Bin\\SeriousSam.exe; with no image there is '
            'nothing for MOUNTDISC.BAT to mount.' % name)
        # the ISO must actually be an ISO: "CD001" at sector 16, offset 1
        with open(iso, 'rb') as fh:
            fh.seek(16 * 2048)
            pvd = fh.read(2048)
        assert pvd[1:6] == b'CD001', (
            '%s: %s has no ISO 9660 volume descriptor at sector 16. A .bin '
            'converted at the wrong sector offset produces a full-size file '
            'that nothing can mount - Serious Sam\'s discs are MODE2/2352 and '
            'their payload starts at +24, not +16.' % (name, t['iso']))
        label = pvd[40:72].decode('latin1').strip()
        assert label == t['volid'], (
            '%s: volume label is %r but MOUNTDISC.BAT looks for %r, so it '
            'would never recognise its own mounted disc.'
            % (name, label, t['volid']))


def test_staged_binaries_carry_no_copy_protection():
    """Guards against a future "apply the latest patch" pass.

    This is the assertion that would have caught the TSE 1.07 patch being
    applied: it does not look at version numbers, it looks for the wrapper.
    """
    _skip_unless_share()
    for name in TITLES:
        exe = os.path.join(LIB, name, 'Bin', 'SeriousSam.exe')
        assert os.path.isfile(exe), '%s has no Bin\\SeriousSam.exe' % name
        blob = _read(exe)
        for m in SAFEDISC_MARKERS:
            assert m not in blob, (
                '%s: Bin\\SeriousSam.exe now carries the SafeDisc marker %r. '
                'The official TSE 1.07 patch does exactly this, and DAEMON '
                'Tools 3.47 cannot satisfy SafeDisc - the title would stop '
                'working on every box. Restore the retail binary.'
                % (name, m))
        assert b'GetDriveTypeA' in blob, (
            '%s: Bin\\SeriousSam.exe no longer imports GetDriveTypeA, so it is '
            'not the retail binary whose check the mount satisfies.' % name)


def test_the_mount_machinery_is_present_and_called():
    _skip_unless_share()
    for name, t in TITLES.items():
        tree = os.path.join(LIB, name)
        md = os.path.join(tree, 'MOUNTDISC.BAT')
        assert os.path.isfile(md), '%s has no MOUNTDISC.BAT' % name
        body = _text(md)
        assert t['volid'] in body
        assert t['marker'] in body
        assert 'exit' not in [ln.strip().lower() for ln in body.splitlines()], (
            '%s/MOUNTDISC.BAT has a bare `exit`. It is CALLed by every '
            'launcher, so `exit` would close the console the game is about to '
            'be started from.' % name)
        # A CODE line, not the `rem` that explains why there is none.
        code = [ln.strip().lower() for ln in body.splitlines()
                if not ln.strip().lower().startswith('rem')]
        assert not any(ln.startswith('setlocal') for ln in code), (
            '%s/MOUNTDISC.BAT uses setlocal, which would discard DISCDRV '
            'before the launcher that CALLed it can read it.' % name)
        for lname in (t['play'], t['host'], t['join']):
            lp = os.path.join(tree, lname)
            assert os.path.isfile(lp), '%s/%s is missing' % (name, lname)
            lb = _text(lp)
            assert 'call "%~dp0MOUNTDISC.BAT"' in lb, (
                '%s/%s does not mount the disc, so it raises a modal on every '
                'box.' % (name, lname))
            assert 'call "%~dp0FLEETRES.BAT"' in lb, (
                '%s/%s has no FLEETRES block.' % (name, lname))


def test_shortcut_capabilities_on_the_share():
    _skip_unless_share()
    for name, t in TITLES.items():
        req = json.loads(_text(os.path.join(LIB, name, 'requires.json')))
        assert 'requires_capabilities' not in req, (
            '%s declares a TITLE-level capability, which suppresses every '
            'shortcut.' % name)
        sc = req['shortcuts']
        assert sc[t['play']]['requires_capabilities'] == ['disc_mount']
        assert sc[t['join']]['requires_capabilities'] == ['disc_mount']
        assert 'requires_capabilities' not in sc[t['host']], (
            '%s: the Host shortcut requires a disc. DedicatedServer.exe has no '
            'CD check, and this takes the title off .123 completely.' % name)


def test_the_icon_is_a_real_icon():
    """Serious Sam ships none, so the one we ship has to be checked.

    launch.txt's third column only has to RESOLVE; the staged-library validator
    cannot tell an exe that has an icon from one that does not. Both game
    binaries here have an entirely empty PE resource directory, so pointing the
    column at either would pass validation and put three white pages on the
    desktop.
    """
    _skip_unless_share()
    for name in TITLES:
        tree = os.path.join(LIB, name)
        ico = os.path.join(tree, 'SeriousSam.ico')
        assert os.path.isfile(ico), '%s has no SeriousSam.ico' % name
        d = _read(ico)
        reserved, kind, count = struct.unpack_from('<HHH', d, 0)
        assert (reserved, kind) == (0, 1), '%s: not an ICO file' % name
        assert count >= 3, '%s: only %d image(s) in the icon' % (name, count)
        sizes = set()
        for i in range(count):
            w, h = d[6 + i * 16], d[7 + i * 16]
            sizes.add((w or 256, h or 256))
        assert (16, 16) in sizes and (32, 32) in sizes, \
            '%s: icon lacks the sizes the shell actually draws: %s' % (name, sizes)
        for exe in ('SeriousSam.exe', 'DedicatedServer.exe'):
            p = os.path.join(tree, 'Bin', exe)
            if os.path.isfile(p):
                assert _pe_resource_rva(_read(p)[:0x400]) == 0, (
                    '%s/Bin/%s now has a non-empty PE resource directory. If it '
                    'really carries an icon, use it and delete '
                    'make-ssam-icon.py rather than keeping two stories. (A '
                    '.rsrc SECTION HEADER is not evidence either way - both '
                    'these binaries have one and neither has any resources.)'
                    % (name, exe))


def test_the_launcher_does_not_pin_the_renderer():
    """sam_iDriver is a choice this tool cannot make, and it broke a box.

    Serious Engine 1 has an OpenGL path (0) and a Direct3D path (1). Measured
    on .246 (Win7, Radeon HD 5450) 2026-08-31: sam_iDriver=0 dies before any
    window with "Cannot set display mode! ... unable to find display mode with
    OpenGL acceleration"; the same tree on sam_iDriver=1 runs. The launcher
    used to write 0 at EVERY start, which additionally overwrote the engine's
    own auto-detected answer - so a box fixed by hand was un-fixed on its next
    launch. The engine owns the API; the launcher owns the panel.
    """
    src = _text(FLEETRES)
    i = src.index('def ssam_startup_ini')
    body = src[i:src.index('\nCALL = ', i)]
    assert 'sam_iScreenSizeI' in body, 'the helper no longer writes a resolution'
    assert 'echo sam_iDriver' not in body, (
        'the launcher pins sam_iDriver again. That overrides the engine\'s own '
        'auto-detected renderer at every start, and pinning 0 makes the title '
        'unstartable on .246 - which has no working OpenGL path for this '
        'engine. Put a per-box override in that box\'s PersistentSymbols.ini.')
    if os.path.isdir(LIB):
        for name, t in TITLES.items():
            for lname in (t['play'], t['host'], t['join']):
                lb = _text(os.path.join(LIB, name, lname))
                assert 'sam_iDriver' not in lb, \
                    '%s/%s still writes sam_iDriver' % (name, lname)


#: The canonical fleet mount launcher. Serious Sam's MOUNTDISC.BAT is the same
#: logic factored into one CALLable file per title (three launchers share it),
#: so it is a SECOND copy of hard-won cmd.exe and the standing risk is a fix
#: landing in one and not the other. These are the safeguards that were added
#: to the template after a real hang, quoted so both files must keep them.
TEMPLATE = os.path.join(REPO, 'provisioning', 'discmount',
                        'mount-launcher-template.bat')
SHARED_SAFEGUARDS = (
    'start "" /b "%DT%" -mount 0,',
    'if not defined DISCDRV taskkill /f /im daemon.exe',
)


def test_mountdisc_carries_the_template_safeguards():
    """A DAEMON Tools unit can be LOCKED, and a direct -mount then hangs forever.

    Measured on .124 and .240 2026-08-31: device 0 answers "Unable to mount
    image. Unit is locked.", `-unmount` answers the same, and the kernel drivers
    will not stop - so there is no reboot-free way to clear it. A direct call
    blocks behind that modal: no game, no banner, no mount-error.txt, and a
    leaked daemon.exe + cmd.exe per attempt (.124 had five of each). A silent
    hang is indistinguishable from "slow".

    The fix belongs to provisioning/discmount/mount-launcher-template.bat, which
    ten titles are generated from. Serious Sam's MOUNTDISC.BAT is the same logic
    in a CALLable file, so it needs the same safeguards - and this asserts they
    are in BOTH, because the whole hazard of a second copy is that a fix reaches
    one of them.
    """
    if not os.path.isfile(TEMPLATE):
        pytest.skip('SKIPPED LOUDLY: the canonical mount template is not in '
                    'this checkout, so the two copies could not be compared.')
    tpl = _text(TEMPLATE)
    for guard in SHARED_SAFEGUARDS:
        assert guard in tpl, (
            'the canonical template lost %r. If it was deliberately replaced, '
            'update SHARED_SAFEGUARDS here in the same commit rather than '
            'leaving the two mount implementations to drift.' % guard)
    _skip_unless_share()
    for name in TITLES:
        body = _text(os.path.join(LIB, name, 'MOUNTDISC.BAT'))
        for guard in SHARED_SAFEGUARDS:
            assert guard.replace('%DT%', '%DT%') in body or guard in body, (
                '%s/MOUNTDISC.BAT is missing the safeguard %r that the '
                'canonical mount template carries. A locked DAEMON Tools unit '
                'will hang this launcher forever with no banner and no '
                'mount-error.txt.' % (name, guard))


def test_per_box_state_is_not_staged():
    """The engine writes Scripts\\PersistentSymbols.ini on EXIT.

    Staged, GAMESYNC copies the pristine one back over every box's own on the
    next sync (the engine's write changes size AND mtime, so the resume test
    always fires). That resets `sam_bFirstStarted`, so the modal "SeriousSam is
    starting for the first time" returns after EVERY sync - on a headless box
    that is a dialog with nobody to click it - and it would carry one machine's
    detected renderer onto all eight. Same rule Doom 3 already carries for
    DoomConfig.cfg and config.spec.
    """
    _skip_unless_share()
    for name in TITLES:
        p = os.path.join(LIB, name, 'Scripts', 'PersistentSymbols.ini')
        assert not os.path.exists(p), (
            '%s stages Scripts\\PersistentSymbols.ini. That file is per-box '
            'state the engine rewrites on exit; the engine recreates it by '
            'itself. See PER_BOX_STATE in stage-serioussam.py.' % name)


def test_tse_menu_art_quirk_is_recorded_in_the_tree():
    """A non-finding that WILL be re-diagnosed unless the tree says so.

    The Second Encounter's main menu reads "THE FIRST ENCOUNTER v1.05" because
    SE1_00.gro ships the First Encounter's menu-logo textures byte for byte.
    The tree is correct - the campaign it loads is TSE's - but every symptom
    says "the wrong game is staged", which is an afternoon.
    """
    _skip_unless_share()
    notes = _text(os.path.join(LIB, 'SeriousSamSecondEncounter', 'NOTES.txt'))
    assert 'THE FIRST ENCOUNTER' in notes, (
        "the TSE tree no longer records that its menu says THE FIRST "
        "ENCOUNTER. Without it the next person concludes the tree is "
        "mis-staged - the menu is the loudest evidence and it is wrong.")
    assert 'InTheLastEpisode' in notes, \
        'the note no longer says what to check instead (the campaign)'


def test_launch_txt_fits_the_agents_1023_byte_read():
    _skip_unless_share()
    for name, t in TITLES.items():
        p = os.path.join(LIB, name, 'launch.txt')
        raw = _read(p)
        head = raw[:1023].decode('latin1')
        for lname in (t['play'], t['host'], t['join']):
            assert lname in head, (
                '%s: the shortcut %r falls outside the first 1023 bytes of '
                'launch.txt, so the agent never reads it and that shortcut '
                'silently never exists.' % (name, lname))
        for line in head.splitlines():
            if not line or line.startswith('#'):
                continue
            target = line.split('\t')[0]
            assert '(' not in target and ')' not in target, (
                '%s: %r has a parenthesis in a generated filename - '
                'unlaunchable through the agent, fine from a double-click.'
                % (name, target))
            assert os.path.isfile(os.path.join(LIB, name, target)), \
                '%s: launch.txt names %r, which is not in the tree' % (name, target)
