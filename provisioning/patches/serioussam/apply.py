#!/usr/bin/env python3
r"""Serious Sam TFE 1.00 / TSE 1.05 - put 1920x1080 in the in-game RESOLUTION list.

    python3 provisioning/patches/serioussam/apply.py --check
    python3 provisioning/patches/serioussam/apply.py --build ~/.retro-fleet/patch-out/serioussam
    python3 provisioning/patches/serioussam/apply.py --publish OUTDIR [--only engine|startup]   # FUTURE
    python3 provisioning/patches/serioussam/apply.py --install-server OUTDIR                     # FUTURE, not needed

WHAT THE MENU LISTS, AND WHY 1920x1080 IS NOT IN IT
---------------------------------------------------
Serious Engine 1 does not ask the driver which modes exist. `Bin\Engine.dll`
carries a hardcoded table `_areResolutions` of int32 (w, h) pairs, and the
mode-enumeration loop offers only the entries of that table the driver accepts
(OpenGL: ChangeDisplaySettings CDS_TEST per entry; TSE's Direct3D 8 loop keeps
an EnumAdapterModes mode only if it EQUALS a table entry). The table has no
16:9 entry at all except the 848x480/856x480 NTSC modes, so on a 1080p LCD the
menu cannot offer the mode the launcher already runs the game at - and it opens
showing 640x480, so APPLY from that screen drops the box to 640x480.

    TFE 1.00  Engine.dll 1,560,576 B  md5 6281c69ec427b9a0dd828bc1cbf0b14b
              table @ file 0x159200 (.data, VA 0x60219200), 22 pairs,
              count stored as DATA at 0x1592b0 (= 22)
    TSE 1.05  Engine.dll 1,929,274 B  md5 230c1b52f3bd125dc9af049b70472100
              table @ file 0x1ad6d0 (.data, VA 0x601ad6d0), 23 pairs (adds
              720x576), both loops end at the compiled pointer 0x601ad788

THE PATCH (same size, data only)
--------------------------------
The three Matrox DualHead entries (8:3, a two-monitor spanning desktop) are
the only ones no current fleet box can pass - no box's published inventory
record names a Matrox adapter (102B, checked 2026-09-29) - so they are
overwritten in place and the count / end pointer stay exactly as they are.
(Cards move between boxes here: a Matrox G400/G450/G550 in DualHead mode
would lose exactly these three menu entries, nothing else.)

    1280x480  -> 1280x720        TFE 0x159288   TSE 0x1ad760
    1600x600  -> 1600x900        TFE 0x159290   TSE 0x1ad768
    2048x768  -> 1920x1080       TFE 0x159298   TSE 0x1ad770

Checked, not assumed (see check_engine()):
  * the display-MODE helpers are not tripped: CDisplayMode::IsDualHead is
    w*3 == h*8 (TFE 0x600fc1c0, TSE 0x6003e600) and CDisplayMode::IsWideScreen
    is exactly 856x480 (TFE 0x600fc1e0, TSE 0x6003e620) - none of the three new
    pairs matches either. NOTE the DRAWPORT helper is different:
    CDrawPort::IsWideScreen (TFE 0x600fc610, TSE 0x6003ed60) is w*9 == h*16, so
    it IS true for all three new modes - but it is a property of the size the
    game renders at, and every 1080p box already renders at 1920x1080 today
    (the launcher sets it). The table patch changes what the MENU offers, not
    how a given resolution renders;
  * every relocated pointer into the table addresses its BASE (or the TFE count
    word / TSE end pointer), never an individual entry, so no code indexes the
    three slots directly; and there is no base relocation inside the patched
    bytes;
  * the PE header checksum was VALID in both originals, so it is recomputed and
    stays valid (Windows ignores it for a user-mode DLL; keeping it right costs
    four bytes and removes a difference someone would otherwise chase).

Multiplayer: Engine.dll is bound by the PE loader through SeriousSam.exe's /
DedicatedServer.exe's import table. Serious Engine's CRC challenge
(CNetworkLibrary::InitCRCGather/FinishCRCGather, CRCT_AddFile_t) covers files
opened through the engine's own file system during a level load - the world,
entity classes (.ecl) and their components - and never Bin\*.dll. The fleet's
ssam-tfe/tse servers run their own copies of the ORIGINAL Engine.dll (md5s
6281c69e... / 230c1b52..., identical to the library); the TFE server log
already shows a client join passing that challenge ("CRC check OK"). A
dedicated server never enumerates display modes, so the table is dead data
there. --install-server exists only as the belt-and-braces fallback if a
hardware join ever says otherwise; it replaces the DLL by rename (new inode),
so a running Wine server keeps its mapped copy until the unit restarts.

THE STARTUP SCRIPT (the TSE must-fix, same tool so it is one reproducible set)
-------------------------------------------------------------------------------
The launchers and GAMERES used to REPLACE Scripts\Game_startup.ini, which in
TSE deletes the four shipped lines (sam_strFirstLevel, sam_strIntroLevel,
sam_strTechTestLevel, sam_strGameName="serioussamse") - NEW GAME then fails on
'Levels\01_Hatshepsut.wld' (proven on .123). The staged file is therefore
rebuilt as SHIPPED + one include of a per-box file the writers own:

    include "Scripts\Game_FleetRes.ini";

The form is Croteam's own (Scripts\GLSettings\*.ini: `include "Scripts\GLSettings\Default.ini";`
- single backslashes, trailing semicolon, nested three deep in the shipped
scripts). The scanner's INCLUDE state takes the name raw up to the closing
quote (no escape processing), so the backslash is not an escape; the name
still starts with G only as belt-and-braces, because `\G` is the exact form
the shipped files prove. Game_FleetRes.ini itself must NEVER be staged.

Checked statically (review 2026-09-29), not assumed:
  * ORDER: CGame::Initialize includes PersistentSymbols.ini (client only) and
    THEN, unconditionally, Game_startup.ini (TSE GameMP.dll 0x608f2565 then
    0x608f2602; TFE Game.dll 0x604b0286 then 0x604b0323) - so Game_FleetRes.ini runs
    last and beats the persisted mode. That matters in TSE, where
    sam_strIntroLevel and sam_strGameName are PERSISTENT: every box that ran
    with the truncated script has saved TFE's defaults into its
    PersistentSymbols.ini, and the restored shipped lines override them.
  * A MISSING Game_FleetRes.ini is not fatal: the include's catch calls
    CShell::ErrorF (TSE 0x6013c2d8, TFE 0x601d2f88), which only CPrintFs, and
    the scanner carries on (BEGIN(INITIAL)).
  * An UNDECLARED identifier does not stop the lines after it - proven live:
    the dev-host TSE server's DedicatedServer.exe logs
    "Game_startup.ini(3): Identifier 'sam_strTechTestLevel' is not declared"
    at every start and still reports gamename 'serioussamse', which only
    line 4 of that file sets. DedicatedServer.exe runs Game_startup.ini too,
    so the box-hosted LAN server (Host launcher) also gets its TSE game name
    back from this fix.
This only takes effect together with the writer changes (launcher specs +
gameres.h) - see the build notes.

Outputs go OUTSIDE git (~/.retro-fleet/patch-out/serioussam/); game binaries
are never committed, and --build refuses an OUTDIR inside a git work tree.
"""
import argparse
import hashlib
import json
import os
import shutil
import struct
import subprocess
import sys

