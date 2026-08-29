#!/usr/bin/env python3
"""Install the 3dfx Voodoo 2 driver on a fleet Windows XP/2000 box.

A Voodoo 2 is a 3D-only passthrough card: its INF is Class=MEDIA, so it never
appears in VIDEODIAG or any display-class enumeration. Detection is done
against the raw PCI enum key instead.

The Win2K 1.02.00 kit is the driver that works on XP (most Win9x Voodoo2
drivers do not).  Its INF registers fxgpio/fxptl/Ntremap at StartType=2
(auto); the Win2K display driver is core-level and fails SILENTLY on XP unless
all three are moved to StartType=1 (system).  That fix-up is the whole reason
this script exists -- see fleetbook recipe
`voodoo-2-and-voodoo-2-sli-on-a-fleet-windows-xp-box-driver-d`.

Usage:
    python3 scripts/voodoo2/install_voodoo2.py <ip> [--driver DIR] [--dry-run]
    python3 scripts/voodoo2/install_voodoo2.py <ip> --detect-only
"""
import argparse
import asyncio
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))
from client.retro_protocol import RetroConnection  # noqa: E402

SECRET = "retro-agent-secret"
V2_HWID = r"PCI\VEN_121A&DEV_0002"
SERVICES = ("fxgpio", "fxptl", "Ntremap")
STAGE = r"C:\V2DRV"
SHARE_DIR = r"Z:\Files\Drivers\3DFX\WinXP\Voodoo2_1.02.00_Win2K"


async def cmd(conn, text, payload=None):
    status, data = await conn.send_command(text, binary_payload=payload)
    return status, data.decode("ascii", "replace")


async def find_voodoo2(conn):
    """Return the list of Voodoo 2 PCI instance keys present on the box.

    Tries the NT/XP enum path first, then the Win9x one -- they differ.
    """
    for path in (r"SYSTEM\CurrentControlSet\Enum\PCI", r"Enum\PCI"):
        try:
            _, raw = await cmd(conn, f"REGREAD HKLM {path}")
        except Exception:
            continue
        devs = [
            x.strip().strip('",')
            for x in raw.replace(",", "\n").splitlines()
            if "VEN_" in x.upper()
        ]
        if not devs:
            continue
        # VEN_1102&DEV_0002 is a Creative SB Live!, NOT a Voodoo 2. Match the
        # 3dfx vendor id explicitly.
        return [d for d in devs if "VEN_121A" in d.upper() and "DEV_0002" in d.upper()]
    return []


async def service_start_types(conn):
    """Current Start value for each of the three Voodoo 2 kernel services."""
    out = {}
    for svc in SERVICES:
        try:
            _, raw = await cmd(
                conn, rf"REGREAD HKLM SYSTEM\CurrentControlSet\Services\{svc}"
            )
            j = json.loads(raw)
            val = next(
                (v for v in j.get("values", []) if v.get("name", "").lower() == "start"),
                None,
            )
            out[svc] = val.get("data") if val else None
        except Exception:
            out[svc] = None
    return out


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("ip")
    ap.add_argument("--driver", help="local dir holding the extracted 1.02.00 kit")
    ap.add_argument("--from-share", action="store_true",
                    help=f"copy the kit from {SHARE_DIR} instead of uploading")
    ap.add_argument("--detect-only", action="store_true")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    conn = RetroConnection(args.ip, 9898)
    await conn.connect(SECRET, timeout=15.0)

    cards = await find_voodoo2(conn)
    print(f"Voodoo 2 cards found: {len(cards)}")
    for c in cards:
        print("   ", c)
    if not cards:
        print("\nNo VEN_121A&DEV_0002 on this box. Nothing to do.")
        print("(Reminder: a Voodoo 2 is Class=MEDIA and never shows in VIDEODIAG.)")
        await conn.close()
        return 1
    if len(cards) >= 2:
        print(f"\n{len(cards)} cards -> SLI capable. The official 3dfx driver does")
        print("NOT support mismatched SLI (different vendor or RAM size).")

    starts = await service_start_types(conn)
    print(f"\nservice start types (want 1=system): {starts}")

    if args.detect_only:
        await conn.close()
        return 0
    if args.dry_run:
        print("\n[dry-run] would install, then set the three services to Start=1")
        await conn.close()
        return 0

    # Unsigned-driver dialog blocks EXEC forever on an invisible prompt.
    await cmd(conn, 'EXEC reg add "HKLM\\Software\\Microsoft\\Driver Signing" '
                    "/v Policy /t REG_BINARY /d 00 /f")

    await cmd(conn, f"MKDIR {STAGE}")
    if args.from_share:
        _, out = await cmd(conn, f'EXEC cmd /c copy /Y "{SHARE_DIR}\\*.*" {STAGE}\\')
        print(f"\ncopied from share:\n{out[:300]}")
    else:
        if not args.driver:
            print("need --driver DIR or --from-share")
            await conn.close()
            return 2
        for fn in sorted(os.listdir(args.driver)):
            blob = open(os.path.join(args.driver, fn), "rb").read()
            await cmd(conn, f"UPLOAD {STAGE}\\{fn}", payload=blob)
            print(f"  uploaded {fn} ({len(blob)})")

    # drvupd.exe: headless INF bind (XP has no devcon and no CLI PnP rescan).
    _, out = await cmd(
        conn, f'EXECW 420 {STAGE}\\drvupd.exe "{STAGE}\\Voodoo2.inf" "{V2_HWID}"'
    )
    print(f"\ndrvupd: {out.strip()[:400]}")

    # THE FIX: Win2K core-level driver needs system start on XP, not auto.
    # REGWRITE is <root> <path> <name> <type> <data> -- five tokens. Folding the
    # value name into the path makes the agent CREATE A SUBKEY of that name and
    # still answer OK, leaving Start untouched (agent/src/registry.c:284).
    for svc in SERVICES:
        await cmd(
            conn,
            rf"REGWRITE HKLM SYSTEM\CurrentControlSet\Services\{svc} Start REG_DWORD 1",
        )
    print(f"set {', '.join(SERVICES)} to Start=1 (system)")

    # Never trust the OK -- a misparsed REGWRITE reports success either way.
    final = await service_start_types(conn)
    print(f"\npost-install start types: {final}")
    bad = [s for s, v in final.items() if str(v) != "1"]
    if bad:
        print(f"WARNING: {bad} did not reach Start=1 -- the driver will load but "
              f"render nothing. Re-check the REGWRITE argument order.")
    print("\nA REBOOT is required for the start-type change to take effect.")
    print("Reboot needs explicit user approval -- not issued by this script.")
    await conn.close()
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
