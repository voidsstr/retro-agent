#!/usr/bin/env python3
"""Move a box's game library to another drive and repoint everything at it.

    python3 scripts/fleet/relocate-games.py <ip> --from C: --to D:
    python3 scripts/fleet/relocate-games.py <ip> --from C: --to D: --dry-run

The copy itself is NOT done here - 32 GB over the agent's command channel is the
wrong tool, and a detached `xcopy` on the box is the right one. What this script
owns is the half that is easy to forget and silent when wrong: every POINTER at
the old location.

Three classes of pointer, all of which really exist on the fleet:

1. **Desktop and Start Menu `.lnk` files.** Patched as BYTES, not rewritten. A
   shell link stores its target three times over (the LinkInfo local base path,
   the relative/working-dir strings, and the icon location), in a mix of ANSI
   and UTF-16, behind length-prefixed structures. Regenerating a link loses the
   icon index and the arguments; recomputing the length fields by hand is how
   you produce a link that opens Explorer instead of the game. Swapping ONE
   BYTE - the drive letter - changes no length anywhere, so it is the only edit
   that cannot corrupt the structure.

2. **Registry values** - install paths, `App Paths`, per-title "CD Path". Found
   by exporting the hives and searching the export, because XP's `reg query` has
   NO `/f` search option (that arrived with Vista) and answers `Too many
   command-line parameters` if you try, which reads exactly like a quoting bug.

3. **Text files inside the tree itself** - `.bat` launchers, `.ini`, `.cfg`.
   These MOVE with the tree, so the file is fine and its CONTENT is stale.

Nothing here trusts a return code. Every `.lnk` is read back after the write and
compared, the registry values are re-read with `REGREAD`, and the script reports
what it verified rather than what it attempted.
"""

import argparse
import asyncio
import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..'))

from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = 'retro-agent-secret'
PORT = 9898

#: Directories holding shortcuts that can point into the game tree. Expanded on
#: the box, because the profile names are not ours to guess.
LNK_DIRS = (
    r'%ALLUSERSPROFILE%\Desktop',
    r'%ALLUSERSPROFILE%\Start Menu',
    r'%USERPROFILE%\Desktop',
    r'%USERPROFILE%\Start Menu',
)

#: Extensions worth rewriting inside the moved tree. Deliberately a short list:
#: a blind sweep of 23,000 files would rewrite save games and binary configs.
TEXT_EXTS = ('.bat', '.cmd', '.ini', '.cfg', '.txt', '.url', '.reg', '.lst')


def _utf16_pattern(text):
    """Case-insensitive UTF-16LE byte pattern for an ASCII string."""
    out = b''
    for ch in text:
        lo, up = ch.lower().encode('ascii'), ch.upper().encode('ascii')
        out += b'[' + re.escape(lo) + re.escape(up) + b']\\x00' if lo != up \
            else re.escape(ch.encode('ascii')) + b'\\x00'
    return out


def patch_bytes(blob, old_root, new_root):
    """Swap old_root for new_root in ANSI and UTF-16LE. Returns (blob, hits).

    Byte-length preserving by construction - the two roots must be the same
    length, which the caller asserts. That is what makes it safe to do this to a
    structured binary like a shell link, whose length-prefixed fields would
    otherwise all need recomputing.

    Case-insensitive on purpose: Windows paths are, and one tool's 'c:\\games'
    is another's 'C:\\Games'. A case-sensitive search reports a file clean that
    is not - the exact mistake this repo has paid for repeatedly.
    """
    hits = 0
    # The replacement must be a callable: a literal 'D:\\Games' is parsed as a
    # regex TEMPLATE, where '\\G' is an invalid escape and raises.
    ansi = re.compile(re.escape(old_root.encode('ascii')), re.IGNORECASE)
    blob, n = ansi.subn(lambda m: new_root.encode('ascii'), blob)
    hits += n
    wide = re.compile(_utf16_pattern(old_root))
    blob, n = wide.subn(lambda m: new_root.encode('utf-16-le'), blob)
    hits += n
    return blob, hits


async def text(conn, cmd, timeout=120.0):
    return await conn.command_text(cmd, timeout=timeout)


