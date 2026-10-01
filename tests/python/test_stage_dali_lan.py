"""scripts/dosgames/stage_dali_lan.py - the Win9x box's real-DOS LAN launchers.

Proven 2026-10-01: the Win98 build VM in real DOS joined .123's DOSBox Descent
game through DALI. These tests pin what made that work and the traps around it.
"""
import hashlib
import os
import re
import sys

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(REPO, 'scripts', 'dosgames'))

import stage_dali_lan as s  # noqa: E402
from stage_win9x_dos import RD_HOOK  # noqa: E402



def bats(title):
    t = s.TITLES[title]
    return {t['bat']: s.windows_bat(t), 'RDLAN.BAT': s.rdlan_bat(t)}


@pytest.mark.parametrize('title', sorted(s.TITLES))
def test_command_com_dialect(title):
    for name, data in bats(title).items():
        assert re.fullmatch(r'[A-Z0-9_]{1,8}\.BAT', name), name
        assert data.endswith(b'\r\n') and b'\n' not in data.replace(b'\r\n', b''), name
        for line in data.decode('ascii').split('\r\n'):
            assert len(line) <= 126, (name, line)
            assert '%~' not in line and 'cd /d' not in line.lower(), (name, line)
            assert 'start ""' not in line, (name, line)
            assert '(' not in line and ')' not in line, (name, line)
            if line.lower().startswith('rem'):
                assert not re.search(r'[<>|]', line), (name, line)


@pytest.mark.parametrize('title', sorted(s.TITLES))
def test_desktop_launcher_ends_with_cls_and_restarts_with_force(title):
    t = s.TITLES[title]
    lines = s.windows_bat(t).decode('ascii').split('\r\n')
    assert lines[-2] == 'cls'                      # a Win9x DOS box closes only when empty
    assert 'rundll32.exe shell32.dll,SHExitWindowsEx 6' in lines
    assert 'echo call C:\\GAMES\\%s\\RDLAN.BAT> C:\\RUNDOS\\NEXT.BAT' % t['dir'] in lines


@pytest.mark.parametrize('title', sorted(s.TITLES))
def test_real_dos_copy_is_never_the_source_tree(title):
    """The VM's GamesDir is C:\\Games: C:\\GAMES\\<title> would BE the source and
    xcopy refuses a self-copy - the real-DOS folder must be another name."""
    t = s.TITLES[title]
    assert t['dir'].upper() != title.upper()
    assert re.fullmatch(r'[A-Z0-9_]{1,8}', t['dir'])


def test_packet_drivers_each_get_their_own_vector():
    vecs = [v for _, v, _ in s.PKT_CANDIDATES]
    assert len(vecs) == len(set(vecs))             # PLAY.BAT's hard-won rule
    assert s.PKT_CANDIDATES[0][0] == '3C509.COM'    # .243's EtherLink III first
    # the VM's measured PCI card BEFORE any ISA guess: NE2000 at 0x300 loads on
    # a phantom in 86Box and DHCP then fails (measured 2026-10-01)
    assert s.PKT_CANDIDATES[1] == ('NE2000.COM', '0x61', '10 0xE000')
    guesses = [i for i, c in enumerate(s.PKT_CANDIDATES) if c[2].endswith(('0x300', '0x280'))]
    assert guesses and min(guesses) > 1


@pytest.mark.parametrize('title', sorted(s.TITLES))
def test_every_path_with_a_packet_driver_reboots(title):
    """A Crynwr packet driver cannot unload: once one loaded, the batch must
    end in WBOOT (never fall through into Windows). Only 'no card' may."""
    txt = s.rdlan_bat(s.TITLES[title]).decode('ascii')
    after_pkt = txt[txt.index(':pkt'):]
    assert 'goto done' not in after_pkt
    assert txt.index('DALI /u') < txt.index(':boot') < txt.index('WBOOT') < txt.index(':done')
    assert txt.index('IPXCHK > nul') < txt.index(':play')
    assert 'DALI %HOSTIP% 213' in txt
    # ASKIP writes HOSTIP.BAT itself: DOS line input ECHOES to stdout, so a
    # redirect captured the keystrokes into the batch (VM, 2026-10-01)
    ask = [l for l in txt.split('\r\n') if l.startswith('ASKIP ')]
    assert ask == ['ASKIP C:\\GAMES\\LANHOST.TXT C:\\RUNDOS\\HOSTIP.BAT'], ask


def test_rundos_hook_is_the_shared_one():
    assert s.title_files.__module__ == 'stage_dali_lan'
    t = sorted(s.TITLES)[0]
    assert s.title_files(t)['RDHOOK.TXT'] == RD_HOOK.encode('ascii')


def test_kit_matches_its_pinned_md5s():
    kit = s.KIT
    pinned = {}
    for line in open(os.path.join(kit, 'MD5SUMS.txt')):
        md5, name = line.split()
        pinned[name] = md5
    for f in s.KIT_FILES:
        data = open(os.path.join(kit, f), 'rb').read()
        assert hashlib.md5(data).hexdigest() == pinned[f], f


def test_launch_row_is_legal():
    for title in s.TITLES:
        row = s.launch_row(title)
        target, label, icon = row.split('\t')
        assert not re.search(r'[\\/:*?"<>|()]', label), label
        assert len(row.encode('ascii')) < 1023