LIB = '/mnt/retro-share/Files/Games-Library'
SHARE_PREFIX = 'Files/Games-Library'
BACKUP_DIR = '_patches/%s/originals-2026-09-29'
HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, '..', '..', '..'))
SHAREWRITE = os.path.join(REPO, 'scripts', 'fleet', 'sharewrite.py')

# ---------------------------------------------------------------------------
# The two engines. Every number here was read from the staged originals.
# ---------------------------------------------------------------------------
TFE_TABLE = [(320, 240), (400, 300), (480, 360), (512, 384), (640, 480),
             (720, 540), (800, 600), (960, 720), (1024, 768), (1152, 864),
             (1280, 960), (1280, 1024), (1600, 1200), (1792, 1344),
             (1856, 1392), (1920, 1440), (2048, 1536),
             (1280, 480), (1600, 600), (2048, 768),        # Matrox DualHead
             (848, 480), (856, 480)]
TSE_TABLE = TFE_TABLE[:6] + [(720, 576)] + TFE_TABLE[6:]

# (old pair) -> (new pair), applied at the index where the old pair sits
REPLACE = [((1280, 480), (1280, 720)),
           ((1600, 600), (1600, 900)),
           ((2048, 768), (1920, 1080))]

ENGINES = {
    'SeriousSamFirstEncounter': dict(
        rel='Bin/Engine.dll', size=1560576,
        md5='6281c69ec427b9a0dd828bc1cbf0b14b',
        image_base=0x600c0000, table_off=0x159200, table=TFE_TABLE,
        # the count is a data word right after the table - left untouched
        count_off=0x1592b0, count=22,
        # every relocated pointer into [table-8, table_end+8) and its target
        table_refs={0x3ad68: 0x602192b4, 0x3ad7b: 0x602192b0,
                    0x3ad9a: 0x60219200, 0x3ae30: 0x602192b0},
        patched_md5='7158150f6ba8981f6c4d141e0a379f58'),
    'SeriousSamSecondEncounter': dict(
        rel='Bin/Engine.dll', size=1929274,
        md5='230c1b52f3bd125dc9af049b70472100',
        image_base=0x60000000, table_off=0x1ad6d0, table=TSE_TABLE,
        count_off=None, count=None,        # loops end at pointer 0x601ad788
        table_refs={0x371ee: 0x601ad6c8,   # float 1.0 just BEFORE the table
                    0x3a7f8: 0x601ad788, 0x3a8a2: 0x601ad6d0,
                    0x3a91e: 0x601ad788, 0x3ab19: 0x601ad6d0,
                    0x3abcc: 0x601ad788},
        patched_md5='4bdeec64bb57c0bd09d20319d195e08e'),
}

