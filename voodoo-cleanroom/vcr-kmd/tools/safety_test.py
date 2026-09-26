#!/usr/bin/env python3
"""safety_test.py - prove the vcr-kmd safety net before trusting a box to it.

  disable    Diag\\Disable=1 -> after a reboot XP runs on its VGA driver, the
             agent answers, Diag\\LastDecline says "disabled"; then undo it.
  bootloop   BootAttempts=MaxBootAttempts -> the next boot DECLINES the card
             (the boot-loop breaker), LastDecline says "boot loop"; then undo.
  crashdump  force a bugcheck (right-Ctrl + ScrollLock x2, CrashOnCtrlScroll)
             after writing a marker into the flight recorder; after the
             automatic reboot, fetch MEMORY.DMP and require vcrdump.py to find
             the bugcheck AND the marker.

The VM test bed (tools/qemu/run-vcrkmd-vm.sh) is the default target: reboots
go through the agent, keys through the QEMU monitor. A fleet box needs
--reboot-cmd with scripts/fleet/safe-reboot.py, and crashdump needs a person
at a PS/2 keyboard - it is VM-only here.

    safety_test.py [--tests disable,bootloop,crashdump]
"""
import argparse
import asyncio
import json
import socket
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2]))
sys.path.insert(0, str(HERE))
from client.retro_protocol import RetroConnection  # noqa: E402
import vcrdump  # noqa: E402

DIAG = r"SYSTEM\CurrentControlSet\Services\vcrmp\Diag"


class Box:
    def __init__(self, a):
        self.a = a

    async def cmd(self, s, timeout=60):
        c = RetroConnection(self.a.host, self.a.port)
        await c.connect("retro-agent-secret", timeout=20)
        try:
            st, d = await c.send_command(s, timeout=timeout)
            return d
        finally:
            await c.close()

    async def text(self, s, timeout=60):
        return (await self.cmd(s, timeout)).decode("ascii", "replace")

    async def vcr(self, args):
        t = await self.text(rf"EXEC {self.a.tool} {args}")
        for ln in reversed(t.strip().splitlines()):
            if ln.startswith("{"):
                return json.loads(ln)
        return {}

    async def diag(self):
        j = json.loads(await self.text(f"REGREAD HKLM {DIAG}"))
        return {v["name"]: v.get("data") for v in j.get("values", [])}

    async def regwrite(self, path, name, value):
        r = await self.text(f"REGWRITE HKLM {path} {name} REG_DWORD {value}")
        assert r.strip().startswith("OK"), r

    async def alive(self):
        try:
            return (await self.cmd("PING", timeout=15)).startswith(b"PONG")
        except Exception:
            return False

    async def reboot_and_wait(self, down_first=True):
        if self.a.reboot_cmd:
            subprocess.run(self.a.reboot_cmd, shell=True, check=True)
        else:
            try:
                await self.cmd("REBOOT", timeout=15)
            except Exception:
                pass
        await self.wait_back(down_first)

    async def wait_back(self, down_first=True, limit=600):
        t0 = time.time()
        if down_first:
            while time.time() - t0 < 120 and await self.alive():
                await asyncio.sleep(3)
        while time.time() - t0 < limit:
            if await self.alive():
                await asyncio.sleep(10)          # let explorer and the desktop settle
                return time.time() - t0
            await asyncio.sleep(5)
        raise RuntimeError("box did not come back")


def monitor(sock, line):
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(sock)
    s.settimeout(3)
    try:
        s.recv(4096)
    except socket.timeout:
        pass
    s.sendall(line.encode() + b"\n")
    time.sleep(0.3)
    s.close()


async def t_disable(b):
    await b.regwrite(DIAG, "Disable", 1)
    await b.reboot_and_wait()
    info = await b.vcr("info")
    d = await b.diag()
    ok = (not info.get("ok")) and (d.get("LastDecline", 0) >> 16) == 1
    print(f"  disabled boot: our driver active={info.get('ok')}, LastDecline="
          f"{d.get('LastDecline', 0):#x} -> {'ok' if ok else 'FAIL'}")
    await b.regwrite(DIAG, "Disable", 0)
    await b.reboot_and_wait()
    back = await b.vcr("info")
    print(f"  re-enabled: our driver active={back.get('ok')}")
    return ok and bool(back.get("ok"))


