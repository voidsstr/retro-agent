#!/usr/bin/env python3
"""The disc-mount launcher template — the invariants that make ONE copy safe.

WHY THIS EXISTS
---------------
The fleet's resilient disc-mount launcher is ~300 lines of cmd.exe, and every
paragraph of it records a failure that really happened on hardware:

  * spraying -mount spellings at Daemon Tools 3.x raises a MODAL dialog that
    then blocks every later daemon.exe call (System Shock 2 died on that);
  * batchmnt64.exe on 32-bit Windows exits 216, and because the exit code was
    not checked the launcher fell through and started Brood War against the
    SHOGO disc — a wrong-architecture binary produced a CONFIDENT WRONG RESULT;
  * a MARKER of AUTORUN.INF matched a mounted StarCraft disc, so Descent II
    skipped its own mount and ran against the wrong disc;
  * "no mounter installed" and "a mounter ran but no drive appeared" are
    different calls to action and must not render the same.

By 2026-08-31 that file had been hand-copied into three staged titles, and two
more were about to be added. A hand-copy is how a fix lands in one launcher and
not the others. So the launcher is now GENERATED from one template plus a small
per-title spec, and these tests are what stop the template drifting away from
the hand-written launchers that were actually proven on hardware.

These read the repo, not the share, so they run on the dev host in milliseconds.
"""
import json
import os
import subprocess
import sys

import pytest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
GEN = os.path.join(REPO, 'scripts', 'fleet', 'make-mount-launcher.py')
TEMPLATE = os.path.join(REPO, 'provisioning', 'discmount', 'mount-launcher-template.bat')
SPECS = os.path.join(REPO, 'provisioning', 'discmount', 'specs')
LIBRARY = '/mnt/retro-share/Files/Games-Library'


def _template():
    with open(TEMPLATE, 'r', encoding='latin-1', newline='') as f:
        return f.read()


def _spec(name):
    with open(os.path.join(SPECS, name)) as f:
        return json.load(f)


def _generate(spec_name):
    out = subprocess.run([sys.executable, GEN, '--spec', os.path.join(SPECS, spec_name)],
                         capture_output=True)
    assert out.returncode == 0, out.stderr.decode()
    return out.stdout.decode('latin-1')


# --------------------------------------------------------------------------
# 1. The template must still contain every hard-won safeguard.
# --------------------------------------------------------------------------

@pytest.mark.parametrize('needle,why', [
    ('-mount 0,',
     'Daemon Tools 3.47 syntax. An unsupported switch raises a modal dialog '
     'that blocks every later daemon.exe call.'),
    ('DTKIND',
     'the build is decided by install path and ONE call is issued; the old '
     'code sprayed every spelling and ignored the failures.'),
    ('batchmnt64.exe',
     'WinCDEmu 64-bit binary must be selected by ARCHITECTURE - on 32-bit it '
     'exits 216 and the launcher used to fall through to the wrong disc.'),
    ('PROCESSOR_ARCHITEW6432',
     'the only reliable 32-on-64 detection; without it WCD/WCD2 are picked '
     'backwards.'),
    ('NoDriveTypeAutoRun',
     'AutoPlay throws a modal window over the running game; taskkilling the '
     'autorun exe afterwards does not stop the shell dialog.'),
    ('mount-error.txt',
     'a tolerated failure must be fetchable by an agent, not merely printed.'),
    ('NO DISC MOUNTER IS INSTALLED',
     '"not installed" and "mounted nothing" are different calls to action.'),
    ('MOUNT FAILED',
     'the launcher proceeds against whatever disc is present but must SAY it '
     'is a tolerated failure, not conceal it.'),
    (':waitdisc',
     'a mount is asynchronous; starting the game too early is the classic '
     'spurious "please insert the CD".'),
    ('start "" /b "%DT%"',
     'NEVER wait on daemon.exe. A DAEMON Tools unit can be LOCKED (.124 and '
     '.240, 2026-08-31) and a direct call then blocks forever behind a modal: '
     'no game, no banner, no mount-error.txt, and a leaked daemon.exe plus '
     'cmd.exe on every attempt - .124 had five of each. start /b returns '
     'immediately so :waitdisc decides on the post-condition instead.'),
    ('if not defined DISCDRV taskkill /f /im daemon.exe',
     'a locked unit leaves a stuck daemon.exe behind a modal; clearing it stops '
     'this launch wedging the next one, and stops one title blocking another.'),
    ('@@POSTLAUNCH@@',
     'the post-launch hook. Without it a LAN host that must start a dedicated '
     'server and THEN a local client cannot be expressed, and the title falls '
     'back to a bespoke launcher - which is the duplication this template '
     'exists to remove.'),
    ('%DISCDRV% IS IN SCOPE IN BOTH',
     'the hooks are only worth having if they can say "skip this step when no '
     'disc mounted" - that is what lets a box with NO mounter still host for '
     'the others. DISCDRV is undefined exactly when no disc was found, '
     'including after a failed mount fell through to :anydisc.'),
])
def test_template_keeps_safeguard(needle, why):
    assert needle in _template(), 'template lost a safeguard: %s (%s)' % (needle, why)