STARTUP_REL = 'Scripts/Game_startup.ini'
FLEETRES_INI = 'Scripts\\Game_FleetRes.ini'           # the per-box file
INCLUDE_LINE = 'include "%s";' % FLEETRES_INI
# Title-neutral on purpose (the same block goes into TFE's file, which has no
# level lines), and free of backslashes, quotes and parentheses: it is read by
# the engine's scanner, and a comment should give it nothing to trip on.
INCLUDE_BLOCK = [
    '',
    '// NSC fleet: everything above this block is the file exactly as shipped.',
    '// In The Second Encounter those shipped lines are what NEW GAME needs to',
    '// find its first level, so this file is never rewritten. The per-box',
    '// display mode lives in Game_FleetRes.ini, which the launcher writes at',
    '// every start and GAMERES at every GAMESYNC. Never stage Game_FleetRes.ini.',
    INCLUDE_LINE,
]
STARTUP = {
    'SeriousSamFirstEncounter': dict(
        size=86, md5='cf9c168d9af4e35df51b81b2b7a925ec',
        must_keep=['// executed each time SeriousSam is started'],
        patched_md5='dc8292081b7a038acc9f6def8e9f1aac'),
    'SeriousSamSecondEncounter': dict(
        size=224, md5='fb6e5696d4980a0d13e390aa745fea54',
        must_keep=['sam_strFirstLevel = "Levels\\\\LevelsMP\\\\1_0_InTheLastEpisode.wld";',
                   'sam_strIntroLevel = "Levels\\\\LevelsMP\\\\Intro.wld";',
                   'sam_strTechTestLevel = "Levels\\\\LevelsMP\\\\Technology\\\\TechTest.wld";',
                   'sam_strGameName = "serioussamse";'],
        patched_md5='7e38c3e47336da600a196bf5a06cca55'),
}

SERVERS = {
    'SeriousSamFirstEncounter': ('~/ssam-tfe-server', 'ssam-tfe-server'),
    'SeriousSamSecondEncounter': ('~/ssam-tse-server', 'ssam-tse-server'),
}


# ---------------------------------------------------------------------------
# Pure logic - no share needed (tests/python/test_patch_serioussam.py)
# ---------------------------------------------------------------------------
def is_dual_head(w, h):
    """Engine.dll IsDualHead (TFE 0x600fc1c0): w*3 == h*8."""
    return w * 3 == h * 8


def is_wide_screen(w, h):
    """Engine.dll CDisplayMode::IsWideScreen (TFE 0x600fc1e0): exactly 856x480."""
    return (w, h) == (856, 480)