async def t_bootloop(b):
    d = await b.diag()
    mx = d.get("MaxBootAttempts", 3)
    declined0 = d.get("DeclinedBoots", 0)
    await b.regwrite(DIAG, "BootAttempts", mx)
    await b.reboot_and_wait()
    info = await b.vcr("info")
    d = await b.diag()
    ok = (not info.get("ok")) and (d.get("LastDecline", 0) >> 16) == 2 and \
        d.get("DeclinedBoots", 0) == declined0 + 1
    print(f"  after {mx} unstable boots: our driver active={info.get('ok')}, "
          f"LastDecline={d.get('LastDecline', 0):#x}, DeclinedBoots={d.get('DeclinedBoots')} "
          f"-> {'ok' if ok else 'FAIL'}")
    await b.regwrite(DIAG, "BootAttempts", 0)
    await b.reboot_and_wait()
    back = await b.vcr("info")
    print(f"  counter reset: our driver active={back.get('ok')}")
    return ok and bool(back.get("ok"))


async def t_crashdump(b):
    if not b.a.monitor:
        print("  crashdump needs --monitor (VM only)")
        return False
    await b.regwrite(r"SYSTEM\CurrentControlSet\Services\i8042prt\Parameters",
                     "CrashOnCtrlScroll", 1)
    await b.reboot_and_wait()
    marker = f"safety-test-{int(time.time())}"
    await b.vcr(f"mark {marker}")
    print(f"  marker {marker} written; forcing bugcheck 0xE2")
    # ONE chord: i8042prt counts ScrollLock presses while right-Ctrl is held,
    # and two separate sendkey chords release Ctrl in between (measured: no
    # crash). The repeated key is a second make code under the same hold.
    monitor(b.a.monitor, "sendkey ctrl_r-scroll_lock-scroll_lock 500")
    took = await b.wait_back(down_first=True, limit=900)
    print(f"  back after {took:.0f}s")
    listing = await b.text(r"DIRLIST C:\WINDOWS")
    size = next((e["size"] for e in json.loads(listing) if e["name"].upper() == "MEMORY.DMP"), 0)
    print(f"  MEMORY.DMP {size / 1e6:.1f} MB")
    if not size:
        return False
    data = await b.cmd(r"DOWNLOAD C:\WINDOWS\MEMORY.DMP", timeout=900)
    out = Path(b.a.workdir) / "MEMORY.DMP"
    out.write_bytes(data)
    h = vcrdump.dump_header(data)
    rings = vcrdump.find_rings(data)
    found = False
    if rings:
        best = max(rings, key=lambda r: r["next_seq"])
        found = any(marker in e["msg"] for e in vcrdump.read_entries(data, best))
    ok = bool(h) and h["bugcheck"] == 0xE2 and found
    print(f"  dump: bugcheck {h['bugcheck']:#x} type {h['dump_type']}, rings {len(rings)}, "
          f"marker {'FOUND' if found else 'missing'} -> {'ok' if ok else 'FAIL'}")
    return ok


async def main_async(a):
    b = Box(a)
    Path(a.workdir).mkdir(parents=True, exist_ok=True)
    results = {}
    for t in a.tests.split(","):
        print(f"[{t}]")
        results[t] = await {"disable": t_disable, "bootloop": t_bootloop,
                            "crashdump": t_crashdump}[t](b)
    print("summary:", results)
    return 0 if all(results.values()) else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=19910)
    ap.add_argument("--tool", default=r"C:\vcr\vcrctl.exe")
    ap.add_argument("--tests", default="disable,bootloop,crashdump")
    ap.add_argument("--monitor", default=str(Path.home() / "retro-vm/vcrkmd/mon.sock"))
    ap.add_argument("--reboot-cmd", help="e.g. 'python3 scripts/fleet/safe-reboot.py <ip>'")
    ap.add_argument("--workdir", default=str(Path.home() / "retro-vm/vcrkmd/safety"))
    a = ap.parse_args()
    sys.exit(asyncio.run(main_async(a)))


if __name__ == "__main__":
    main()
