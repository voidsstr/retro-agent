#!/usr/bin/env python3
"""Where a generator's BINARY payload comes from - and proof it is that file.

A generator that stages a binary into the library (a no-CD exe, a DLL the Win95
build imports, a copy of DOSBox for a second title) must not take "the file at
that path" on trust: the share holds two byte-identical-sized builds of more
than one exe (Doom 3's official 1.3 and a scene crack are both 5,832,704
bytes), and a re-staged archive can change under a path. So every source names
its md5, and fetch() refuses anything else. Nothing here writes anything.

Source specs (dicts):

  {"lib":  "Descent1/DOSBOX/DOSBox.exe", "md5": ...}
        a file in the staged library, looked up CASE-INSENSITIVELY (a Windows
        tree read from Linux - CLAUDE.md "Search Windows Trees CASE-INSENSITIVELY")
  {"share": "Files/Games/DOS/.../x.zip", "md5": ...}
        any file on the share, the same way
  {"zip": "<share path>", "member": "CRACK/DESCENT2.EXE", "md5": ...}
        a zip member; "a.zip/B.OVL" in member reads a zip INSIDE the zip
  {"iso": "<share path>", "member": "D2DATA/DESCENT2.SOW", "arj": "IFORCE.DLL", "md5": ...}
        an ISO9660 member (read with 7z), optionally a member of an ARJ archive
        inside it (7z -tarj)
  {"repo": "scripts/dosgames/files/x.txt", "md5": optional}
        a file committed to this repo (our own text: a pinned md5 is optional)
"""
import hashlib
import io
import os
import shutil
import subprocess
import tempfile
import zipfile

MNT = "/mnt/retro-share"
LIBRARY_REL = "Files/Games-Library"
REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))


class SourceError(RuntimeError):
    """The source is missing, unreadable, or not the pinned file."""


def find_ci_path(base, rel):
    """`rel` ('/'- or '\\'-separated) under `base`, matched case-insensitively
    one component at a time; None when any component is absent."""
    cur = base
    for part in rel.replace("\\", "/").split("/"):
        if not part:
            continue
        try:
            names = os.listdir(cur)
        except OSError:
            return None
        hit = next((n for n in names if n.lower() == part.lower()), None)
        if hit is None:
            return None
        cur = os.path.join(cur, hit)
    return cur


def md5(data):
    return hashlib.md5(data).hexdigest()


def _zip_member(data, member):
    """`member` of the zip in `data`; 'inner.zip/X' descends into a zip member."""
    parts = member.replace("\\", "/").split("/")
    zf = zipfile.ZipFile(io.BytesIO(data))
    names = {i.filename.lower(): i for i in zf.infolist() if not i.is_dir()}
    # longest prefix that is a member: "CRACK/DESCENT2.EXE" or "crack.zip"
    for k in range(len(parts), 0, -1):
        cand = "/".join(parts[:k]).lower()
        if cand in names:
            body = zf.read(names[cand])
            rest = "/".join(parts[k:])
            return _zip_member(body, rest) if rest else body
    raise SourceError("zip has no member %r" % member)


def _run7z(args):
    exe = shutil.which("7z") or shutil.which("7za")
    if not exe:
        raise SourceError("7z is not installed on this host - it reads the ISO "
                          "and ARJ sources")
    r = subprocess.run([exe] + args, capture_output=True, timeout=600)
    if r.returncode != 0:
        raise SourceError("7z %s failed: %s" % (" ".join(args[:2]),
                                                 r.stderr.decode("latin-1")[-300:]))
    return r.stdout


def _iso_member(path, member, arj=None):
    data = _run7z(["x", "-so", path, member.replace("\\", "/")])
    if not data:
        raise SourceError("%s holds no %s (7z read 0 bytes)" % (path, member))
    if not arj:
        return data
    tmp = tempfile.mkdtemp(prefix="libsource-")
    try:
        arc = os.path.join(tmp, "a.arj")
        with open(arc, "wb") as fh:
            fh.write(data)
        out = _run7z(["e", "-so", "-tarj", arc, arj])
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    if not out:
        raise SourceError("the ARJ %s inside %s holds no %s" % (member, path, arj))
    return out


def _share_file(rel, mnt):
    p = find_ci_path(mnt, rel)
    if p is None or not os.path.isfile(p):
        raise SourceError("not on the share (case-insensitive): %s" % rel)
    return p


def fetch(src, mnt=MNT, repo=REPO):
    """The bytes of `src`, verified against its md5. Raises SourceError."""
    if "lib" in src:
        path = _share_file(LIBRARY_REL + "/" + src["lib"], mnt)
        with open(path, "rb") as fh:
            data = fh.read()
    elif "share" in src:
        with open(_share_file(src["share"], mnt), "rb") as fh:
            data = fh.read()
    elif "zip" in src:
        with open(_share_file(src["zip"], mnt), "rb") as fh:
            data = _zip_member(fh.read(), src["member"])
    elif "iso" in src:
        data = _iso_member(_share_file(src["iso"], mnt), src["member"], src.get("arj"))
    elif "repo" in src:
        with open(os.path.join(repo, src["repo"]), "rb") as fh:
            data = fh.read()
    else:
        raise SourceError("unknown source kind: %r" % (src,))
    want = src.get("md5")
    if want is None and "repo" not in src:
        raise SourceError("a share source must pin its md5: %r" % (src,))
    if want is not None and md5(data) != want.lower():
        raise SourceError("%r is not the pinned file: md5 %s, expected %s"
                          % (src, md5(data), want))
    return data


def describe(src):
    """One line naming a source, for a generator's report."""
    for k in ("lib", "share", "zip", "iso", "repo"):
        if k in src:
            s = "%s:%s" % (k, src[k])
            if "member" in src:
                s += "!" + src["member"]
            if "arj" in src:
                s += "!" + src["arj"]
            return s
    return repr(src)
