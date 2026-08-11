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

SHARE = "/run/user/1000/gvfs/smb-share:server=192.168.1.122,share=files/Games/DOS"
HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "data")          # GAMES.CAT + tiles/ live here

SEARCH_DIRS = [SHARE,
               os.path.join(SHARE, "DOS Games Collection", "1-C"),
               os.path.join(SHARE, "DOS Games Collection", "D-L"),
               os.path.join(SHARE, "DOS Games Collection", "M-R"),
               os.path.join(SHARE, "DOS Games Collection", "S-Z"),
               os.path.join(SHARE, "More Dos Games")]

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
        if p in ("/GAMES.CAT", "/games.cat"):
            self._send_file(os.path.join(DATA, "GAMES.CAT")); return
        if p.lower().startswith("/tiles/"):
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
