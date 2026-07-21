#!/usr/bin/env python3
"""Automate mounting an ISO on a fleet XP box via DaemonTools 3.47 (classic).

  python3 scripts/mount_iso.py <ip> "<iso-path-on-box>" [--dtdir "D:\\Program Files\\D-Tools"]
  python3 scripts/mount_iso.py <ip> --unmount

Hard-won specifics on .124 (see retro-3dfx/FINDINGS.md):
- Two D-Tools installs (C: and D:); only the one on the ACTIVE Windows volume (D:)
  is registered — the C: daemon.exe throws "Product not installed!".
- daemon.exe stays resident (tray): launch DETACHED (`start ""`) from its own dir;
  a tree-kill (EXECW) undoes the mount.
- The d347bus virtual-SCSI driver is already installed+running (creates the virtual
  CD drives). daemon.exe -mount 0,<img> mounts to the first virtual drive.
- Path must be SPACE-FREE (daemon.exe 3.47 can't parse spaces) — stage the ISO to a
  no-space path, or use an 8.3 short path.
"""
import asyncio, sys, os, re
sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
from client.retro_protocol import RetroConnection

SECRET = os.environ.get("RETRO_AGENT_SECRET", "retro-agent-secret")


async def ex(c, cmd, t=30):
    try:
        return (await c.command_text("EXEC cmd /c " + cmd, timeout=t))
    except Exception as e:
        return "__ERR__ %s" % e


async def mount(ip, image, dtdir):
    c = RetroConnection(ip, 9898); await c.connect(SECRET, timeout=15)
    short = dtdir.replace(r"D:\Program Files", r"D:\PROGRA~1").replace(r"C:\Program Files", r"C:\PROGRA~1")
    if " " in image:
        print("WARNING: ISO path has spaces; DaemonTools 3.47 needs a space-free path "
              "(stage/copy it to e.g. D:\\ISO\\name.iso).")
    # start tray (idempotent) + mount, both detached from the DT dir
    await ex(c, r'start "" /d "%s" %s\daemon.exe' % (dtdir, short), 12)
    await asyncio.sleep(5)
    await ex(c, r'start "" /d "%s" %s\daemon.exe -mount 0,%s' % (dtdir, short, image), 12)
    await asyncio.sleep(8)
    dl = await ex(c, r'wmic logicaldisk where drivetype=5 get deviceid,volumename', 20)
    drv = None
    for line in dl.splitlines():
        m = re.match(r'\s*([E-Z]):\s+(\S.*\S)\s*$', line)
        if m and m.group(2).lower() != "volumename":
            drv = m.group(1); print("MOUNTED: %s: = %s" % (drv, m.group(2)))
    if not drv:
        print("no volume mounted (check the ISO path is space-free + exists on the box)")
    await c.close()
    return drv


async def unmount(ip, dtdir):
    c = RetroConnection(ip, 9898); await c.connect(SECRET, timeout=15)
    short = dtdir.replace(r"D:\Program Files", r"D:\PROGRA~1")
    await ex(c, r'start "" /d "%s" %s\daemon.exe -unmount 0' % (dtdir, short), 12)
    print("unmount issued")
    await c.close()


if __name__ == "__main__":
    if len(sys.argv) < 3:
        print(__doc__); sys.exit(1)
    ip = sys.argv[1]
    dtdir = r"D:\Program Files\D-Tools"
    if "--dtdir" in sys.argv:
        dtdir = sys.argv[sys.argv.index("--dtdir") + 1]
    if "--unmount" in sys.argv:
        asyncio.run(unmount(ip, dtdir))
    else:
        asyncio.run(mount(ip, sys.argv[2], dtdir))