def is_drawport_wide(w, h):
    """Engine.dll CDrawPort::IsWideScreen (TFE 0x600fc610, TSE 0x6003ed60):
    w*9 == h*16. Already true at runtime on every 1080p box (the launcher runs
    the game at 1920x1080); the table patch does not change it."""
    return w * 9 == h * 16


def new_table(table):
    """The patched table: same length, DualHead slots replaced in place."""
    out = list(table)
    for old, new in REPLACE:
        out[out.index(old)] = new
    return out


def edits_for(eng):
    """[(file offset, old pair, new pair)] for one engine."""
    t = eng['table']
    return [(eng['table_off'] + 8 * t.index(old), old, new) for old, new in REPLACE]


def md5b(b):
    return hashlib.md5(b).hexdigest()


def pe_headers(b):
    """Minimal PE32 parse: (checksum offset, image base, sections, reloc dir)."""
    if b[:2] != b'MZ':
        raise ValueError('not an MZ image')
    pe = struct.unpack_from('<I', b, 0x3c)[0]
    if b[pe:pe + 4] != b'PE\0\0':
        raise ValueError('no PE signature')
    nsec = struct.unpack_from('<H', b, pe + 6)[0]
    optsz = struct.unpack_from('<H', b, pe + 20)[0]
    opt = pe + 24
    if struct.unpack_from('<H', b, opt)[0] != 0x10b:
        raise ValueError('not PE32')
    base = struct.unpack_from('<I', b, opt + 28)[0]
    ndirs = struct.unpack_from('<I', b, opt + 92)[0]
    reloc = struct.unpack_from('<II', b, opt + 96 + 8 * 5) if ndirs > 5 else (0, 0)
    secs = []
    so = opt + optsz
    for i in range(nsec):
        name, vsize, va, rsize, raw = struct.unpack_from('<8sIIII', b, so + 40 * i)
        secs.append((name.rstrip(b'\0').decode('latin1'), va, max(vsize, rsize), raw, rsize))
    return dict(checksum_off=opt + 64, image_base=base, sections=secs, reloc=reloc)


def rva_to_off(h, rva):
    for _n, va, vs, raw, rs in h['sections']:
        if va <= rva < va + vs:
            if rva - va >= rs:
                return None
            return raw + rva - va
    return None


def off_to_section(h, off):
    for n, va, _vs, raw, rs in h['sections']:
        if raw <= off < raw + rs:
            return n, va + off - raw
    return None, None


