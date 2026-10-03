"""Descent II's original Win95 engine stops at an Electronic Registration Card.

DESCENTW.EXE starts <disc>:\\D2DATA\\REGCARD.EXE at EVERY launch (CreateProcessA,
then it waits for the card to exit). REGCARD.EXE reads

    GetPrivateProfileIntA("Registration Counters", "Times Bypassed", 0, "EREGREG.INI")

- a bare file name, so %windir%\\EREGREG.INI - and shows its full-screen form
unless the counter is 3 or more (`cmp eax, 3 / jae`). Closing the card does not
count as a bypass: .124's file read `Times Bypassed=0`, `Times Run=1` after a
sweep closed it. Measured 2026-10-03 on .124: with the counter set to 3 by hand,
the shortcut went straight to the Interplay intro, no dialog
(voodoo-cleanroom/vcr-kmd/evidence/gametune_1001/descent2_regcard/).

The fix is the generated mount launcher's prelaunch step, which sets that
counter on the box before the game starts (provisioning/discmount/specs/
Descent2.json). These tests pin it, and - where the share is mounted - ask the
disc's own REGCARD.EXE that the counter and threshold are still the ones the
launcher writes.
"""
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

import pytest

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = os.path.join(REPO, 'provisioning', 'discmount', 'specs', 'Descent2.json')
GEN = os.path.join(REPO, 'scripts', 'fleet', 'make-mount-launcher.py')
ISO = '/mnt/retro-share/Files/Games-Library/Descent2/d2disc.iso'


def _spec():
    with open(SPEC) as f:
        return json.load(f)


def _prelaunch_lines():
    return [l for l in _spec()['prelaunch'].split('\r\n')]


def _generated():
    r = subprocess.run([sys.executable, GEN, '--spec', SPEC], capture_output=True, check=True)
    return r.stdout.decode('latin-1')


def test_the_launcher_sets_the_cards_own_bypass_counter_before_the_game():
    text = _generated()
    lines = text.replace('\r\n', '\n').split('\n')
    start = next(i for i, l in enumerate(lines) if l.startswith('start "" /D "%~dp0" "%GAME%"'))
    pre = lines[:start]
    # the counter goes into the file REGCARD.EXE reads, under its own section
    assert 'set "D2INI=%windir%\\EREGREG.INI"' in pre
    assert 'if not defined D2REGOK >"%D2INI%" echo [Registration Counters]' in pre
    assert 'if not defined D2REGOK >>"%D2INI%" echo Times Bypassed=3' in pre
    # only when it is not already 3+ (a settled box rewrites nothing)
    check = 'findstr /i /r /c:"^Times Bypassed=[3-9]" "%D2INI%" >nul 2>&1 && set "D2REGOK=1"'
    assert pre.count(check) == 2          # before the write, and the read-back after it
    assert pre.index(check) < pre.index('if not defined D2REGOK >"%D2INI%" echo [Registration Counters]')


def test_the_counter_line_cannot_turn_into_a_stream_redirect():
    # `echo Times Bypassed=3>>file` would redirect STREAM 3 and write a bare
    # "Times Bypassed=" - the redirection goes first on every write line.
    for l in _prelaunch_lines():
        if 'echo Times Bypassed=' in l:
            assert l.rstrip().endswith('echo Times Bypassed=3'), l
            assert '3>' not in l, l


def test_vista_and_later_also_get_the_virtualstore_copy():
    # Under UAC a legacy exe's %windir% INI read is redirected to the
    # VirtualStore when a copy exists there, and cmd cannot write %windir%.
    pre = _prelaunch_lines()
    assert 'if defined LOCALAPPDATA set "D2VS=%LOCALAPPDATA%\\VirtualStore\\Windows"' in pre
    assert 'if defined D2VS if not defined D2VSOK >>"%D2VS%\\EREGREG.INI" echo Times Bypassed=3' in pre


def test_a_failed_write_says_so():
    pre = _prelaunch_lines()
    assert any(l.startswith('if not defined D2REGOK echo [%GTITLE%] REGISTRATION CARD NOT SUPPRESSED')
               for l in pre)


def test_no_block_can_be_closed_early_by_an_expanded_value():
    # CLAUDE.md: a ')' in an expanded value closes a ( ... ) block. The step
    # uses single-line IFs only, so there is no block to close.
    for l in _prelaunch_lines():
        if l.startswith('rem '):
            continue
        assert '(' not in l and ')' not in l, l


def _regcard_bytes():
    if not os.path.isfile(ISO):
        pytest.skip('SHARE NOT MOUNTED - cannot ask the disc\'s REGCARD.EXE which counter '
                    'it reads; the launcher could be writing the wrong key and this would '
                    'not notice')
    if not shutil.which('7z'):
        pytest.skip('7z not installed - cannot read REGCARD.EXE out of the disc image')
    with tempfile.TemporaryDirectory() as d:
        subprocess.run(['7z', 'e', '-y', '-o' + d, ISO, 'D2DATA/REGCARD.EXE'],
                       capture_output=True, check=True)
        with open(os.path.join(d, 'REGCARD.EXE'), 'rb') as f:
            return f.read()


def test_the_discs_regcard_reads_the_counter_the_launcher_writes():
    pefile = pytest.importorskip('pefile')
    exe = _regcard_bytes()
    pe = pefile.PE(data=exe)
    base = pe.OPTIONAL_HEADER.ImageBase
    iat = {imp.address: imp.name.decode() for e in pe.DIRECTORY_ENTRY_IMPORT
           for imp in e.imports if imp.name}

    def cstr(va):
        off = pe.get_offset_from_rva(va - base)
        return exe[off:exe.index(b'\0', off)].decode('latin-1')

    # push <file> / push 0 / push <key> / push <section> /
    # call [GetPrivateProfileIntA] / cmp eax, 3 / jae <skip the card>
    m = re.search(rb'\x68(....)\x6a\x00\x68(....)\x68(....)\xff\x15(....)\x83\xf8(.)\x73',
                  exe, re.S)
    assert m, 'REGCARD.EXE no longer has the counter test the launcher is written against'
    f, key, sect, call, thr = (struct.unpack('<I', g)[0] if len(g) == 4 else g[0]
                               for g in m.groups())
    assert iat.get(call) == 'GetPrivateProfileIntA'
    assert (cstr(sect), cstr(key), cstr(f)) == ('Registration Counters', 'Times Bypassed',
                                                 'EREGREG.INI')
    assert thr == 3          # the launcher writes exactly 3
