#!/usr/bin/env python3
"""Mirror Hugging Face model repos onto the NAS - every file verified twice.

    python3 scripts/models/hfmirror.py add <org/repo> [--exclude GLOB ...] [--note TEXT]
    python3 scripts/models/hfmirror.py run             # work the queue (resumable)
    python3 scripts/models/hfmirror.py status [--json]
    python3 scripts/models/hfmirror.py plan <org/repo> [--exclude GLOB ...]   # size only

WHERE IT WRITES - and nowhere else:
    \\\\192.168.1.122\\files\\Files\\AI-Models\\<org>\\<repo>\\
`dest_path()` refuses anything outside Files/AI-Models. That guard is not
tidiness: a new top-level directory under Files/Games-Library is a new "title",
and retro-autodeploy answers one with GAMESYNC RESET + START on every box.

HOW A FILE IS PROVEN (CLAUDE.md "WRITING TO THE SHARE: VERIFY EACH FILE"):
  1. downloaded from the PINNED commit (resolve/<sha>/...), with its hash taken
     on the fly and compared with what Hugging Face publishes for it - the LFS
     SHA-256, or the git blob SHA-1 for a small non-LFS file;
  2. written to the share with smbclient (vault creds via sharewrite.Auth);
  3. read back through the READ-ONLY /mnt/retro-share mount - a different
     client from the one that wrote it - and hashed again.
Only then is the staging copy deleted and the file recorded. A model folder
gets SHA256SUMS and SOURCE.txt once every file in it has passed, so the NAS
copy can be re-checked later with `sha256sum -c SHA256SUMS`.

THE NAS IS SHARED WITH THE GAME LIBRARY AND THE BACKUPS, so the run keeps a
free-space floor (HFMIRROR_NAS_FLOOR_GB, default 500) and STOPS, loudly, rather
than cross it. State: ~/.retro-fleet/hfmirror/ (queue.json, state.json,
status.json, hfmirror.log, staging/). A run re-reads queue.json between repos,
so `add` is safe while `run` is going.
"""
import argparse
import datetime
import fcntl
import fnmatch
import hashlib
import http.client
import json
import os
import queue
import signal
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "fleet"))
import sharewrite  # noqa: E402  (Auth, smb_quote, MNT, SERVER, put)

ROOT_SHARE = "Files/AI-Models"
MNT = sharewrite.MNT
STATE_DIR = os.path.expanduser(os.environ.get("HFMIRROR_STATE", "~/.retro-fleet/hfmirror"))
QUEUE = os.path.join(STATE_DIR, "queue.json")
STATE = os.path.join(STATE_DIR, "state.json")
STATUS = os.path.join(STATE_DIR, "status.json")
LOG = os.path.join(STATE_DIR, "hfmirror.log")
STAGING = os.path.join(STATE_DIR, "staging")
GB = 10 ** 9
NAS_FLOOR = int(os.environ.get("HFMIRROR_NAS_FLOOR_GB", "500")) * GB
LOCAL_FLOOR = int(os.environ.get("HFMIRROR_LOCAL_FLOOR_GB", "200")) * GB
UA = {"User-Agent": "hfmirror/1.0"}
CHUNK = 8 << 20
MAX_TRIES = 3          # per file, per stage
NET_RETRIES = 40       # transient network errors inside one download
STAGE_DEPTH = 3        # files waiting between stages (bounds staging disk use)

_log_lock = threading.Lock()


