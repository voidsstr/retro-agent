#!/usr/bin/env python3
"""deploy_box.py - put vcr-kmd on a real box, or take it off again, safely.

  deploy_box.py install  <ip> [--hwid ...]   preflight, install, reboot, verify
  deploy_box.py rollback <ip> [--hwid ...]   back to the vendor driver package
  deploy_box.py status   <ip>                what is on it, what the driver said

INSTALL
  preflight  agent answers; activation is not pending (a reboot into an
             activation lockout needs a keyboard); kernel dumps are on (the
             flight recorder lives in non-paged pool, a minidump has none);
             a ROLLBACK PACKAGE of the current vendor driver exists on the box
             (its INF plus every file it copies - the INF alone is not enough,
             setupapi needs the sources); the boot counter is armed low.
  install    upload the package, DRVUPDATE (forced UpdateDriverForPlugAndPlay-
             Devices), then read the service key back - an OK is not proof.
  reboot     scripts/fleet/safe-reboot.py (PXE hold armed first).
  verify     wait for the agent; report Diag (LastPhase, PhaseLog,
             LastDecline, BootAttempts), `vcrctl info`, and save `vcrctl log`
             + `vcrctl snapshot` + a screenshot under the evidence directory.

ROLLBACK
  DRVUPDATE the vendor INF from the rollback package, then restore the
  OpenGLDrivers\\3dfx DLL value the vendor INF overwrites (its AddReg points
  it back at 3dfxOGL.dll), then safe-reboot.

Nothing here reboots a box that failed preflight.
"""
import argparse
import asyncio
import json
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
KMD = HERE.parent
REPO = KMD.parents[1]
sys.path.insert(0, str(REPO))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = "retro-agent-secret"
DIAG = r"SYSTEM\CurrentControlSet\Services\vcrmp\Diag"
OGL = r"SOFTWARE\Microsoft\Windows NT\CurrentVersion\OpenGLDrivers\3dfx"
V56K_HWID = r"PCI\VEN_121A&DEV_0009&SUBSYS_0001121A"


class Agent:
    def __init__(self, ip, port=9898):
        self.ip = ip
        self.port = port

    async def raw(self, cmd, timeout=60, payload=None):
        c = RetroConnection(self.ip, self.port)
        await c.connect(SECRET, timeout=20)
        try:
            if payload is not None:
                st, d = await c.send_command(cmd, binary_payload=payload, timeout=timeout)
            else:
                st, d = await c.send_command(cmd, timeout=timeout)
            return d
        finally:
            await c.close()

    async def text(self, cmd, timeout=60):
        return (await self.raw(cmd, timeout)).decode("ascii", "replace")

    async def regvals(self, path):
        try:
            j = json.loads(await self.text(f'REGREAD HKLM "{path}"' if " " in path
                                           else f"REGREAD HKLM {path}"))
        except (ValueError, json.JSONDecodeError):
            return None
        return {v["name"]: v.get("data") for v in j.get("values", [])}

    async def alive(self):
        try:
            return (await self.raw("PING", timeout=15)).startswith(b"PONG")
        except Exception:
            return False

    async def vcrctl(self, args, tool, timeout=60):
        t = await self.text(f"EXEC {tool} {args}", timeout)
        for ln in reversed(t.strip().splitlines()):
            if ln.startswith("{"):
                try:
                    return json.loads(ln), t
                except json.JSONDecodeError:
                    break
        return {}, t


async def drvupdate(a, hwid, inf):
    """DRVUPDATE, clicking through XP's unsigned-driver dialog.

    The "has not passed Windows Logo testing" dialog blocks the install until
    someone clicks Continue Anyway (default button: STOP). The registry policy
    value does not help on a box whose policy was not set through the proper
    path - XP guards it with a hash (measured on .124). So a second connection
    watches for the dialog and clicks the button, at its fixed offset in the
    dialog, while the install waits."""
    task = asyncio.ensure_future(a.text(rf"DRVUPDATE {hwid} {inf}", timeout=300))
    clicks = 0
    while not task.done():
        await asyncio.sleep(4)
        try:
            wins = json.loads(await a.text("WINLIST", timeout=20)).get("windows", [])
        except Exception:
            continue
        for w in wins:
            if w.get("title") == "Hardware Installation" and w.get("class") == "#32770":
                x, y = w["rect"]["left"] + 219, w["rect"]["top"] + 288
                await a.text(f"UICLICK {x} {y}")
                clicks += 1
                print(f"  clicked Continue Anyway at {x},{y}")
    r = await task
    return r, clicks


REBOOT_CMD = None      # --reboot-cmd: the VM test bed reboots without PXE


