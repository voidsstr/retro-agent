#!/usr/bin/env python3
"""stage-new-title.py - publish a captured tree as a NEW library title, whole.

    python3 scripts/fleet/stage-new-title.py <local tree> <Title> [--dry-run]

WHY. A new top-level directory in Games-Library is what retro-autodeploy
answers - within ~90 s - with GAMESYNC RESET + START on every box that answers
(CLAUDE.md "WRITING TO THE SHARE"). Uploading a title file by file straight to
its final name therefore lets boxes start copying a HALF-uploaded tree, and
GAMESYNC never deletes, so whatever they copied of it stays. The library
generators refuse to create a title at all (libwrite.check_place); this is the
one deliberate way to do it:

  1. refuse if the title already exists (case-insensitive), starts with '_',
     or is not a plain name;
  2. upload every file to Files/Games-Library/_incoming/<Title>/... - a
     '_'-prefixed support dir the agent skips - one sharewrite put per file,
     each read back through /mnt (md5 + a fresh write time); files already
     there with the same md5 are skipped, so a failed run can be re-run;
  3. verify the WHOLE incoming tree through /mnt: file count, byte total and
     every md5 against the local tree (the d2disk lesson: never an exit code);
  4. ONE server-side rename to Files/Games-Library/<Title> - the moment the
     title appears it is complete;
  5. verify the final tree the same way, and that _incoming/<Title> is gone.

Run scripts/validate-staged-library.py afterwards; the fleet syncs the title
by itself (autodeploy), gated per box by its requires.json.
"""
import argparse
import hashlib
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sharewrite  # noqa: E402

MNT = sharewrite.MNT
LIB_REL = 'Files/Games-Library'
INCOMING = '_incoming'
NAME = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')


def md5_file(p):
    h = hashlib.md5()
    with open(p, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            h.update(chunk)
    return h.hexdigest()


def local_files(root):
    out = []
    for d, _, fs in os.walk(root):
        for f in fs:
            p = os.path.join(d, f)
            out.append((os.path.relpath(p, root).replace(os.sep, '/'), p))
    return sorted(out)


def exists_ci(parent, name):
    try:
        return any(e.lower() == name.lower() for e in os.listdir(parent))
    except FileNotFoundError:
        return False


def verify_tree(mnt_root, files, log):
    """count + bytes + md5 of every file under mnt_root against the local list"""
    bad, total = [], 0
    for rel, p in files:
        q = os.path.join(mnt_root, *rel.split('/'))
        if not os.path.isfile(q):
            bad.append('missing ' + rel)
            continue
        if os.path.getsize(q) != os.path.getsize(p) or md5_file(q) != md5_file(p):
            bad.append('differs ' + rel)
        total += os.path.getsize(q)
    have = sum(len(fs) for _, _, fs in os.walk(mnt_root)) if os.path.isdir(mnt_root) else 0
    if have != len(files):
        bad.append('file count %d on the share, %d local' % (have, len(files)))
    log('verify %s: %d file(s), %d bytes, %d problem(s)' % (mnt_root, have, total, len(bad)))
    return bad


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('tree')
    ap.add_argument('title')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args(argv)
    if not NAME.match(a.title):
        raise SystemExit('not a plain title name: %r' % a.title)
    lib = os.path.join(MNT, *LIB_REL.split('/'))
    if exists_ci(lib, a.title):
        raise SystemExit('%s already exists in the library (case-insensitive) - this only '
                         'creates NEW titles; change an existing one with its generator' % a.title)
    files = local_files(a.tree)
    if not files:
        raise SystemExit('no files under %s' % a.tree)
    size = sum(os.path.getsize(p) for _, p in files)
    print('%s: %d file(s), %.1f MB -> %s/%s/%s' % (a.title, len(files), size / 1048576.0,
                                                  LIB_REL, INCOMING, a.title))
    if a.dry_run:
        return 0
    inc_rel = '%s/%s/%s' % (LIB_REL, INCOMING, a.title)
    inc_mnt = os.path.join(MNT, *inc_rel.split('/'))
    with sharewrite.Auth() as auth:
        for i, (rel, p) in enumerate(files, 1):
            q = os.path.join(inc_mnt, *rel.split('/'))
            if os.path.isfile(q) and os.path.getsize(q) == os.path.getsize(p) and md5_file(q) == md5_file(p):
                continue
            rc = sharewrite.put(p, '%s/%s' % (inc_rel, rel), auth, log=lambda m: None)
            if rc != 0:
                raise SystemExit('PUBLISH FAILED at %s (%d of %d) - the title is NOT live; '
                                 're-run to resume' % (rel, i, len(files)))
            if i % 10 == 0 or i == len(files):
                print('  %d/%d uploaded' % (i, len(files)))
        bad = verify_tree(inc_mnt, files, print)
        if bad:
            raise SystemExit('the incoming tree is not whole - NOT promoted: %s' % bad[:10])
        final_rel = '%s/%s' % (LIB_REL, a.title)
        rc, out = sharewrite.smb(auth, 'rename %s %s' % (sharewrite.smb_quote(inc_rel),
                                                         sharewrite.smb_quote(final_rel)))
        final_mnt = os.path.join(lib, a.title)
        deadline = time.time() + 30
        while time.time() < deadline and not os.path.isdir(final_mnt):
            time.sleep(2)
        if not os.path.isdir(final_mnt):
            raise SystemExit('rename did not show up through /mnt: %s' % out)
    bad = verify_tree(final_mnt, files, print)
    if bad or os.path.isdir(inc_mnt):
        raise SystemExit('PROMOTED BUT NOT CLEAN: %s%s' % (bad[:10], ' (incoming still there)' if os.path.isdir(inc_mnt) else ''))
    print('LIVE: %s/%s - retro-autodeploy will offer it to every box; run '
          'scripts/validate-staged-library.py' % (LIB_REL, a.title))
    return 0


if __name__ == '__main__':
    sys.exit(main())