async def list_lnks(conn, old_root):
    """Every .lnk under the shortcut dirs that mentions the old root."""
    out = []
    for d in LNK_DIRS:
        try:
            listing = await text(conn, 'EXEC cmd /c dir "%s\\*.lnk" /s /b' % d)
        except Exception:
            continue
        for line in listing.splitlines():
            line = line.strip()
            if line.lower().endswith('.lnk'):
                out.append(line)
    # de-dup: %USERPROFILE% and %ALLUSERSPROFILE% can resolve to the same tree
    seen, uniq = set(), []
    for p in out:
        if p.lower() not in seen:
            seen.add(p.lower())
            uniq.append(p)
    return uniq


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('host')
    ap.add_argument('--from', dest='src', default='C:', help='old drive, e.g. C:')
    ap.add_argument('--to', dest='dst', default='D:', help='new drive, e.g. D:')
    ap.add_argument('--dir', default='Games', help='directory name under each drive')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--skip-lnk', action='store_true')
    ap.add_argument('--skip-reg', action='store_true')
    ap.add_argument('--skip-text', action='store_true')
    args = ap.parse_args()

    old_root = '%s\\%s' % (args.src.rstrip('\\'), args.dir)
    new_root = '%s\\%s' % (args.dst.rstrip('\\'), args.dir)
    if len(old_root) != len(new_root):
        sys.exit('refusing: %r and %r differ in length - a byte-preserving '
                 '.lnk patch is only safe when they do not' % (old_root, new_root))

    conn = RetroConnection(args.host, PORT)
    await conn.connect(SECRET, timeout=15.0)
    print('connected to %s' % args.host)

    try:
        # ---------- shortcuts ----------
        if not args.skip_lnk:
            lnks = await list_lnks(conn, old_root)
            print('\n== shortcuts: %d found' % len(lnks))
            changed = failed = clean = 0
            for p in lnks:
                status, blob = await conn.send_command('DOWNLOAD %s' % p)
                if status != 0:
                    print('  READ FAIL  %s' % p)
                    failed += 1
                    continue
                new, hits = patch_bytes(blob, old_root, new_root)
                if not hits:
                    clean += 1
                    continue
                if args.dry_run:
                    print('  would patch (%d hits) %s' % (hits, os.path.basename(p)))
                    changed += 1
                    continue
                st, _ = await conn.send_command('UPLOAD %s' % p, binary_payload=new)
                # Verify the POST-CONDITION: read it back and compare. An
                # UPLOAD that answers OK has still not been proven to have
                # landed the bytes we meant.
                st2, back = await conn.send_command('DOWNLOAD %s' % p)
                if st != 0 or st2 != 0 or back != new:
                    print('  VERIFY FAIL %s' % os.path.basename(p))
                    failed += 1
                else:
                    changed += 1
            print('  patched %d, already clean %d, failed %d'
                  % (changed, clean, failed))

        # ---------- registry ----------
        if not args.skip_reg:
            print('\n== registry')
            hits = await find_registry(conn, old_root, args.dst + '\\reloctmp')
            print('  %d value(s) reference %s' % (len(hits), old_root))
            for key, name, val in hits:
                newval = re.sub(re.escape(old_root), new_root, val, flags=re.I)
                if args.dry_run:
                    print('  would set [%s] %s = %s' % (key, name, newval))
                    continue
                root, sub = key.split('\\', 1)
                nm = name if name else '@'
                st, out = await conn.send_command(
                    'REGWRITE %s %s %s REG_SZ %s' % (root, sub, nm, newval))
                # never trust the OK - read it back
                st2, back = await conn.send_command('REGREAD %s %s %s' % (root, sub, nm))
                ok = st == 0 and newval.lower() in back.decode('ascii', 'replace').lower()
                print('  %s [%s] %s' % ('OK  ' if ok else 'FAIL', key, nm))

        # ---------- text files inside the tree ----------
        if not args.skip_text:
            print('\n== text files under %s' % new_root)
            pats = ' '.join('"%s\\*%s"' % (new_root, e) for e in TEXT_EXTS)
            listing = await text(
                conn,
                'EXEC cmd /c findstr /s /m /i /c:"%s" %s' % (old_root, pats),
                timeout=600.0)
            files = [l.strip() for l in listing.splitlines() if l.strip().lower().startswith(args.dst.lower())]
            print('  %d file(s) still name %s' % (len(files), old_root))
            for p in files:
                st, blob = await conn.send_command('DOWNLOAD %s' % p)
                if st != 0:
                    print('  READ FAIL %s' % p)
                    continue
                new, hits = patch_bytes(blob, old_root, new_root)
                if args.dry_run:
                    print('  would patch (%d) %s' % (hits, p))
                    continue
                await conn.send_command('UPLOAD %s' % p, binary_payload=new)
                st2, back = await conn.send_command('DOWNLOAD %s' % p)
                print('  %s %s' % ('OK  ' if (st2 == 0 and back == new) else 'FAIL', p))
    finally:
        await conn.close()


async def find_registry(conn, old_root, tmp):
    """Export the hives on the box and search the export.

    XP's `reg query` has no `/f`; it fails with "Too many command-line
    parameters", which looks like a quoting problem and is not one.
    """
    await conn.send_command('EXEC cmd /c mkdir "%s" 2>nul' % tmp)
    hives = (('HKLM', r'HKLM\SOFTWARE'), ('HKCU', 'HKCU'), ('HKCR', 'HKEY_CLASSES_ROOT'))
    hits = []
    for tag, hive in hives:
        await conn.command_text(
            'EXECW 600 cmd /c reg export %s "%s\\%s.reg" > nul 2>&1' % (hive, tmp, tag),
            timeout=650.0)
        st, blob = await conn.send_command('DOWNLOAD %s\\%s.reg' % (tmp, tag))
        if st != 0:
            continue
        try:
            body = blob.decode('utf-16-le', 'replace')
        except Exception:
            body = blob.decode('latin-1', 'replace')
        key = None
        esc = old_root.replace('\\', '\\\\')
        for line in body.splitlines():
            line = line.rstrip('\r')
            if line.startswith('['):
                key = line.strip('[]')
            elif esc.lower() in line.lower() and '=' in line:
                name, _, val = line.partition('=')
                name = name.strip().strip('"')
                if name == '@':
                    name = ''
                val = val.strip()
                if val.startswith('"') and val.endswith('"'):
                    val = val[1:-1].replace('\\\\', '\\')
                    hits.append((key, name, val))
    return hits


if __name__ == '__main__':
    asyncio.run(main())