def safe_reboot(ip):
    if REBOOT_CMD:
        r = subprocess.run(REBOOT_CMD, shell=True, capture_output=True, text=True, timeout=240)
        print("  reboot-cmd:", (r.stdout + r.stderr).strip()[-200:], "rc", r.returncode)
        return r.returncode == 0
    r = subprocess.run([sys.executable, str(REPO / "scripts" / "fleet" / "safe-reboot.py"), ip],
                       capture_output=True, text=True, timeout=240)
    print("  safe-reboot:", (r.stdout + r.stderr).strip().replace("\n", " | ")[-300:])
    return r.returncode == 0


async def wait_back(a, limit=900):
    t0 = time.time()
    while time.time() - t0 < 150 and await a.alive():
        await asyncio.sleep(5)
    while time.time() - t0 < limit:
        if await a.alive():
            await asyncio.sleep(20)          # explorer, the agent's own startup work
            return time.time() - t0
        await asyncio.sleep(10)
    return None


async def preflight(a, args):
    ok = True
    if not await a.alive():
        print("  FAIL agent does not answer")
        return False
    lic = json.loads(await a.text("LICSTATUS"))
    act = next((v for v in lic.get("values", []) if v["id"] == "activation_required"), {})
    if act.get("observed") not in ("unknown", "absent", "not_required", None):
        print(f"  FAIL activation: {act}")
        ok = False
    cc = await a.regvals(r"SYSTEM\CurrentControlSet\Control\CrashControl") or {}
    if cc.get("CrashDumpEnabled") != 2:
        print(f"  setting CrashDumpEnabled 2 (was {cc.get('CrashDumpEnabled')})")
        await a.text(r"REGWRITE HKLM SYSTEM\CurrentControlSet\Control\CrashControl "
                     "CrashDumpEnabled REG_DWORD 2")
    # An unsigned driver otherwise stops at XP's "has not passed Windows Logo
    # testing" dialog and DRVUPDATE waits for a click (seen on .124; the VM's
    # image already had the policy set). HKCU is the user the agent runs as.
    # (REGWRITE cannot write REG_BINARY - hence a .reg file.)
    reg = (b"Windows Registry Editor Version 5.00\r\n\r\n"
           b"[HKEY_CURRENT_USER\\Software\\Microsoft\\Driver Signing]\r\n"
           b"\"Policy\"=dword:00000000\r\n\r\n"
           b"[HKEY_LOCAL_MACHINE\\SOFTWARE\\Microsoft\\Driver Signing]\r\n"
           b"\"Policy\"=hex:00\r\n")
    await a.text(rf"MKDIR {args.dir}")
    await a.raw(rf"UPLOAD {args.dir}\signpolicy.reg", payload=reg)
    await a.text(rf"EXEC regedit /s {args.dir}\signpolicy.reg")
    pol = await a.regvals(r"SOFTWARE\Microsoft\Driver Signing")
    print(f"  driver signing policy (HKLM): {pol}")
    if args.rollback_dir:
        try:
            files = {e["name"].lower() for e in json.loads(await a.text(f"DIRLIST {args.rollback_dir}"))}
        except (ValueError, json.JSONDecodeError):
            files = set()
        need = {n.lower() for n in args.rollback_files.split(",")}
        if not need <= files:
            print(f"  FAIL rollback package incomplete in {args.rollback_dir}: missing {sorted(need - files)}")
            ok = False
        else:
            print(f"  rollback package ok: {args.rollback_dir} ({len(need)} files)")
    return ok


async def status(a, args, evidence):
    d = await a.regvals(DIAG) or {}
    for k in ("BootCount", "BootAttempts", "GoodBoots", "DeclinedBoots", "LastDecline",
              "LastPhase", "LastPhaseA", "LastPhaseMs", "PhaseCount"):
        if k in d:
            print(f"  Diag {k} = {d[k]:#x}" if k == "LastDecline" else f"  Diag {k} = {d[k]}")
    info, _ = await a.vcrctl("info", args.tool)
    print("  vcrctl info:", json.dumps(info)[:600])
    evidence.mkdir(parents=True, exist_ok=True)
    _, logtxt = await a.vcrctl("log", args.tool, timeout=90)
    (evidence / "vcrlog.tsv").write_text(logtxt)
    snap, snaptxt = await a.vcrctl("snapshot", args.tool)
    (evidence / "snapshot.json").write_text(snaptxt)
    shot = await a.raw("SCREENSHOT 0", timeout=90)
    (evidence / "screen.bmp").write_bytes(shot)
    try:
        from PIL import Image
        Image.open(evidence / "screen.bmp").save(evidence / "screen.png", optimize=True)
        (evidence / "screen.bmp").unlink()          # the PNG is the evidence
    except Exception:
        pass
    print(f"  evidence -> {evidence}")
    return bool(info.get("ok"))