def log(msg):
    line = "%s %s" % (datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S"), msg)
    with _log_lock:
        print(line, flush=True)
        os.makedirs(STATE_DIR, exist_ok=True)
        with open(LOG, "a") as f:
            f.write(line + "\n")


def banner(msg):
    bar = "!" * 72
    log(bar)
    log("!!! " + msg)
    log(bar)


# ---------------------------------------------------------------- paths

def _clean_component(p):
    if not p or p in (".", "..") or any(c in p for c in '\\";\0') or p != p.strip():
        raise ValueError("unsafe path component: %r" % p)
    return p


def repo_dir(repo):
    """'org/name' -> 'Files/AI-Models/org/name'."""
    parts = repo.split("/")
    if len(parts) != 2:
        raise ValueError("repo must be org/name: %r" % repo)
    org, name = (_clean_component(p) for p in parts)
    return "%s/%s/%s" % (ROOT_SHARE, org, name)


def dest_path(repo, rfilename):
    """Share-relative destination of one repo file. Never outside ROOT_SHARE."""
    parts = rfilename.replace("\\", "/").split("/")
    for p in parts:
        _clean_component(p)
    full = repo_dir(repo) + "/" + "/".join(parts)
    if not full.startswith(ROOT_SHARE + "/") or "/../" in "/" + full + "/":
        raise ValueError("refusing destination outside %s: %r" % (ROOT_SHARE, full))
    return full


def mnt_path(share_rel):
    return os.path.join(MNT, *share_rel.split("/"))


def excluded(name, patterns):
    return any(fnmatch.fnmatch(name, p) for p in patterns or ())


# ---------------------------------------------------------------- hashes

class Hasher:
    """The hash Hugging Face publishes for a file: LFS SHA-256, else git blob SHA-1.
    Always computes SHA-256 too, for SHA256SUMS."""

    def __init__(self, f):
        self.sha256 = hashlib.sha256()
        self.want = f.get("sha256") or f.get("blob")
        self.kind = "sha256" if f.get("sha256") else "git-sha1"
        self.git = None
        if self.kind == "git-sha1":
            self.git = hashlib.sha1()
            self.git.update(b"blob %d\0" % f["size"])

    def update(self, b):
        self.sha256.update(b)
        if self.git:
            self.git.update(b)

    def got(self):
        return self.sha256.hexdigest() if self.kind == "sha256" else self.git.hexdigest()

    def ok(self):
        return bool(self.want) and self.got() == self.want


def hash_file(path, f, progress=None):
    h = Hasher(f)
    with open(path, "rb") as fh:
        n = 0
        for chunk in iter(lambda: fh.read(CHUNK), b""):
            h.update(chunk)
            n += len(chunk)
            if progress:
                progress(len(chunk))
        try:
            os.posix_fadvise(fh.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
        except (AttributeError, OSError):
            pass
    return h


# ---------------------------------------------------------------- HF API

def _open(url, headers=None, timeout=60):
    return urllib.request.urlopen(urllib.request.Request(url, headers=dict(UA, **(headers or {}))),
                                  timeout=timeout)


def api_tree(repo, revision="main"):
    url = "https://huggingface.co/api/models/%s/revision/%s?blobs=true" % (repo, revision)
    for i in range(6):
        try:
            with _open(url) as r:
                j = json.load(r)
            break
        except (urllib.error.URLError, OSError) as e:
            if isinstance(e, urllib.error.HTTPError) and e.code in (401, 403, 404):
                raise SystemExit("%s: HTTP %d (gated, private or missing)" % (repo, e.code))
            time.sleep(5 * (i + 1))
    else:
        raise SystemExit("%s: API unreachable" % repo)
    files = []
    for s in j.get("siblings", []):
        lfs = s.get("lfs")
        files.append({"name": s["rfilename"],
                      "size": (lfs or {}).get("size", s.get("size")),
                      "sha256": (lfs or {}).get("sha256"),
                      "blob": s.get("blobId")})
    card = j.get("cardData") or {}
    meta = {"sha": j["sha"], "license": card.get("license"),
            "license_name": card.get("license_name"),
            "last_modified": j.get("lastModified"), "gated": j.get("gated")}
    return meta, files


# ---------------------------------------------------------------- state

class Store:
    def __init__(self):
        self.lock = threading.Lock()
        self.data = {"items": {}}
        if os.path.exists(STATE):
            with open(STATE) as f:
                self.data = json.load(f)

    def save(self):
        with self.lock:
            tmp = STATE + ".tmp"
            with open(tmp, "w") as f:
                json.dump(self.data, f, indent=1, sort_keys=True)
            os.replace(tmp, STATE)

    def item(self, repo):
        with self.lock:
            return self.data["items"].setdefault(repo, {"files": {}, "failed": {}})

    def verified(self, repo, name):
        return name in self.item(repo)["files"]

    def mark(self, repo, name, sha256, size):
        it = self.item(repo)
        with self.lock:
            it["files"][name] = {"sha256": sha256, "size": size,
                                 "at": datetime.datetime.now().isoformat(timespec="seconds")}
            it["failed"].pop(name, None)
        self.save()

    def fail(self, repo, name, why):
        it = self.item(repo)
        with self.lock:
            it["failed"][name] = why
        self.save()


def load_queue():
    if not os.path.exists(QUEUE):
        return {"items": []}
    with open(QUEUE) as f:
        return json.load(f)


def save_queue(q):
    os.makedirs(STATE_DIR, exist_ok=True)
    tmp = QUEUE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(q, f, indent=1)
    os.replace(tmp, QUEUE)


# ---------------------------------------------------------------- NAS

def nas_free():
    st = os.statvfs(MNT)
    return st.f_bavail * st.f_frsize


def local_free():
    os.makedirs(STAGING, exist_ok=True)
    st = os.statvfs(STAGING)
    return st.f_bavail * st.f_frsize


def smb(auth, commands, timeout_s=300):
    r = subprocess.run(["smbclient", sharewrite.SERVER, "-A", auth, "-t", str(timeout_s),
                        "-c", commands], capture_output=True, text=True)
    out = r.stdout + r.stderr
    return r.returncode, "\n".join(l for l in out.splitlines() if "password" not in l.lower())


class Nas:
    def __init__(self, auth):
        self.auth = auth
        self.made = set()

    def put(self, local, dest):
        dirs, name = dest.split("/")[:-1], dest.split("/")[-1]
        cmds, path = [], ""
        for d in dirs:
            path = d if not path else path + "/" + d
            if path not in self.made:
                cmds.append("mkdir %s" % sharewrite.smb_quote(path))
        cmds.append("cd %s" % sharewrite.smb_quote("/".join(dirs)))
        cmds.append("put %s %s" % (sharewrite.smb_quote(local), sharewrite.smb_quote(name)))
        rc, out = smb(self.auth, "; ".join(cmds))
        bad = [l for l in out.splitlines()
               if "NT_STATUS_" in l and "NT_STATUS_OBJECT_NAME_COLLISION" not in l]
        ok = "putting file" in out and not bad
        if ok:
            self.made.update("/".join(dirs[:i + 1]) for i in range(len(dirs)))
        return ok, out

    def delete(self, dest):
        dirs, name = dest.split("/")[:-1], dest.split("/")[-1]
        smb(self.auth, "cd %s; del %s" % (sharewrite.smb_quote("/".join(dirs)),
                                         sharewrite.smb_quote(name)))

    @staticmethod
    def verify(f, dest, progress=None, wait_s=60):
        """Read the share copy back through /mnt and hash it. -> (ok, why)"""
        p = mnt_path(dest)
        deadline = time.time() + wait_s
        while True:
            try:
                if os.path.getsize(p) == f["size"]:
                    break
            except OSError:
                pass
            if time.time() >= deadline:
                try:
                    return False, "size %s on the share, want %d" % (os.path.getsize(p), f["size"])
                except OSError:
                    return False, "absent on the share"
            time.sleep(2)
        h = hash_file(p, f, progress)
        if h.ok():
            return True, h.sha256.hexdigest()
        return False, "read back as %s %s, want %s" % (h.kind, h.got(), h.want)


# ---------------------------------------------------------------- pipeline

class Rate:
    def __init__(self):
        self.lock = threading.Lock()
        self.total = 0
        self.samples = []

    def add(self, n):
        with self.lock:
            self.total += n
            now = time.time()
            self.samples.append((now, self.total))
            while self.samples and now - self.samples[0][0] > 60:
                self.samples.pop(0)

    def per_s(self):
        with self.lock:
            if len(self.samples) < 2:
                return 0.0
            (t0, b0), (t1, b1) = self.samples[0], self.samples[-1]
            return (b1 - b0) / max(t1 - t0, 1e-6)


class Runner:
    def __init__(self, store, nas):
        self.store, self.nas = store, nas
        self.upq = queue.Queue(maxsize=STAGE_DEPTH)
        self.vq = queue.Queue(maxsize=STAGE_DEPTH)
        self.stop = threading.Event()
        self.stop_reason = None
        self.inflight = 0              # bytes downloaded or downloading, not yet on the share
        self.lock = threading.Lock()
        self.rates = {"download": Rate(), "upload": Rate(), "verify": Rate()}
        self.current = {"download": None, "upload": None, "verify": None}
        self.items = {}                # repo -> {"meta", "files"}
        self.started = time.time()

    # ---- helpers
    def halt(self, why):
        if not self.stop.is_set():
            self.stop_reason = why
            banner("STOPPED: " + why)
            self.stop.set()

    def staging_path(self, repo, name):
        return os.path.join(STAGING, *repo.split("/"), *name.split("/"))

    def space_ok(self, size):
        with self.lock:
            need = self.inflight + size
        if nas_free() - need < NAS_FLOOR:
            self.halt("NAS free space would fall below the %d GB floor (free %.0f GB, "
                      "%.0f GB more to place) - nothing else will be downloaded"
                      % (NAS_FLOOR // GB, nas_free() / GB, need / GB))
            return False
        if local_free() - size < LOCAL_FLOOR:
            self.halt("local staging disk below its %d GB floor" % (LOCAL_FLOOR // GB))
            return False
        return True

    # ---- stage 1: download + hash
    def download(self, repo, sha, f):
        local = self.staging_path(repo, f["name"])
        part = local + ".part"
        os.makedirs(os.path.dirname(local), exist_ok=True)
        if os.path.exists(local):
            h = hash_file(local, f)
            if h.ok():
                return local
            os.unlink(local)
        url = "https://huggingface.co/%s/resolve/%s/%s" % (repo, sha, urllib.parse.quote(f["name"]))
        h = Hasher(f)
        have = 0
        if os.path.exists(part):
            have = os.path.getsize(part)
            if have > f["size"]:
                os.unlink(part)
                have = 0
            elif have:
                with open(part, "rb") as fh:
                    for chunk in iter(lambda: fh.read(CHUNK), b""):
                        h.update(chunk)
        errors = 0
        with open(part, "ab") as out:
            while have < f["size"]:
                if self.stop.is_set():
                    return None
                try:
                    hdr = {"Range": "bytes=%d-" % have} if have else {}
                    with _open(url, hdr, timeout=90) as r:
                        if have and r.status != 206:
                            raise IOError("server ignored the Range request")
                        while True:
                            b = r.read(CHUNK)
                            if not b:
                                break
                            out.write(b)
                            h.update(b)
                            have += len(b)
                            self.rates["download"].add(len(b))
                    if have < f["size"]:
                        raise IOError("connection ended at %d of %d" % (have, f["size"]))
                except urllib.error.HTTPError as e:
                    if e.code in (401, 403, 404, 416):
                        raise IOError("HTTP %d for %s" % (e.code, f["name"]))
                    errors += 1
                    if errors > NET_RETRIES:
                        raise IOError("HTTP %d (%d retries)" % (e.code, errors))
                    out.flush()
                    time.sleep(min(60, 3 * errors))
                except (urllib.error.URLError, OSError, http.client.HTTPException) as e:
                    errors += 1
                    if errors > NET_RETRIES:
                        raise IOError("network: %s (%d retries)" % (e, errors))
                    out.flush()
                    time.sleep(min(60, 3 * errors))
            try:
                os.posix_fadvise(out.fileno(), 0, 0, os.POSIX_FADV_DONTNEED)
            except (AttributeError, OSError):
                pass
        if have != f["size"] or not h.ok():
            os.unlink(part)
            raise IOError("download of %s does not match Hugging Face's %s (got %s)"
                          % (f["name"], h.kind, h.got()))
        os.replace(part, local)
        return local

    def downloader(self):
        try:
            for repo in self.iterate_repos():
                if self.stop.is_set():
                    break
                meta, files = self.items[repo]["meta"], self.items[repo]["files"]
                for f in files:
                    if self.stop.is_set():
                        break
                    if self.store.verified(repo, f["name"]):
                        continue
                    dest = dest_path(repo, f["name"])
                    try:
                        on_share = os.path.getsize(mnt_path(dest))
                    except OSError:
                        on_share = None
                    if on_share == f["size"]:
                        # landed before a crash/restart but never recorded: prove it
                        self.vq.put(("file", repo, f, None, dest))
                        continue
                    if not self.space_ok(f["size"]):
                        break
                    with self.lock:
                        self.inflight += f["size"]
                    self.current["download"] = "%s/%s" % (repo, f["name"])
                    local = None
                    for attempt in range(1, MAX_TRIES + 1):
                        try:
                            local = self.download(repo, meta["sha"], f)
                            break
                        except IOError as e:
                            log("download try %d/%d failed: %s/%s: %s"
                                % (attempt, MAX_TRIES, repo, f["name"], e))
                    self.current["download"] = None
                    if not local:
                        with self.lock:
                            self.inflight -= f["size"]
                        if not self.stop.is_set():
                            self.store.fail(repo, f["name"], "download failed %d times" % MAX_TRIES)
                        continue
                    self.upq.put(("file", repo, f, local, dest))
                self.upq.put(("end", repo, None, None, None))
        finally:
            self.upq.put(None)

    # ---- stage 2: smbclient put
    def uploader(self):
        while True:
            job = self.upq.get()
            if job is None:
                self.vq.put(None)
                return
            kind, repo, f, local, dest = job
            if kind == "file":
                self.current["upload"] = dest
                ok, out = False, ""
                for attempt in range(1, MAX_TRIES + 1):
                    t0 = time.time()
                    ok, out = self.nas.put(local, dest)
                    if ok:
                        self.rates["upload"].add(f["size"])
                        break
                    log("upload try %d/%d failed after %.0f s: %s\n%s"
                        % (attempt, MAX_TRIES, time.time() - t0, dest, out[-800:]))
                    if "NT_STATUS_DISK_FULL" in out:
                        self.halt("the NAS reported DISK FULL")
                        break
                    time.sleep(10)
                self.current["upload"] = None
                if not ok:
                    self.store.fail(repo, f["name"], "smbclient put failed")
                    self.done_with(local, f)
                    continue
            self.vq.put(job)

    # ---- stage 3: read back through /mnt
    def verifier(self):
        while True:
            job = self.vq.get()
            if job is None:
                return
            kind, repo, f, local, dest = job
            if kind == "barrier":
                repo.set()          # the event: everything queued before it is done
                continue
            if kind == "end":
                self.finish_item(repo)
                continue
            self.current["verify"] = dest
            ok, why = Nas.verify(f, dest, self.rates["verify"].add)
            tries = 1
            while not ok and local and tries < MAX_TRIES:
                log("verify FAILED (%s): %s - writing it again" % (why, dest))
                put_ok, out = self.nas.put(local, dest)
                if not put_ok:
                    log(out[-800:])
                ok, why = Nas.verify(f, dest, self.rates["verify"].add)
                tries += 1
            self.current["verify"] = None
            if ok:
                self.store.mark(repo, f["name"], why, f["size"])
                log("verified %s (%.2f GB)" % (dest, f["size"] / GB))
            else:
                banner("FILE FAILED VERIFICATION: %s - %s" % (dest, why))
                if local is None:
                    # an unrecorded file on the share that does not hash right: remove it so
                    # the next run downloads it instead of trusting its size
                    self.nas.delete(dest)
                self.store.fail(repo, f["name"], why)
            self.done_with(local, f)

    def done_with(self, local, f):
        if local:
            try:
                os.unlink(local)
            except OSError:
                pass
            with self.lock:
                self.inflight -= f["size"]

    # ---- queue walking
    def iterate_repos(self):
        """Yield queued repos in order, re-reading queue.json after each, plus up to
        two retry passes over repos that ended incomplete."""
        seen = set()
        while True:
            pending = [it for it in load_queue()["items"] if it["repo"] not in seen]
            if not pending:
                break
            it = pending[0]
            seen.add(it["repo"])
            try:
                ready = self.prepare(it)
            except (SystemExit, Exception) as e:
                banner("SKIPPING %s: %s" % (it["repo"], e))
                ready = False
            if ready:
                yield it["repo"]
        for _ in range(2):
            self.drain()
            redo = [r for r in seen if r in self.items and not self.complete(r)
                    and not self.stop.is_set()]
            if not redo:
                break
            log("retry pass over %d incomplete repo(s): %s" % (len(redo), ", ".join(redo)))
            for r in redo:
                yield r

    def drain(self):
        """Block until every job queued so far has been uploaded and verified."""
        ev = threading.Event()
        self.upq.put(("barrier", ev, None, None, None))
        while not ev.wait(30):
            pass

    def prepare(self, it):
        repo = it["repo"]
        st = self.store.item(repo)
        meta, files = api_tree(repo, st.get("sha") or it.get("revision") or "main")
        if st.get("sha") and st["sha"] != meta["sha"]:
            raise SystemExit("%s: pinned %s but the API answered %s" % (repo, st["sha"], meta["sha"]))
        kept = [f for f in files if not excluded(f["name"], it.get("exclude"))]
        dropped = [f["name"] for f in files if excluded(f["name"], it.get("exclude"))]
        for f in kept:
            dest_path(repo, f["name"])          # raises on anything unsafe, before any write
        with self.store.lock:
            st.update({"sha": meta["sha"], "license": meta["license"],
                       "license_name": meta["license_name"], "note": it.get("note"),
                       "exclude": it.get("exclude") or [], "excluded_files": dropped,
                       "total_bytes": sum(f["size"] for f in kept), "file_count": len(kept),
                       "queued": it.get("added")})
        self.store.save()
        self.items[repo] = {"meta": meta, "files": kept}
        log("repo %s @ %s: %d file(s), %.1f GB (%d excluded)"
            % (repo, meta["sha"][:12], len(kept), st["total_bytes"] / GB, len(dropped)))
        return True

    def complete(self, repo):
        st = self.store.item(repo)
        return all(f["name"] in st["files"] for f in self.items[repo]["files"])

    def finish_item(self, repo):
        if not self.complete(repo):
            st = self.store.item(repo)
            missing = [f["name"] for f in self.items[repo]["files"] if f["name"] not in st["files"]]
            banner("%s INCOMPLETE: %d file(s) not verified (%s)"
                   % (repo, len(missing), ", ".join(missing[:5])))
            return
        st = self.store.item(repo)
        if st.get("complete_sha") == st["sha"]:
            return
        files = self.items[repo]["files"]
        sums = "".join("%s  %s\n" % (st["files"][f["name"]]["sha256"], f["name"])
                       for f in sorted(files, key=lambda f: f["name"]))
        lic = st.get("license_name") or st.get("license") or "see the model card"
        src = "\r\n".join([
            "repo:      %s" % repo,
            "source:    https://huggingface.co/%s" % repo,
            "revision:  %s (pinned commit; tree as of %s)" % (st["sha"], self.items[repo]["meta"].get("last_modified")),
            "license:   %s" % lic,
            "files:     %d, %.1f GB" % (len(files), st["total_bytes"] / GB),
            "excluded:  %s" % (", ".join(st.get("exclude") or []) or "nothing"),
            "note:      %s" % (st.get("note") or ""),
            "mirrored:  %s by scripts/models/hfmirror.py (retro-agent repo)" % datetime.date.today(),
            "",
            "Every file was hashed against the value Hugging Face publishes for it when it",
            "was downloaded, and hashed again after being read back from this share.",
            "Re-check this copy at any time:   sha256sum -c SHA256SUMS",
            ""])
        tmp = os.path.join(STATE_DIR, "meta", *repo.split("/"))
        os.makedirs(tmp, exist_ok=True)
        ok = True
        for name, body in (("SHA256SUMS", sums), ("SOURCE.txt", src)):
            p = os.path.join(tmp, name)
            with open(p, "w", newline="") as fh:
                fh.write(body)
            # mtime does not matter here (nothing GAMESYNCs this folder): 0 or 4 is landed
            rc = sharewrite.put(p, repo_dir(repo) + "/" + name, self.nas.auth, log=log)
            ok = ok and rc in (0, 4)
        if ok:
            with self.store.lock:
                st["complete_sha"] = st["sha"]
                st["completed"] = datetime.datetime.now().isoformat(timespec="seconds")
            self.store.save()
            log("COMPLETE: %s (%d files, %.1f GB) - SHA256SUMS + SOURCE.txt on the share"
                % (repo, len(files), st["total_bytes"] / GB))
            self.write_index()

    def write_index(self):
        rows = []
        with self.store.lock:
            snapshot = json.loads(json.dumps(self.store.data["items"]))
        for repo, st in sorted(snapshot.items()):
            if st.get("complete_sha"):
                rows.append("%-60s %8.1f GB  %-14s %s  %s" % (
                    repo, st["total_bytes"] / GB, (st.get("license") or "?")[:14],
                    st["sha"][:12], st.get("note") or ""))
        body = "\r\n".join([
            "AI model weights mirrored from Hugging Face",
            "===========================================",
            "",
            "Each folder is <org>\\<repo> exactly as on huggingface.co, at the pinned commit",
            "named in its SOURCE.txt. SHA256SUMS in each folder lists every file; re-check",
            "a copy with:  sha256sum -c SHA256SUMS",
            "",
            "Written by scripts/models/hfmirror.py in the retro-agent repo, which verifies",
            "every file against Hugging Face's published hash on download and again after",
            "reading it back from this share. Updated %s." % datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
            "",
            "%-60s %11s  %-14s %-12s  %s" % ("repo", "size", "license", "revision", "note"),
            "-" * 110] + rows + [""])
        p = os.path.join(STATE_DIR, "meta", "README.txt")
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", newline="") as fh:
            fh.write(body)
        sharewrite.put(p, ROOT_SHARE + "/README.txt", self.nas.auth, log=log)

    # ---- status
    def status_loop(self):
        while True:
            try:
                self.write_status()
            except Exception as e:      # a status glitch must never end the run
                log("status write failed: %r" % e)
            if self.done.is_set():
                return
            self.done.wait(15)

    def write_status(self):
        items = []
        q = load_queue()["items"]
        with self.store.lock:
            for it in q:
                st = self.store.data["items"].get(it["repo"], {})
                vb = sum(v["size"] for v in st.get("files", {}).values())
                items.append({"repo": it["repo"], "total_gb": round(st.get("total_bytes", 0) / GB, 1),
                              "verified_gb": round(vb / GB, 1), "files": st.get("file_count"),
                              "verified_files": len(st.get("files", {})),
                              "failed": dict(st.get("failed", {})),
                              "complete": bool(st.get("complete_sha"))})
        left = sum(max(i["total_gb"] - i["verified_gb"], 0) for i in items)
        rate = self.rates["verify"].per_s()
        s = {"updated": datetime.datetime.now().isoformat(timespec="seconds"),
             "running": not self.done.is_set(), "stopped": self.stop_reason,
             "current": self.current,
             "mb_s": {k: round(v.per_s() / 1e6, 1) for k, v in self.rates.items()},
             "nas_free_gb": round(nas_free() / GB), "nas_floor_gb": NAS_FLOOR // GB,
             "local_free_gb": round(local_free() / GB),
             "remaining_gb": round(left, 1),
             "eta_h": round(left * GB / rate / 3600, 1) if rate > 0 else None,
             "items": items}
        tmp = STATUS + ".tmp"
        with open(tmp, "w") as f:
            json.dump(s, f, indent=1)
        os.replace(tmp, STATUS)

    def run(self):
        self.done = threading.Event()
        threads = [threading.Thread(target=t, name=t.__name__, daemon=True)
                   for t in (self.downloader, self.uploader, self.verifier)]
        mon = threading.Thread(target=self.status_loop, daemon=True)
        mon.start()
        for t in threads:
            t.start()
        for t in threads:
            t.join()
        self.done.set()
        mon.join()
        with self.store.lock:
            bad = {r: dict(st["failed"]) for r, st in self.store.data["items"].items() if st.get("failed")}
        if bad:
            banner("RUN ENDED WITH FAILURES: %s" % json.dumps(bad)[:2000])
        if self.stop_reason:
            banner("RUN STOPPED EARLY: %s" % self.stop_reason)
        return 1 if (bad or self.stop_reason) else 0


# ---------------------------------------------------------------- commands

def cmd_add(a):
    q = load_queue()
    for repo in a.repo:
        repo_dir(repo)
        if any(it["repo"] == repo for it in q["items"]):
            print("already queued: %s" % repo)
            continue
        q["items"].append({"repo": repo, "exclude": a.exclude or [], "note": a.note,
                           "added": datetime.datetime.now().isoformat(timespec="seconds")})
        print("queued: %s" % repo)
    save_queue(q)
    return 0


def cmd_plan(a):
    tot = 0
    for repo in a.repo:
        meta, files = api_tree(repo)
        kept = [f for f in files if not excluded(f["name"], a.exclude)]
        b = sum(f["size"] for f in kept)
        tot += b
        print("%-60s %8.1f GB  %4d files  license=%s  gated=%s"
              % (repo, b / GB, len(kept), meta["license"], meta["gated"]))
    print("total %.1f GB; NAS free %.0f GB (floor %d GB)" % (tot / GB, nas_free() / GB, NAS_FLOOR // GB))
    return 0


def cmd_status(a):
    if not os.path.exists(STATUS):
        print("no status yet")
        return 1
    with open(STATUS) as f:
        s = json.load(f)
    if a.json:
        print(json.dumps(s, indent=1))
        return 0
    print("updated %s  running=%s  stopped=%s" % (s["updated"], s["running"], s["stopped"]))
    print("MB/s %s   NAS free %s GB (floor %s)   remaining %s GB   ETA %s h"
          % (s["mb_s"], s["nas_free_gb"], s["nas_floor_gb"], s["remaining_gb"], s["eta_h"]))
    for k, v in s["current"].items():
        if v:
            print("  %-8s %s" % (k, v))
    for i in s["items"]:
        mark = "DONE" if i["complete"] else ("FAIL" if i["failed"] else "    ")
        print("  %s %-58s %7.1f / %7.1f GB  %s/%s files%s" % (
            mark, i["repo"], i["verified_gb"], i["total_gb"], i["verified_files"], i["files"],
            ("  failed: %d" % len(i["failed"])) if i["failed"] else ""))
    return 0


def cmd_run(a):
    os.makedirs(STATE_DIR, exist_ok=True)
    lockf = open(os.path.join(STATE_DIR, "run.lock"), "w")
    try:
        fcntl.flock(lockf, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        raise SystemExit("another hfmirror run holds %s" % lockf.name)
    if not os.path.ismount(MNT):
        raise SystemExit("%s is not mounted - cannot verify anything" % MNT)
    log("run start: NAS free %.0f GB, floor %d GB; staging %s (%.0f GB free)"
        % (nas_free() / GB, NAS_FLOOR // GB, STAGING, local_free() / GB))
    # systemd stops with SIGTERM: unwind, so Auth deletes its 0600 credentials file
    signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
    with sharewrite.Auth() as auth:
        return Runner(Store(), Nas(auth)).run()


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("add")
    p.add_argument("repo", nargs="+")
    p.add_argument("--exclude", action="append", help="fnmatch glob over repo paths")
    p.add_argument("--note")
    p = sub.add_parser("plan")
    p.add_argument("repo", nargs="+")
    p.add_argument("--exclude", action="append")
    sub.add_parser("run")
    p = sub.add_parser("status")
    p.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    return {"add": cmd_add, "plan": cmd_plan, "run": cmd_run, "status": cmd_status}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