def test_template_checks_volume_label_before_marker():
    """The label is the strong test; the marker is the fallback.

    Getting this the other way round is what made Descent II 'find' a mounted
    StarCraft disc via AUTORUN.INF and start the game against it.
    """
    t = _template()
    # The LABEL, not the first `call :finddisc` - the call sites come earlier
    # in the file and finding one of those would make this test vacuous.
    i = t.index('\r\n:finddisc\r\n')
    body = t[i:t.index('\r\n:anydisc\r\n', i)]
    label_at = body.index('%VOLID%')
    marker_at = body.index('%MARKER%')
    assert label_at < marker_at, \
        'finddisc must test the VOLUME LABEL first and the marker only as a fallback'


# --------------------------------------------------------------------------
# 2. Every shipped launcher must still BE what its spec generates.
#
# WHAT THIS DID AND WHAT IT DOES NOW - the distinction matters.
#
# When the template was extracted (2026-08-31) this was a genuine round-trip:
# two INDEPENDENTLY hand-written launchers, both proven on hardware, regenerated
# byte-identical from one template. That is what showed the extraction had lost
# nothing and was not overfitted to the file it came from. The other eight were
# then brought onto the template the same way, each one diffed first so the only
# lines that changed were the three the fix touches.
#
# Those launchers are now GENERATED, so this test can no longer prove the
# template is faithful - that proof was a one-time event and is recorded in the
# commit. What it still does, and what it exists for now, is catch DRIFT: a
# hand-edit to a generated launcher, or a template change that was never shipped
# to the ten trees. Both are exactly how a fix lands in one launcher and not the
# others, which is the failure this whole mechanism was built to stop.
# --------------------------------------------------------------------------

SHIPPED = [
    ('RedFaction.json', 'RedFaction/Play Red Faction.bat'),
    ('SoldierOfFortune2.json', 'SoldierOfFortune2/Play Soldier of Fortune II.bat'),
    ('BF1942.json', 'BF1942/Play Battlefield 1942.bat'),
    ('Descent2.json', 'Descent2/Play Descent 2 - original Win95.bat'),
    ('MaxPayne.json', 'MaxPayne/Play Max Payne.bat'),
    ('Shogo.json', 'Shogo/Play Shogo.bat'),
    ('StarCraft.json', 'StarCraft/Play StarCraft.bat'),
    ('SystemShock2.json', 'SystemShock2/Play System Shock 2.bat'),
    ('JediAcademy.json', 'JediAcademy/Play Jedi Academy.bat'),
    ('JediAcademy-Multiplayer.json', 'JediAcademy/Play Jedi Academy - Multiplayer.bat'),
]


@pytest.mark.parametrize('spec,shipped', SHIPPED)
def test_shipped_launcher_matches_its_spec(spec, shipped):
    path = os.path.join(LIBRARY, shipped)
    if not os.path.exists(path):
        pytest.skip('SHARE NOT MOUNTED - cannot check %s against the template. '
                    'This test is what stops a fix landing in one launcher and '
                    'not the other nine; a silent pass here would let them drift.'
                    % shipped)
    r = subprocess.run([sys.executable, GEN, '--spec', os.path.join(SPECS, spec),
                        '--check', path], capture_output=True)
    assert r.returncode == 0, \
        ('%s is no longer what the template + spec generates. Either someone '
         'hand-edited a GENERATED launcher, or the template was changed and '
         'never re-shipped to the staged trees.\n%s%s'
         % (shipped, r.stdout.decode(), r.stderr.decode()))


# --------------------------------------------------------------------------
# 3. Every spec must be well formed, and must not reintroduce known traps.
# --------------------------------------------------------------------------

def _all_specs():
    return sorted(f for f in os.listdir(SPECS) if f.endswith('.json'))


def test_there_are_specs():
    assert _all_specs(), 'no launcher specs at all - the generator has nothing to check'


@pytest.mark.parametrize('name', _all_specs())
def test_spec_is_complete_and_generates(name):
    spec = _spec(name)
    for key in ('title', 'vars'):
        assert key in spec, '%s: missing %r' % (name, key)
    for var in ('GTITLE', 'IMAGE', 'VOLID', 'MARKER', 'GAME'):
        assert spec['vars'].get(var), '%s: vars.%s is empty' % (name, var)
    text = _generate(name)
    assert '@@' not in text, '%s: generated launcher still has placeholders' % name
    assert text.startswith('@echo off'), '%s: generated launcher lost its header' % name