async def install(a, args, evidence):
    print("[preflight]")
    if not await preflight(a, args):
        return 2
    print("[install]")
    await a.text(rf"MKDIR {args.dir}")
    for f in ("out/vcrmp.sys", "out/vcrdd.dll", "out/vcrctl.exe", "inf/vcrkmd.inf"):
        data = (KMD / f).read_bytes()
        name = Path(f).name
        await a.raw(rf"UPLOAD {args.dir}\{name}", timeout=60, payload=data)
    listing = json.loads(await a.text(f"DIRLIST {args.dir}"))
    sizes = {e["name"].lower(): e["size"] for e in listing}
    for f in ("out/vcrmp.sys", "out/vcrdd.dll", "inf/vcrkmd.inf"):
        n = Path(f).name.lower()
        if sizes.get(n) != (KMD / f).stat().st_size:
            print(f"  FAIL {n} did not land ({sizes.get(n)})")
            return 2
    r, _ = await drvupdate(a, args.hwid, rf"{args.dir}\vcrkmd.inf")
    print("  DRVUPDATE:", r.strip()[:200])
    svc = await a.regvals(r"SYSTEM\CurrentControlSet\Services\vcrmp")
    if not svc or "ImagePath" not in svc:
        print("  FAIL no vcrmp service key after DRVUPDATE")
        return 2
    await a.text(rf"REGWRITE HKLM {DIAG} MaxBootAttempts REG_DWORD {args.max_boot_attempts}")
    await a.text(rf"REGWRITE HKLM {DIAG} LogLevel REG_DWORD 3")
    await a.text(rf"REGWRITE HKLM {DIAG} BootAttempts REG_DWORD 0")
    print("[reboot]")
    if not safe_reboot(a.ip):
        print("  FAIL safe-reboot refused - NOT rebooted; the new driver loads at the next boot")
        return 2
    took = await wait_back(a)
    if took is None:
        print("  FAIL the box did not come back within 15 min - Diag phases are on its disk")
        return 3
    print(f"[verify] back after {took:.0f}s")
    return 0 if await status(a, args, evidence) else 1


async def rollback(a, args, evidence):
    inf = rf"{args.rollback_dir}\{args.rollback_inf}"
    r, _ = await drvupdate(a, args.hwid, inf)
    print("  DRVUPDATE:", r.strip()[:200])
    if args.ogl_dll:
        # QUOTED: the path holds "Windows NT", and an unquoted REGWRITE splits
        # it and writes nowhere useful (seen: the value stayed 3dfxOGL.dll)
        await a.text(rf'REGWRITE HKLM "{OGL}" DLL REG_SZ {args.ogl_dll}')
        got = (await a.regvals(OGL) or {}).get("DLL")
        print(f"  OpenGLDrivers\\3dfx DLL = {got}" + ("" if got == args.ogl_dll else "  <-- NOT restored"))
    if not safe_reboot(a.ip):
        return 2
    took = await wait_back(a)
    print(f"  back after {took}s" if took else "  FAIL box did not come back")
    return 0 if took else 3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("action", choices=("install", "rollback", "status"))
    ap.add_argument("ip")
    ap.add_argument("--hwid", default=V56K_HWID)
    ap.add_argument("--dir", default=r"C:\vcr")
    ap.add_argument("--tool", default=r"C:\vcr\vcrctl.exe")
    ap.add_argument("--rollback-dir", default=r"C:\vcr\am31")
    ap.add_argument("--rollback-inf", default="am31.inf")
    ap.add_argument("--rollback-files",
                    default="am31.inf,3dfxvsm.sys,3dfxvs.dll,glide2x.dll,glide3x.dll,"
                            "3dfxOGL.dll,3dfxSpl2.dll,3dfxSpl3.dll,dxtn.dll")
    ap.add_argument("--ogl-dll", default="retroicd.dll",
                    help="OpenGLDrivers\\3dfx DLL to restore after a rollback")
    ap.add_argument("--max-boot-attempts", type=int, default=2)
    ap.add_argument("--evidence", default=str(KMD / "evidence"))
    ap.add_argument("--port", type=int, default=9898, help="agent port (19910: the VM test bed)")
    ap.add_argument("--reboot-cmd", help="instead of safe-reboot.py - ONLY for the VM test bed "
                    "(a fleet box PXE-boots first and needs the hold safe-reboot arms)")
    args = ap.parse_args()
    global REBOOT_CMD
    REBOOT_CMD = args.reboot_cmd
    a = Agent(args.ip, args.port)
    evidence = Path(args.evidence) / f"{args.ip}_{time.strftime('%Y%m%d-%H%M%S')}_{args.action}"
    fn = {"install": install, "rollback": rollback, "status": status}[args.action]
    rc = asyncio.run(fn(a, args, evidence))
    sys.exit(rc if isinstance(rc, int) else (0 if rc else 1))


if __name__ == "__main__":
    main()
