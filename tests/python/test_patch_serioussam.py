"""Serious Sam TFE 1.00 / TSE 1.05: the Engine.dll resolution-table patch and
the Game_startup.ini include (provisioning/patches/serioussam/apply.py).

Pure logic runs anywhere. The share checks SKIP LOUDLY when the library is not
mounted, because a silent skip would let the staged originals drift unnoticed.
"""
import importlib.util
import os
import struct

import pytest

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..'))
APPLY = os.path.join(REPO, 'provisioning', 'patches', 'serioussam', 'apply.py')
LIB = '/mnt/retro-share/Files/Games-Library'

spec = importlib.util.spec_from_file_location('ssam_apply', APPLY)
ap = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ap)

TFE = ap.ENGINES['SeriousSamFirstEncounter']
REAL_PINNED = ap.pinned
TSE = ap.ENGINES['SeriousSamSecondEncounter']


def _skip_unless_share():
    if not os.path.isdir(LIB):
        pytest.skip('SHARE NOT MOUNTED: %s absent - the staged Serious Sam '
                    'originals were NOT checked' % LIB)


# ---------------------------------------------------------------------------
# Offset tables and the table transform
# ---------------------------------------------------------------------------
def test_offsets_are_the_verified_ones():
    assert [o for o, _a, _b in ap.edits_for(TFE)] == [0x159288, 0x159290, 0x159298]
    assert [o for o, _a, _b in ap.edits_for(TSE)] == [0x1ad760, 0x1ad768, 0x1ad770]
    assert len(TFE['table']) == 22 and TFE['count'] == 22
    assert len(TSE['table']) == 23 and (720, 576) in TSE['table']
    # TSE's loops end at the compiled pointer 0x601ad788 = table end
    assert TSE['image_base'] + TSE['table_off'] + 8 * 23 == 0x601ad788


@pytest.mark.parametrize('eng', [TFE, TSE])
def test_new_table_gains_16_9_and_loses_only_dualhead(eng):
    new = ap.new_table(eng['table'])
    assert len(new) == len(eng['table']), 'the count / end pointer must stay valid'
    for m in [(1920, 1080), (1600, 900), (1280, 720)]:
        assert m in new
        assert m not in eng['table']
    gone = set(eng['table']) - set(new)
    assert gone == {(1280, 480), (1600, 600), (2048, 768)}
    assert all(ap.is_dual_head(*p) for p in gone), 'only Matrox DualHead slots are sacrificed'
    assert len(set(new)) == len(new), 'no duplicate entries'
    # the engine's own helpers must not reclassify a new mode
    for p in set(new) - set(eng['table']):
        assert not ap.is_dual_head(*p)
        assert not ap.is_wide_screen(*p)
    # CDisplayMode::IsWideScreen is still exactly the one NTSC entry
    assert [p for p in new if ap.is_wide_screen(*p)] == [(856, 480)]
    # CDrawPort::IsWideScreen (w*9 == h*16) IS true for the new modes - a
    # runtime property of the render size that 1920x1080 already has today
    # (the launcher runs it there); recorded so nobody reads the patch as the
    # thing that turned it on.
    assert ap.is_drawport_wide(1920, 1080) and ap.is_drawport_wide(1280, 720)
    assert not any(ap.is_drawport_wide(*p) for p in eng['table'])


