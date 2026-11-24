#!/usr/bin/env python3
"""stage-title-subdir.py - add a NEW subdirectory to an EXISTING library title, whole.

    python3 scripts/fleet/stage-title-subdir.py <local dir> <Title> <subdir> [--dry-run]

e.g. The Specialists 3.0 is a mod folder `ts\\` inside CounterStrike16:

    python3 scripts/fleet/stage-title-subdir.py ~/ts-capture/ts CounterStrike16 ts

WHY. stage-new-title.py makes a NEW title appear whole. This does the same
for a new mod or addon folder inside a title that already exists. Uploading
1,000+ files straight to their final names would let a box that is syncing the
title (GAMESYNC START from autodeploy, an operator, a fresh image) copy a
half-uploaded folder, and GAMESYNC never deletes, so the partial copy stays.

  1. refuse if <Title> does not exist, or <Title>/<subdir> already exists
     (case-insensitive). This only ADDS; change existing files with their
     generator or sharewrite;
  2. upload to Files/Games-Library/_incoming/<Title>__<subdir>/<subdir>/...
     ('_' dirs are skipped by the agent), one verified sharewrite put per file,
     resumable;
  3. verify the whole incoming folder through /mnt: count, bytes and every md5;
  4. ONE server-side rename into Files/Games-Library/<Title>/<subdir>;
  5. verify the final folder the same way, and that the incoming dir is gone.

This adds no top-level directory and does not bump _deploy_generation.txt, so
autodeploy does not react. Push it to boxes with GAMESYNC RESET + START.
"""
import argparse
import importlib.util
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import sharewrite  # noqa: E402

_spec = importlib.util.spec_from_file_location("stage_new_title", os.path.join(HERE, "stage-new-title.py"))
snt = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(snt)

MNT = sharewrite.MNT
LIB_REL = snt.LIB_REL
INCOMING = snt.INCOMING
SUBDIR = re.compile(r'^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$')


def plan(lib, title, subdir):
    """Return an error string, or None when the add is allowed."""
    if not snt.NAME.match(title) or title.startswith('_'):
        return 'not a plain title name: %r' % title
    if not SUBDIR.match(subdir):
        return 'not a plain subdirectory name: %r' % subdir
    if not snt.exists_ci(lib, title):
        return '%s is not a library title - use stage-new-title.py for a new title' % title
    real = next(e for e in os.listdir(lib) if e.lower() == title.lower())
    if real != title:
        return 'title is spelled %r on the share, not %r' % (real, title)
    if snt.exists_ci(os.path.join(lib, title), subdir):
        return '%s/%s already exists (case-insensitive) - this only adds new folders' % (title, subdir)
    return None


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('tree')
    ap.add_argument('title')
    ap.add_argument('subdir')
    ap.add_argument('--dry-run', action='store_true')
    a = ap.parse_args(argv)
    lib = os.path.join(MNT, *LIB_REL.split('/'))
    err = plan(lib, a.title, a.subdir)
    if err:
        raise SystemExit(err)
    files = snt.local_files(a.tree)
    if not files:
        raise SystemExit('no files under %s' % a.tree)
    size = sum(os.path.getsize(p) for _, p in files)
    stage = '%s__%s' % (a.title, a.subdir)
    inc_rel = '%s/%s/%s/%s' % (LIB_REL, INCOMING, stage, a.subdir)
    final_rel = '%s/%s/%s' % (LIB_REL, a.title, a.subdir)
    print('%s/%s: %d file(s), %.1f MB -> %s' % (a.title, a.subdir, len(files), size / 1048576.0, inc_rel))
    if a.dry_run:
        return 0
    inc_mnt = os.path.join(MNT, *inc_rel.split('/'))
    final_mnt = os.path.join(MNT, *final_rel.split('/'))
    with sharewrite.Auth() as auth:
        for i, (rel, p) in enumerate(files, 1):
            q = os.path.join(inc_mnt, *rel.split('/'))
            if os.path.isfile(q) and os.path.getsize(q) == os.path.getsize(p) and snt.md5_file(q) == snt.md5_file(p):
                continue
            if sharewrite.put(p, '%s/%s' % (inc_rel, rel), auth, log=lambda m: None) != 0:
                raise SystemExit('PUBLISH FAILED at %s (%d of %d) - nothing is live; re-run to resume'
                                 % (rel, i, len(files)))
            if i % 50 == 0 or i == len(files):
                print('  %d/%d uploaded' % (i, len(files)))
        bad = snt.verify_tree(inc_mnt, files, print)
        if bad:
            raise SystemExit('the incoming folder is not whole - NOT promoted: %s' % bad[:10])
        rc, out = sharewrite.smb(auth, 'rename %s %s' % (sharewrite.smb_quote(inc_rel),
                                                         sharewrite.smb_quote(final_rel)))
        deadline = time.time() + 30
        while time.time() < deadline and not os.path.isdir(final_mnt):
            time.sleep(2)
        if not os.path.isdir(final_mnt):
            raise SystemExit('rename did not show up through /mnt: %s' % out)
        stage_rel = '%s/%s/%s' % (LIB_REL, INCOMING, stage)
        sharewrite.smb(auth, 'rmdir %s' % sharewrite.smb_quote(stage_rel))
    bad = snt.verify_tree(final_mnt, files, print)
    if bad or os.path.isdir(inc_mnt):
        raise SystemExit('PROMOTED BUT NOT CLEAN: %s' % bad[:10])
    print('LIVE: %s - push it with GAMESYNC RESET + START; run scripts/validate-staged-library.py' % final_rel)
    return 0


if __name__ == '__main__':
    sys.exit(main())
