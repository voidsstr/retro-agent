#!/usr/bin/env python3
"""Stage B on .243 once its second disk (D:) is online in Windows: prove D:,
point DOSGAME at the host, expand the pre-packed titles, audit them.

Never touches the IDE ports (ide9x/idewrite9x refuse a channel Windows owns;
running them on a live channel is what took the agent down on 2026-09-27).
    python3 scripts/dosgames/fill_243.py [--host 192.168.1.243] [--bridge 192.168.1.196]
"""
import argparse
import asyncio
import json
import os
import sys
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, REPO)
from client.retro_protocol import RetroConnection  # noqa: E402

MANIFEST = os.path.join(REPO, "scripts", "dosgames", "data", "fill-243.MANIFEST.TXT")
SHARE_FILL = "W:\\FILES\\GAMES\\DOSFILL"


class Stop(Exception):
    pass


async def cmd(c, s, **kw):
    st, d = await c.send_command(s, **kw)
    return st, d


async def main(a):
    rows = [l.split("|") for l in open(MANIFEST).read().splitlines() if l.strip()]
    c = RetroConnection(a.host, 9898)
    await c.connect("retro-agent-secret", timeout=60.0)
    try:
        st, d = await cmd(c, "SYSINFO")
        drives = {x.get("root"): x.get("type") for x in json.loads(d).get("drives", [])}
        print("drives:", drives)
        if drives.get("D:\\") != "fixed":
            raise Stop("no fixed D: - the disk is not online in Windows")
        blob = os.urandom(1 << 20)
        await cmd(c, "MKDIR D:\\GAMES")
        await cmd(c, "UPLOAD D:\\GAMES\\RWTEST.BIN", binary_payload=blob)
        st, back = await cmd(c, "DOWNLOAD D:\\GAMES\\RWTEST.BIN")
        if back != blob:
            raise Stop("D: write/read test FAILED")
        await cmd(c, "DELETE D:\\GAMES\\RWTEST.BIN")
        print("D: 1 MB write/read OK")

        st, cfg = await cmd(c, "DOWNLOAD C:\\DOSGAME\\DOSGAME.CFG")
        text = cfg.decode("latin-1") if st == 1 else ""
        lines = [l for l in text.replace("\r", "").split("\n") if l.strip()]
        new = []
        for l in lines:
            k = l.split("=", 1)[0].strip().lower()
            if k == "url":
                l = "url=http://%s:8181" % a.bridge
            elif k == "scan":
                l = "scan=D:\\GAMES;C:\\GAMES;C:\\"
            new.append(l)
        if not any(l.lower().startswith("scan=") for l in new):
            new.append("scan=D:\\GAMES;C:\\GAMES;C:\\")
        body = ("\r\n".join(new) + "\r\n").encode("latin-1")
        await cmd(c, "UPLOAD C:\\DOSGAME\\DOSGAME.CFG", binary_payload=body)
        st, back = await cmd(c, "DOWNLOAD C:\\DOSGAME\\DOSGAME.CFG")
        if back != body:
            raise Stop("DOSGAME.CFG did not land")
        print("DOSGAME.CFG:", new)

        st, d = await cmd(c, "DIRLIST " + SHARE_FILL)
        if st != 0:          # text commands answer status 0 on success (DOWNLOAD answers 1)
            raise Stop("the share staging dir is not reachable as " + SHARE_FILL)
        await cmd(c, "DELETE D:\\GAMES\\FILL.LOG")
        st, d = await cmd(c, "LAUNCH %s\\FILL.BAT" % SHARE_FILL)   # LAUNCH already wraps command.com /c on 9x
        print("FILL.BAT launched:", d[:120])
        t0 = time.time()
        while time.time() - t0 < 4 * 3600:
            await asyncio.sleep(30)
            st, log = await cmd(c, "DOWNLOAD D:\\GAMES\\FILL.LOG")
            t = log.decode("latin-1") if st == 1 else ""
            print("  %4d s: %d OK, %d FAIL" % (time.time() - t0, t.count("OK "), t.count("FAIL ")), flush=True)
            if "END" in t:
                break
        missing = []
        for r in rows:
            st, d = await cmd(c, "DOWNLOAD D:\\GAMES\\%s\\FILL.OK" % r[0])
            if st != 1:
                missing.append(r[0] + " " + r[1])
        print("FILL.OK present for %d/%d titles" % (len(rows) - len(missing), len(rows)))
        for m in missing:
            print("  MISSING", m)
    except Stop as e:
        print("STOPPED:", e)
    finally:
        await c.close()


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="192.168.1.243")
    ap.add_argument("--bridge", default="192.168.1.196")
    asyncio.run(main(ap.parse_args()))