def _synthetic(eng):
    """A tiny valid PE32 whose .data holds the table at the engine's offset."""
    toff = 0x400
    n = len(eng['table'])
    size = 0x800
    b = bytearray(size)
    b[0:2] = b'MZ'
    struct.pack_into('<I', b, 0x3c, 0x80)
    b[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HH', b, 0x84, 0x14c, 1)          # machine, 1 section
    struct.pack_into('<H', b, 0x80 + 20, 224)           # SizeOfOptionalHeader
    opt = 0x98
    struct.pack_into('<H', b, opt, 0x10b)
    struct.pack_into('<I', b, opt + 28, 0x10000000)     # ImageBase
    struct.pack_into('<I', b, opt + 92, 16)             # NumberOfRvaAndSizes
    sec = opt + 224
    struct.pack_into('<8sIIII', b, sec, b'.data', 0x400, 0x1000, 0x400, 0x400)
    for i, (w, h) in enumerate(eng['table']):
        struct.pack_into('<ii', b, toff + 8 * i, w, h)
    fake = dict(eng, table_off=toff, image_base=0x10000000, count_off=None,
                count=None, table_refs={})
    csum = ap.pe_checksum(bytes(b), opt + 64)
    struct.pack_into('<I', b, opt + 64, csum)
    return bytes(b), fake


@pytest.mark.parametrize('eng', [TFE, TSE])
def test_patch_on_synthetic_image_changes_only_the_three_slots(eng):
    b, fake = _synthetic(eng)
    out = ap.patch_engine(b, fake)
    assert len(out) == len(b)
    got = ap.read_table(out, fake['table_off'], len(eng['table']))
    assert [tuple(p) for p in got] == ap.new_table(eng['table'])
    h = ap.pe_headers(b)
    allowed = [(o, o + 8) for o, _x, _y in ap.edits_for(fake)] + \
              [(h['checksum_off'], h['checksum_off'] + 4)]
    for s, e in ap.diff_ranges(b, out):
        assert any(x <= s and e <= y for x, y in allowed), (s, e)
    # a valid checksum stays valid
    assert struct.unpack_from('<I', out, h['checksum_off'])[0] == \
        ap.pe_checksum(out, h['checksum_off'])


def test_patch_refuses_a_table_that_is_not_the_original():
    b, fake = _synthetic(TFE)
    bb = bytearray(b)
    struct.pack_into('<ii', bb, fake['table_off'] + 8 * 17, 1280, 720)  # already altered
    with pytest.raises(ValueError):
        ap.patch_engine(bytes(bb), fake)


def test_pe_checksum_ignores_its_own_field():
    b, _f = _synthetic(TFE)
    h = ap.pe_headers(b)
    bb = bytearray(b)
    struct.pack_into('<I', bb, h['checksum_off'], 0xdeadbeef)
    assert ap.pe_checksum(bytes(bb), h['checksum_off']) == ap.pe_checksum(b, h['checksum_off'])


# ---------------------------------------------------------------------------
# Game_startup.ini
# ---------------------------------------------------------------------------
TSE_SHIPPED = (b'sam_strFirstLevel = "Levels\\\\LevelsMP\\\\1_0_InTheLastEpisode.wld";\r\n'
               b'sam_strIntroLevel = "Levels\\\\LevelsMP\\\\Intro.wld";\r\n'
               b'sam_strTechTestLevel = "Levels\\\\LevelsMP\\\\Technology\\\\TechTest.wld";\r\n'
               b'sam_strGameName = "serioussamse";\r\n')


def test_startup_keeps_every_shipped_byte_and_adds_one_include():
    out = ap.patch_startup(TSE_SHIPPED)
    assert out.startswith(TSE_SHIPPED), 'the TSE level/gamename lines must survive'
    assert out.count(b'include "Scripts\\Game_FleetRes.ini";') == 1
    assert out.endswith(b'include "Scripts\\Game_FleetRes.ini";\r\n')
    assert b'\n' not in out.replace(b'\r\n', b''), 'CRLF only'
    ap.check_startup(out, ap.STARTUP['SeriousSamSecondEncounter'], want_md5=False)


def test_startup_is_idempotent_and_handles_a_missing_final_newline():
    once = ap.patch_startup(TSE_SHIPPED)
    assert ap.patch_startup(once) == once
    bare = TSE_SHIPPED.rstrip(b'\r\n')
    assert ap.patch_startup(bare).startswith(bare + b'\r\n')


def test_include_uses_croteams_own_form():
    """Croteam's shipped scripts write `include "Scripts\\GLSettings\\X.ini";`
    - SINGLE backslashes, a trailing semicolon - and every one of them follows
    a backslash with a letter that is proven not to be an escape. The per-box
    file's name starts with G for exactly that reason."""
    assert ap.INCLUDE_LINE == 'include "Scripts\\Game_FleetRes.ini";'
    assert '\\\\' not in ap.INCLUDE_LINE
    assert ap.FLEETRES_INI.split('\\')[1][0] == 'G'


def test_startup_check_catches_the_truncating_writer():
    """What the launcher/GAMERES wrote before the fix - the four TSE lines gone."""
    truncated = (b'// written by the launcher at every start - do not edit\r\n'
                 b'sam_bFullScreen=1;\r\nsam_iScreenSizeI=1920;\r\nsam_iScreenSizeJ=1080;\r\n')
    with pytest.raises(ValueError):
        ap.check_startup(truncated, ap.STARTUP['SeriousSamSecondEncounter'], want_md5=False)


def test_include_block_is_title_neutral_and_gives_the_scanner_nothing_to_trip_on():
    """The SAME block is appended to TFE's file, which has no level lines - an
    earlier draft told TFE readers about 'the TSE lines above'. The comment
    lines are read by the engine's scanner, so they carry no backslash, no
    quote and no parenthesis; the include line is the only non-comment."""
    lines = [l for l in ap.INCLUDE_BLOCK if l]
    assert lines[-1] == ap.INCLUDE_LINE
    for l in lines[:-1]:
        assert l.startswith('//'), l
        assert not any(c in l for c in '\\"()'), l
    text = '\n'.join(ap.INCLUDE_BLOCK)
    assert 'TSE lines above' not in text
    assert ap.INCLUDE_LINE not in '\n'.join(lines[:-1]), 'idempotency keys on the include line'


# ---------------------------------------------------------------------------
# Share side
# ---------------------------------------------------------------------------
@pytest.mark.parametrize('title', sorted(ap.ENGINES))
def test_staged_engine_builds_to_the_pinned_md5(title):
    """Before --publish the original is the library copy; after it, the backup
    --publish made. Either way it must rebuild to the pinned bytes - a patched
    library copy with no verified backup FAILS here rather than skipping."""
    _skip_unless_share()
    eng = ap.ENGINES[title]
    orig, _where = ap.read_original(LIB, title, 'engine', eng['rel'])
    ap.check_engine(orig, eng)
    _o, new, _rep = ap.build_one(LIB, title, 'engine', eng['rel'])
    assert ap.md5b(new) == eng['patched_md5']
    assert len(new) == eng['size']


@pytest.mark.parametrize('title', sorted(ap.STARTUP))
def test_staged_startup_builds_to_the_pinned_md5(title):
    _skip_unless_share()
    s = ap.STARTUP[title]
    orig, new, _rep = ap.build_one(LIB, title, 'startup', ap.STARTUP_REL)
    assert len(orig) == s['size'] and ap.md5b(orig) == s['md5']
    assert ap.md5b(new) == s['patched_md5']
    assert new.startswith(orig), 'every shipped byte survives'


@pytest.mark.parametrize('title', sorted(ap.ENGINES))
def test_per_box_fleetres_ini_is_not_staged(title):
    """Staged, GAMESYNC would copy it over the box's copy at every sync and
    GAMERES would rewrite it again - the churn that kills '0 value(s) changed'."""
    _skip_unless_share()
    d = os.path.join(LIB, title, 'Scripts')
    assert not any(n.lower() == 'game_fleetres.ini' for n in os.listdir(d))


# ---------------------------------------------------------------------------
# --publish ordering (sharewrite is stubbed - nothing touches the share)
# ---------------------------------------------------------------------------
def _manifest(tmp_path, n=2):
    files = []
    for i in range(n):
        orig, new = b'orig%d' % i, b'new%d' % i
        po, pn = tmp_path / ('o%d' % i), tmp_path / ('n%d' % i)
        po.write_bytes(orig)
        pn.write_bytes(new)
        t = 'T%d' % i
        files.append(dict(title=t, kind='engine', share_path='Files/Games-Library/%s/f' % t,
                          backup_share_path='Files/Games-Library/_patches/%s/b/f' % t,
                          local_path=str(pn), original_local_path=str(po),
                          md5=ap.md5b(new), original_md5=ap.md5b(orig)))
    (tmp_path / 'manifest.json').write_text(__import__('json').dumps({'files': files}))
    return files


def _pin(monkeypatch, files):
    """Make the synthetic entries count as the reviewed pins."""
    pins = {(f['title'], f['kind']): (f['original_md5'], f['md5']) for f in files}
    monkeypatch.setattr(ap, 'pinned', lambda t, k: pins.get((t, k)))


def test_publish_backs_up_first_then_puts_and_is_idempotent(tmp_path, monkeypatch):
    files = _manifest(tmp_path)
    _pin(monkeypatch, files)
    share = {f['share_path']: f['original_md5'] for f in files}
    calls = []

    def put(local, dest):
        calls.append(dest)
        share[dest] = ap.md5b(open(local, 'rb').read())
        return True
    monkeypatch.setattr(ap, 'put', put)
    monkeypatch.setattr(ap, 'share_md5', lambda lib, p: share.get(p))
    assert ap.cmd_publish('/lib', str(tmp_path)) == 0
    assert calls == [files[0]['backup_share_path'], files[0]['share_path'],
                     files[1]['backup_share_path'], files[1]['share_path']]
    calls.clear()
    assert ap.cmd_publish('/lib', str(tmp_path)) == 0
    assert calls == [], 'a second publish must write nothing'


def test_publish_refuses_a_manifest_that_is_not_the_reviewed_pin(tmp_path, monkeypatch):
    """An OUTDIR built by an edited script (or a hand-edited manifest) must not
    reach the share - and the refusal happens before the FIRST write, not
    after half the files went out."""
    files = _manifest(tmp_path)
    _pin(monkeypatch, files[:1])             # the second entry has no pin
    calls = []
    monkeypatch.setattr(ap, 'put', lambda l, d: calls.append(d) or True)
    monkeypatch.setattr(ap, 'share_md5', lambda lib, p: None)
    assert ap.cmd_publish('/lib', str(tmp_path)) == 2
    assert calls == [], 'nothing may be written when any entry is off-pin'
    # and the real pins are what the shipped script carries
    assert REAL_PINNED('SeriousSamSecondEncounter', 'startup') == \
        (ap.STARTUP['SeriousSamSecondEncounter']['md5'],
         ap.STARTUP['SeriousSamSecondEncounter']['patched_md5'])
    assert REAL_PINNED('NoSuchTitle', 'engine') is None


def test_publish_stops_on_first_failure_and_on_unknown_share_state(tmp_path, monkeypatch):
    files = _manifest(tmp_path)
    _pin(monkeypatch, files)
    share = {f['share_path']: f['original_md5'] for f in files}
    calls = []
    monkeypatch.setattr(ap, 'share_md5', lambda lib, p: share.get(p))
    monkeypatch.setattr(ap, 'put', lambda l, d: calls.append(d) or False)
    assert ap.cmd_publish('/lib', str(tmp_path)) != 0
    assert calls == [files[0]['backup_share_path']], 'nothing after the first failure'
    calls.clear()
    share[files[0]['share_path']] = 'something-else'
    monkeypatch.setattr(ap, 'put', lambda l, d: calls.append(d) or True)
    assert ap.cmd_publish('/lib', str(tmp_path)) != 0
    assert calls == [], 'a share copy that is neither original nor patch is never overwritten'


# ---------------------------------------------------------------------------
# Reproducible AFTER --publish: the original then lives in the backup
# ---------------------------------------------------------------------------
TSE_TITLE = 'SeriousSamSecondEncounter'


def _fake_library(tmp_path, with_backup=True, backup=TSE_SHIPPED):
    lib = tmp_path / 'lib'
    live = lib / TSE_TITLE / 'Scripts' / 'Game_startup.ini'
    live.parent.mkdir(parents=True)
    live.write_bytes(ap.patch_startup(TSE_SHIPPED))          # already published
    if with_backup:
        bp = ap.backup_path(str(lib), TSE_TITLE, ap.STARTUP_REL)
        os.makedirs(os.path.dirname(bp))
        with open(bp, 'wb') as f:
            f.write(backup)
    return str(lib)


def test_build_reads_the_backup_once_the_library_is_patched(tmp_path, capsys):
    lib = _fake_library(tmp_path)
    orig, new, rep = ap.build_one(lib, TSE_TITLE, 'startup', ap.STARTUP_REL)
    assert orig == TSE_SHIPPED
    assert ap.md5b(new) == ap.STARTUP[TSE_TITLE]['patched_md5']
    assert any('ALREADY PATCHED' in r for r in rep), 'the report must say where the original came from'
    # --check: the three files this fake library lacks FAIL, loudly; the TSE
    # startup script is rebuilt from its backup and matches the pin.
    assert ap.cmd_check(lib) == 1
    out = capsys.readouterr().out
    assert out.count('FAIL:') == 3
    tse = out.split('== %s %s (startup)' % (TSE_TITLE, ap.STARTUP_REL))[1]
    assert 'original read from the backup' in tse.split('==')[0]
    assert '= the pinned md5' in tse.split('==')[0]


def test_a_patched_library_with_no_original_left_fails_loudly(tmp_path):
    with pytest.raises(ValueError, match='can no longer be rebuilt'):
        ap.build_one(_fake_library(tmp_path, with_backup=False), TSE_TITLE, 'startup', ap.STARTUP_REL)
    with pytest.raises(ValueError, match='not the original'):
        ap.build_one(_fake_library(tmp_path / 'b', backup=b'something else'),
                     TSE_TITLE, 'startup', ap.STARTUP_REL)


def test_build_refuses_an_outdir_inside_a_git_tree(tmp_path):
    """Patched game binaries must never land where `git add` can reach them."""
    repo = tmp_path / 'repo'
    (repo / '.git').mkdir(parents=True)
    out = repo / 'deep' / 'out'
    assert ap.cmd_build('/nonexistent-library', str(out)) == 2
    assert not out.exists(), 'refused before writing anything'
    # the detector sees this very repo (a worktree's .git is a FILE)
    assert ap.git_work_tree(os.path.join(REPO, 'provisioning', 'x')) is not None
    assert ap.git_work_tree(str(tmp_path / 'elsewhere')) is None


# ---------------------------------------------------------------------------
# --install-server: never rewrite a DLL a running Wine server has mapped
# ---------------------------------------------------------------------------
def test_install_server_replaces_by_rename_and_keeps_a_backup(tmp_path, monkeypatch):
    files = _manifest(tmp_path, n=1)
    _pin(monkeypatch, files)
    srv = tmp_path / 'srv'
    dll = srv / 'Bin' / 'Engine.dll'
    dll.parent.mkdir(parents=True)
    dll.write_bytes(b'orig0')
    monkeypatch.setattr(ap, 'SERVERS', {'T0': (str(srv), 'ssam-t0-server')})
    held = open(dll, 'rb')                   # what a running server has mapped
    ino = os.stat(dll).st_ino
    try:
        assert ap.cmd_install_server(str(tmp_path)) == 0
        assert dll.read_bytes() == b'new0'
        assert os.stat(dll).st_ino != ino, 'replaced by rename, not rewritten in place'
        assert held.read() == b'orig0', 'the old inode is untouched for the live process'
    finally:
        held.close()
    assert (srv / 'Bin' / 'Engine.dll.orig-2026-09-29').read_bytes() == b'orig0'
    assert not [p for p in os.listdir(dll.parent) if '.new-' in p], 'no temp file left'
    assert ap.cmd_install_server(str(tmp_path)) == 0          # idempotent: skip
    dll.write_bytes(b'someone else')
    assert ap.cmd_install_server(str(tmp_path)) == 2
    assert dll.read_bytes() == b'someone else', 'unknown content is never replaced'
