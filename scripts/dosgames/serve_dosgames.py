#!/usr/bin/env python3
"""serve_dosgames.py — tiny HTTP server bridging the SMB share to DOS boxes.

mTCP HTGET on the DOS side can only speak HTTP, and the share is SMB-only, so
this serves the share's DOS games (via the gvfs mount) plus the generated
catalog and preview tiles:

    /GAMES.CAT          the catalog (regenerate with gen_catalog.py)
    /z/<STEM>           archive by 8-char install stem  <-- what DOS asks for
    /dos/<zipname>      archives by full name (legacy; see the warning below)
    /tiles/<name>.PRV   preview tiles

WHY /z/<STEM> EXISTS: DOS silently truncates a command tail at 126 bytes, and
the old client pasted the URL-encoded zip name (61 chars on average, 137 at
worst) onto the URL. 845 of the 2,982 catalogue entries therefore fetched a
chopped-off URL, got a 404, and told the user "Download failed - check the
network". The 8-char stem is a fixed-length name for the same file, so the
generated command line is always short. /dos/ is kept for older clients.

Run:  python3 serve_dosgames.py [port]      (default 8181)
Keep it running on the dev host; DOSGAME.CFG points url= at it.
"""
import http.server, os, socketserver, sys, urllib.parse

# Where the DOS archives live. Override with DOSGAMES_SHARE - the path differs
# per host: a gvfs mount on the Linux dev box, but "Z:\Files\Games\DOS" when
# this runs on the Windows side (which is what the fleet actually reaches,
# since WSL is NAT'd and a server bound inside it is invisible to the LAN).
SHARE = os.environ.get(
    "DOSGAMES_SHARE",
    "/run/user/1000/gvfs/smb-share:server=192.168.1.122,share=files/Games/DOS")
HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.environ.get("DOSGAMES_DATA", os.path.join(HERE, "data"))


def _search_dirs(root):
    """The archive directories: the root itself plus every subdirectory one
    and two levels down. Discovered rather than hard-coded, because the
    collection's layout ("DOS Games Collection/1-C", "More Dos Games", ...)
    varies between shares and a missing hard-coded name silently serves 404s.
    """
    dirs = [root]
    for depth1 in sorted(_subdirs(root)):
        dirs.append(depth1)
        dirs.extend(sorted(_subdirs(depth1)))
    return dirs


def _subdirs(path):
    try:
        return [os.path.join(path, n) for n in os.listdir(path)
                if os.path.isdir(os.path.join(path, n))]
    except OSError:
        return []


SEARCH_DIRS = _search_dirs(SHARE)

B36 = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ"


def _toupper(ch):
    """C's toupper() on one byte: ASCII only, everything else unchanged.

    Python's str.upper() would fold accented letters too, which would make the
    hash disagree with the 16-bit DOS build on any non-ASCII filename.
    """
    b = ord(ch) & 0xFF
    return b - 32 if 0x61 <= b <= 0x7A else b


def zip_stem(zipname):
    """The 8.3 directory an install lands in — MUST match dosgame.c's
    zip_stem() byte for byte, since that is the name the DOS side asks for.

    5 readable characters plus 3 base-36 characters of a 16-bit FNV-1a hash of
    the whole name. The readable part alone (the original scheme, a plain
    8-character truncation) collided for 1,268 of the 2,982 catalogue entries —
    all eleven Duke Nukem titles shared C:\\GAMES\\DUKE_NUK and unzipped over
    each other. With the hash there are no collisions across the catalogue.
    """
    h = 0x811C
    for ch in zipname:
        h ^= _toupper(ch)
        h = (h * 0x0193) & 0xFFFF
    head = ""
    for c in zipname[:5]:
        if c == ".":
            break
        u = chr(_toupper(c))
        # anything outside A-Z 0-9 - becomes '_': ',' is an argument separator
        # to COMMAND.COM and one of FAT's reserved 8.3 characters (with
        # + ; = [ ]), so it must never reach a mkdir or an UNZIP -d
        head += u if (u.isascii() and u.isalnum()) or u == "-" else "_"
    head = head.ljust(5, "_")
    tail = ""
    for _ in range(3):
        tail = B36[h % 36] + tail
        h //= 36
    return head + tail


class H(http.server.BaseHTTPRequestHandler):
    def _send_file(self, path):
        try:
            size = os.path.getsize(path)
            with open(path, "rb") as f:
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(size))
                self.end_headers()
                while True:
                    chunk = f.read(65536)
                    if not chunk: break
                    self.wfile.write(chunk)
        except OSError:
            self.send_error(404)

    def do_GET(self):
        p = urllib.parse.unquote(self.path)
        if ".." in p:
            self.send_error(400); return
        # DOSGAME.CFG's url= conventionally ends in "/dos", and the client
        # appends its own "/z/<STEM>" or "/GAMES.CAT" to it - so requests
        # arrive as "/dos/z/FOO" just as often as "/z/FOO". Accept either
        # rather than 404 on a trailing-path detail.
        if p.lower().startswith("/dos/") and "/z/" in p.lower():
            p = p[4:]
        if p.lower() in ("/dos/games.cat", "/games.cat"):
            p = "/GAMES.CAT"
        if p in ("/GAMES.CAT", "/games.cat"):
            self._send_file(os.path.join(DATA, "GAMES.CAT")); return
        if p.lower().startswith("/tiles/") or p.lower().startswith("/dos/tiles/"):
            self._send_file(os.path.join(DATA, "tiles",
                                         os.path.basename(p))); return
        if p.lower().startswith("/z/"):
            stem = os.path.basename(p).upper()
            path = self._by_stem(stem)
            if path:
                self._send_file(path); return
            self.send_error(404, "no archive with stem %s" % stem); return
        if p.lower().startswith("/dos/"):
            name = os.path.basename(p)
            for d in SEARCH_DIRS:
                cand = os.path.join(d, name)
                if os.path.isfile(cand):
                    self._send_file(cand); return
            self.send_error(404); return
        self.send_error(404)

    # stem -> full path, built once and reused; the share holds ~3,800 files
    # and re-listing it per request made DOS-side fetches time out.
    _stems = None

    @classmethod
    def _build_index(cls):
        idx = {}
        for d in SEARCH_DIRS:
            try:
                names = os.listdir(d)
            except OSError:
                continue
            for n in names:
                full = os.path.join(d, n)
                if os.path.isfile(full):
                    idx.setdefault(zip_stem(n), full)
        cls._stems = idx
        return idx

    def _by_stem(self, stem):
        idx = H._stems if H._stems is not None else H._build_index()
        hit = idx.get(stem)
        if hit and os.path.isfile(hit):
            return hit
        # a miss may just mean the share gained files since we indexed
        idx = H._build_index()
        return idx.get(stem)

    def log_message(self, fmt, *a):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % a))

class Server(socketserver.ThreadingTCPServer):
    # Must be set on the CLASS: the base __init__ binds immediately, so
    # assigning it on the instance is too late and a restart hits
    # "Address already in use" while the old socket is in TIME_WAIT.
    allow_reuse_address = True
    daemon_threads = True


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8181
    with Server(("", port), H) as srv:
        print("serving DOS games on port %d" % port)
        srv.serve_forever()