def relocs(b, h):
    """Every HIGHLOW (type 3) relocation RVA in the image."""
    rva, size = h['reloc']
    out = []
    if not rva:
        return out
    off = rva_to_off(h, rva)
    end = off + size
    while off + 8 <= end:
        page, bsize = struct.unpack_from('<II', b, off)
        if bsize < 8:
            break
        for i in range((bsize - 8) // 2):
            e = struct.unpack_from('<H', b, off + 8 + 2 * i)[0]
            if e >> 12 == 3:
                out.append(page + (e & 0xfff))
        off += bsize
    return out


def pe_checksum(b, checksum_off):
    """The IMAGEHLP CheckSumMappedFile algorithm (checksum field counted as 0)."""
    s = 0
    n = len(b)
    padded = b + b'\0' * (n & 1)
    for i in range(0, len(padded), 2):
        if checksum_off <= i < checksum_off + 4:
            continue
        s += padded[i] | (padded[i + 1] << 8)
        s = (s & 0xffff) + (s >> 16)
    s = (s & 0xffff) + (s >> 16)
    return (s + n) & 0xffffffff


def read_table(b, off, n):
    return [struct.unpack_from('<ii', b, off + 8 * i) for i in range(n)]


def check_engine(b, eng, want_md5=True):
    """Assert everything the patch relies on. Returns a list of report lines."""
    rep = []
    if want_md5:
        if len(b) != eng['size'] or md5b(b) != eng['md5']:
            raise ValueError('unexpected original: %d B md5 %s (want %d B %s)'
                             % (len(b), md5b(b), eng['size'], eng['md5']))
        rep.append('original %d B md5 %s ok' % (len(b), eng['md5']))
    got = read_table(b, eng['table_off'], len(eng['table']))
    if [tuple(p) for p in got] != eng['table']:
        raise ValueError('resolution table at %#x is not the expected one: %r'
                         % (eng['table_off'], got))
    rep.append('table @%#x: %d pairs as expected' % (eng['table_off'], len(got)))
    if eng['count_off'] is not None:
        c = struct.unpack_from('<i', b, eng['count_off'])[0]
        if c != eng['count']:
            raise ValueError('count word at %#x is %d, want %d' % (eng['count_off'], c, eng['count']))
        rep.append('count word @%#x = %d (left unchanged)' % (eng['count_off'], c))
    h = pe_headers(b)
    if h['image_base'] != eng['image_base']:
        raise ValueError('image base %#x' % h['image_base'])
    sec, rva0 = off_to_section(h, eng['table_off'])
    if sec != '.data':
        raise ValueError('table not in .data (%s)' % sec)
    va0 = h['image_base'] + rva0
    va1 = va0 + 8 * len(eng['table'])
    refs = {}
    rl = relocs(b, h)
    for r in rl:
        o = rva_to_off(h, r)
        if o is None:
            continue
        v = struct.unpack_from('<I', b, o)[0]
        if va0 - 8 <= v < va1 + 8:
            refs[r] = v
    if refs != eng['table_refs']:
        raise ValueError('pointers into the table changed: %s'
                         % {hex(k): hex(v) for k, v in refs.items()})
    for v in refs.values():
        if va0 < v < va1:
            raise ValueError('code addresses an individual entry %#x' % v)
    rep.append('%d relocated pointers near the table, all to its base/count/end - none to an entry'
               % len(refs))
    for off, old, new in edits_for(eng):
        rva = rva0 + off - eng['table_off']
        if any(rva - 3 <= r < rva + 8 for r in rl):
            raise ValueError('a base relocation touches %#x' % off)
        if is_dual_head(*new) or is_wide_screen(*new):
            raise ValueError('%r trips IsDualHead/IsWideScreen' % (new,))
        if not is_dual_head(*old):
            raise ValueError('%r was not a DualHead slot' % (old,))
        rep.append('  %#x  %dx%d -> %dx%d  (no reloc; IsDualHead/IsWideScreen false)'
                   % ((off,) + old + new))
    cs = struct.unpack_from('<I', b, h['checksum_off'])[0]
    calc = pe_checksum(b, h['checksum_off'])
    rep.append('PE checksum header %#x, computed %#x (%s)'
               % (cs, calc, 'valid - will be kept valid' if cs == calc else 'NOT valid - left alone'))
    return rep


def patch_engine(b, eng):
    """Return the patched bytes. Refuses anything but the exact original."""
    check_engine(b, eng, want_md5=False)
    out = bytearray(b)
    for off, old, new in edits_for(eng):
        if struct.unpack_from('<ii', out, off) != old:
            raise ValueError('bytes at %#x are not %r' % (off, old))
        struct.pack_into('<ii', out, off, *new)
    h = pe_headers(b)
    if struct.unpack_from('<I', b, h['checksum_off'])[0] == pe_checksum(b, h['checksum_off']):
        struct.pack_into('<I', out, h['checksum_off'], pe_checksum(bytes(out), h['checksum_off']))
    return bytes(out)


def diff_ranges(a, b):
    """[(start, end)] of differing byte runs - the post-condition of a same-size patch."""
    if len(a) != len(b):
        raise ValueError('size changed')
    out, i, n = [], 0, len(a)
    while i < n:
        if a[i] != b[i]:
            j = i
            while j < n and a[j] != b[j]:
                j += 1
            out.append((i, j))
            i = j
        else:
            i += 1
    return out


def patch_startup(text):
    """Shipped Game_startup.ini bytes -> shipped + include block. Idempotent."""
    s = text.decode('latin1')
    if INCLUDE_LINE in s:
        return text
    if s and not s.endswith('\r\n'):
        s = s.rstrip('\n').rstrip('\r') + '\r\n'
    s += '\r\n'.join(INCLUDE_BLOCK) + '\r\n'
    return s.encode('latin1')


def check_startup(text, spec, want_md5=True):
    rep = []
    if want_md5:
        if len(text) != spec['size'] or md5b(text) != spec['md5']:
            raise ValueError('unexpected Game_startup.ini: %d B md5 %s' % (len(text), md5b(text)))
        rep.append('original %d B md5 %s ok' % (len(text), spec['md5']))
    s = text.decode('latin1')
    for line in spec['must_keep']:
        if line not in s:
            raise ValueError('shipped line missing: %s' % line)
    rep.append('%d shipped line(s) present' % len(spec['must_keep']))
    return rep


# ---------------------------------------------------------------------------
# Share-side actions
# ---------------------------------------------------------------------------
def items(lib, only=None):
    """(title, kind, rel) for every file this patch owns."""
    out = []
    for t in sorted(ENGINES):
        if only in (None, 'engine'):
            out.append((t, 'engine', ENGINES[t]['rel']))
        if only in (None, 'startup'):
            out.append((t, 'startup', STARTUP_REL))
    return out


def src_path(lib, t, rel):
    return os.path.join(lib, t, *rel.split('/'))


def backup_path(lib, t, rel):
    """Where --publish keeps the staged original once it has replaced it."""
    return os.path.join(lib, *(BACKUP_DIR % t).split('/'), *rel.split('/'))


def spec_for(t, kind):
    return ENGINES[t] if kind == 'engine' else STARTUP[t]


def pinned(t, kind):
    """(original md5, patched md5) this review signed off on, or None."""
    spec = (ENGINES if kind == 'engine' else STARTUP).get(t)
    return (spec['md5'], spec['patched_md5']) if spec else None


def read_original(lib, t, kind, rel):
    """The staged original, as bytes, and where it was read from.

    Before --publish that is the library copy. After it, the library holds the
    PATCH and the original lives only in the backup --publish made - so a
    rebuild reads it from there instead of failing, and a patched library copy
    with NO verified backup is a hard error: the patch could no longer be
    reproduced from the staged original, and that must not pass quietly."""
    spec = spec_for(t, kind)
    p = src_path(lib, t, rel)
    b = open(p, 'rb').read()
    if md5b(b) != spec['patched_md5']:
        return b, p
    bp = backup_path(lib, t, rel)
    try:
        ob = open(bp, 'rb').read()
    except OSError:
        raise ValueError('%s is already PATCHED and its original is NOT at %s - '
                         'the patch can no longer be rebuilt from the staged '
                         'original' % (p, bp))
    if len(ob) != spec['size'] or md5b(ob) != spec['md5']:
        raise ValueError('backup %s is %d B md5 %s, not the original %d B %s'
                         % (bp, len(ob), md5b(ob), spec['size'], spec['md5']))
    return ob, bp


def build_one(lib, t, kind, rel):
    orig, where = read_original(lib, t, kind, rel)
    if kind == 'engine':
        eng = ENGINES[t]
        rep = check_engine(orig, eng)
        new = patch_engine(orig, eng)
        d = diff_ranges(orig, new)
        h = pe_headers(orig)
        allowed = [(o, o + 8) for o, _a, _b in edits_for(eng)] + \
                  [(h['checksum_off'], h['checksum_off'] + 4)]
        for a, b_ in d:
            if not any(x <= a and b_ <= y for x, y in allowed):
                raise ValueError('unexpected change %#x-%#x' % (a, b_))
        rep.append('diff: %s' % ', '.join('%#x-%#x' % r for r in d))
        got = [tuple(p) for p in read_table(new, eng['table_off'], len(eng['table']))]
        if got != new_table(eng['table']):
            raise ValueError('post-condition: table did not come out as intended')
        rep.append('post: table = ' + ' '.join('%dx%d' % p for p in got))
    else:
        spec = STARTUP[t]
        rep = check_startup(orig, spec)
        new = patch_startup(orig)
        check_startup(new, spec, want_md5=False)
        if not new.startswith(orig) or new.count(INCLUDE_LINE.encode()) != 1:
            raise ValueError('post-condition: shipped bytes not preserved')
        rep.append('post: shipped %d B kept verbatim + include of %s' % (len(orig), FLEETRES_INI))
    if md5b(new) != spec_for(t, kind)['patched_md5']:
        raise ValueError('rebuilt md5 %s is not the pinned %s - this script no '
                         'longer produces the reviewed bytes'
                         % (md5b(new), spec_for(t, kind)['patched_md5']))
    if where != src_path(lib, t, rel):
        rep.append('library copy is ALREADY PATCHED; original read from the backup %s' % where)
    return orig, new, rep


def cmd_check(lib):
    ok = True
    for t, kind, rel in items(lib):
        print('== %s %s (%s)' % (t, rel, kind))
        try:
            _o, new, rep = build_one(lib, t, kind, rel)
            for r in rep:
                print('  ' + r)
            print('  -> patched md5 %s (%d B) = the pinned md5' % (md5b(new), len(new)))
        except (OSError, ValueError) as e:
            ok = False
            print('  FAIL: %s' % e)
    return 0 if ok else 1


def git_work_tree(path):
    """The git work tree that would contain PATH, or None. Patched game
    binaries must never be written where a `git add` could sweep them up."""
    p = os.path.realpath(os.path.expanduser(path))
    while True:
        if os.path.exists(os.path.join(p, '.git')):
            return p
        up = os.path.dirname(p)
        if up == p:
            return None
        p = up


def cmd_build(lib, outdir):
    outdir = os.path.expanduser(outdir)
    tree = git_work_tree(outdir)
    if tree:
        print('REFUSED: %s is inside the git work tree %s - game binaries must '
              'stay out of git. Use ~/.retro-fleet/patch-out/serioussam.' % (outdir, tree))
        return 2
    man = {'patch': 'serioussam', 'library': lib, 'files': []}
    for t, kind, rel in items(lib):
        orig, new, rep = build_one(lib, t, kind, rel)
        for sub, data in (('patched', new), ('originals', orig)):
            p = os.path.join(outdir, sub, t, *rel.split('/'))
            os.makedirs(os.path.dirname(p), exist_ok=True)
            with open(p, 'wb') as f:
                f.write(data)
            if md5b(open(p, 'rb').read()) != md5b(data):
                raise SystemExit('local write did not verify: %s' % p)
        man['files'].append(dict(
            title=t, kind=kind,
            share_path='%s/%s/%s' % (SHARE_PREFIX, t, rel),
            backup_share_path='%s/%s/%s' % (SHARE_PREFIX, BACKUP_DIR % t, rel),
            local_path=os.path.join(outdir, 'patched', t, *rel.split('/')),
            original_local_path=os.path.join(outdir, 'originals', t, *rel.split('/')),
            md5=md5b(new), size=len(new),
            original_md5=md5b(orig), original_size=len(orig),
            report=rep))
        print('%-27s %-24s %s -> %s' % (t, rel, md5b(orig), md5b(new)))
    mp = os.path.join(outdir, 'manifest.json')
    with open(mp, 'w') as f:
        json.dump(man, f, indent=1)
    print('manifest: %s' % mp)
    return 0


def share_md5(lib, share_path):
    rel = share_path[len(SHARE_PREFIX) + 1:]
    p = os.path.join(lib, *rel.split('/'))
    try:
        return md5b(open(p, 'rb').read())
    except OSError:
        return None


def put(local, dest):
    r = subprocess.run([sys.executable, SHAREWRITE, 'put', local, dest])
    return r.returncode == 0


def cmd_publish(lib, outdir, only=None):
    """FUTURE USE. One file at a time; stop on the first failure; idempotent.

    Only the bytes this review signed off on are published: every manifest
    entry must carry the pinned original and patched md5s, checked before a
    single byte is written - an OUTDIR built by an edited script, or a
    hand-edited manifest, is refused rather than shipped to eight boxes."""
    man = json.load(open(os.path.join(os.path.expanduser(outdir), 'manifest.json')))
    for f in man['files']:
        if only and f['kind'] != only:
            continue
        pin = pinned(f['title'], f['kind'])
        if pin != (f['original_md5'], f['md5']):
            print('STOP: manifest entry %s (%s -> %s) is not the reviewed pin %s - '
                  'nothing was written' % (f['share_path'], f['original_md5'], f['md5'], pin))
            return 2
    for f in man['files']:
        if only and f['kind'] != only:
            continue
        now = share_md5(lib, f['share_path'])
        if now == f['md5']:
            print('skip %s - share already has the patched md5' % f['share_path'])
            continue
        if now != f['original_md5']:
            print('STOP: %s is %s, neither the original %s nor the patch %s'
                  % (f['share_path'], now, f['original_md5'], f['md5']))
            return 2
        if share_md5(lib, f['backup_share_path']) != f['original_md5']:
            if md5b(open(f['original_local_path'], 'rb').read()) != f['original_md5']:
                print('STOP: local original copy is not the original')
                return 2
            if not put(f['original_local_path'], f['backup_share_path']):
                print('STOP: backup of %s failed' % f['share_path'])
                return 3
        if md5b(open(f['local_path'], 'rb').read()) != f['md5']:
            print('STOP: local patched copy changed since --build')
            return 2
        if not put(f['local_path'], f['share_path']):
            print('STOP: put of %s failed - the share copy may be MISSING; '
                  'restore from %s' % (f['share_path'], f['backup_share_path']))
            return 3
        if share_md5(lib, f['share_path']) != f['md5']:
            print('STOP: %s does not read back as the patch' % f['share_path'])
            return 3
        print('published %s (md5 %s)' % (f['share_path'], f['md5']))
    return 0


def cmd_install_server(outdir):
    """FUTURE USE, and NOT NEEDED: the CRC challenge does not cover Bin\\*.dll,
    and a dedicated server never enumerates display modes.
    Puts the patched Engine.dll into each fleet server tree (original kept as
    Engine.dll.orig-2026-09-29) so client and server are byte-identical.

    The server may be RUNNING: ~/ssam-*-server is bind-mounted into a Wine
    container that has Bin\\Engine.dll mapped. Overwriting that file in place
    (open 'wb' truncates the same inode) can SIGBUS the live server, so the
    new bytes go to a temp file beside it and are renamed over it - a new
    inode; the running process keeps the old one until the unit restarts."""
    man = json.load(open(os.path.join(os.path.expanduser(outdir), 'manifest.json')))
    for f in man['files']:
        if f['kind'] != 'engine':
            continue
        if pinned(f['title'], f['kind']) != (f['original_md5'], f['md5']):
            print('STOP: manifest entry for %s is not the reviewed pin' % f['title'])
            return 2
        base, unit = SERVERS[f['title']]
        dst = os.path.join(os.path.expanduser(base), 'Bin', 'Engine.dll')
        cur = md5b(open(dst, 'rb').read())
        if cur == f['md5']:
            print('skip %s - already patched' % dst)
            continue
        if cur != f['original_md5']:
            print('STOP: %s is %s, not the original' % (dst, cur))
            return 2
        bak = dst + '.orig-2026-09-29'
        if not os.path.exists(bak):
            shutil.copy2(dst, bak)
        if md5b(open(bak, 'rb').read()) != f['original_md5']:
            print('STOP: backup %s is not the original - nothing replaced' % bak)
            return 3
        if md5b(open(f['local_path'], 'rb').read()) != f['md5']:
            print('STOP: local patched copy changed since --build')
            return 2
        tmp = '%s.new-%d' % (dst, os.getpid())
        shutil.copyfile(f['local_path'], tmp)
        if md5b(open(tmp, 'rb').read()) != f['md5']:
            os.unlink(tmp)
            print('STOP: %s did not verify - %s left untouched' % (tmp, dst))
            return 3
        os.replace(tmp, dst)
        if md5b(open(dst, 'rb').read()) != f['md5']:
            print('STOP: %s did not verify; restore from %s' % (dst, bak))
            return 3
        print('installed %s; now: systemctl --user restart %s' % (dst, unit))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--library', default=LIB)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument('--check', action='store_true')
    g.add_argument('--build', metavar='OUTDIR')
    g.add_argument('--publish', metavar='OUTDIR')
    g.add_argument('--install-server', metavar='OUTDIR')
    ap.add_argument('--only', choices=('engine', 'startup'))
    a = ap.parse_args(argv)
    if not os.path.isdir(a.library):
        print('library not mounted: %s' % a.library)
        return 2
    if a.check:
        return cmd_check(a.library)
    if a.build:
        return cmd_build(a.library, a.build)
    if a.publish:
        return cmd_publish(a.library, a.publish, a.only)
    return cmd_install_server(a.install_server)


if __name__ == '__main__':
    sys.exit(main())
