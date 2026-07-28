#!/usr/bin/env python3
"""serve_dosgames.py — tiny HTTP server bridging the SMB share to DOS boxes.

mTCP HTGET on the DOS side can only speak HTTP, and the share is SMB-only, so
this serves the share's DOS games (via the gvfs mount) plus the generated
catalog and preview tiles:

    /GAMES.CAT          the catalog (regenerate with gen_catalog.py)
    /dos/<zipname>      archives from the share DOS dir (flat names only)
    /tiles/<name>.PRV   preview tiles

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
        if p.lower().startswith("/dos/"):
            name = os.path.basename(p)
            for d in SEARCH_DIRS:
                cand = os.path.join(d, name)
                if os.path.isfile(cand):
                    self._send_file(cand); return
            self.send_error(404); return
        self.send_error(404)

    def log_message(self, fmt, *a):
        sys.stderr.write("%s %s\n" % (self.address_string(), fmt % a))

if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8181
    with socketserver.ThreadingTCPServer(("", port), H) as srv:
        srv.allow_reuse_address = True
        print("serving DOS games on port %d" % port)
        srv.serve_forever()