def test_postlaunch_hook_reaches_the_generated_launcher():
    """A hook nobody can use is worse than no hook.

    The Serious Sam LAN host needs "start the dedicated server, wait, then start
    a local client ONLY if a disc mounted" - the conditional half is what lets
    a box with no mounter at all still host for the others. If the generator
    silently dropped `postlaunch`, that title would go back to a bespoke
    launcher and the duplication this template removes would come straight back.
    """
    import json as _json
    import tempfile
    spec = _spec('RedFaction.json')
    spec['postlaunch'] = ('ping -n 17 127.0.0.1 >nul\r\n'
                          'if defined DISCDRV start "" /D "%~dp0" "%~dp0client.exe"')
    with tempfile.NamedTemporaryFile('w', suffix='.json', delete=False) as f:
        _json.dump(spec, f)
        tmp = f.name
    out = subprocess.run([sys.executable, GEN, '--spec', tmp], capture_output=True)
    os.unlink(tmp)
    assert out.returncode == 0, out.stderr.decode()
    text = out.stdout.decode('latin-1')
    assert 'if defined DISCDRV start "" /D "%~dp0" "%~dp0client.exe"' in text, \
        'postlaunch did not reach the generated launcher'
    # and it must land AFTER the game is started, not before
    assert text.index('start "" /D "%~dp0" "%GAME%"') < text.index('client.exe'), \
        'postlaunch ran before the game started - that is a prelaunch'


def test_requiredisc_is_per_title_not_a_constant():
    """Descent 2 ships REQUIREDISC=0 and every other title ships 1.

    It was hard-coded to 1 in the first cut of the template, which would have
    made Descent 2 REFUSE TO LAUNCH on a box whose mount failed - a title that
    runs perfectly well without its disc. Caught by diffing the generated file
    against the shipped one rather than trusting the substitution.
    """
    vals = {n[:-5]: _spec(n)['vars'].get('REQUIREDISC') for n in _all_specs()}
    assert vals.get('Descent2') == '0', \
        'Descent 2 must keep REQUIREDISC=0 - it runs without its disc'
    assert set(vals.values()) >= {'0', '1'}, \
        'REQUIREDISC has collapsed to a constant; it is per title'
    for name, v in vals.items():
        assert v in ('0', '1'), '%s: REQUIREDISC=%r is not 0 or 1' % (name, v)


@pytest.mark.parametrize('name', _all_specs())
def test_spec_marker_is_not_a_generic_cd_file(name):
    """A MARKER must be unique to THIS disc.

    AUTORUN.INF / INSTALL.EXE / SETUP.EXE / AUTORUN.EXE are on essentially every
    game CD ever pressed. Using one made the Descent II launcher match a mounted
    StarCraft disc, skip its own mount, and start the game against it while
    reporting success.
    """
    marker = _spec(name)['vars']['MARKER'].upper()
    leaf = marker.replace('/', '\\').split('\\')[-1]
    assert leaf not in ('AUTORUN.INF', 'AUTORUN.EXE', 'INSTALL.EXE',
                        'SETUP.EXE', 'SETUP.INI', 'AUTORUN.INI'), \
        '%s: MARKER %r is on every game CD - pick a file unique to this disc' % (name, marker)


@pytest.mark.parametrize('name', _all_specs())
def test_spec_image_is_relocatable(name):
    """No absolute paths: a staged tree must run from wherever it lands."""
    image = _spec(name)['vars']['IMAGE']
    assert image.startswith('%~dp0'), \
        ('%s: IMAGE %r is not relative to %%~dp0. A staged title must relocate; '
         'C:\\Games\\<Title> is not guaranteed (.124 keeps games on two volumes).'
         % (name, image))
    assert ':' not in image.replace('%~dp0', ''), \
        '%s: IMAGE %r looks like an absolute path' % (name, image)


@pytest.mark.parametrize('name', _all_specs())
def test_spec_would_not_generate_an_unlaunchable_filename(name):
    """CLAUDE.md: a generated filename must never contain ( or ).

    `EXEC cmd /c start "" /D "<dir>" "Host X (LAN).bat"` loses its quoting by the
    time cmd parses it. A desktop double-click still works, so the defect is
    invisible to review and only bites automation.
    """
    r = subprocess.run([sys.executable, GEN, '--spec', os.path.join(SPECS, name),
                        '--out', '/tmp/Play Something (LAN).bat'], capture_output=True)
    assert r.returncode != 0, 'the generator accepted a filename containing parentheses'
    assert b'( or )' in r.stderr + r.stdout


if __name__ == '__main__':
    sys.exit(pytest.main([__file__, '-v']))
