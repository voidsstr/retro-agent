#!/usr/bin/env python3
"""prep-xp-disk.py - make an XP install from the QEMU build VM boot on 86Box's
ASUS P3B-F (440BX, one CPU, PIIX4), through the guest's agent.

    prep-xp-disk.py 127.0.0.1 --port 19910      # the QEMU vcr-kmd VM, before copying its disk

What QEMU installed and what the board needs are different in two places, and
each is a boot failure with nothing on the screen to say why:

  HAL/kernel  QEMU `-smp 2` installs the ACPI MULTIPROCESSOR kernel and HAL
              (halmacpi.dll, ntkrnlmp as ntoskrnl.exe). The P3B-F has one CPU
              and no I/O APIC. The uniprocessor ACPI (PIC) pair is extracted
              from the SP3 cab next to the originals - ntosup.exe, halacpi.dll -
              and a boot.ini entry selects them with /kernel= /hal=, FIRST, so
              it is the default. The original entry stays, second.
  IDE         the PIIX4 IDE function (8086:7111) is not in the critical device
              database of an install that only ever saw QEMU's PIIX3
              (8086:7010) - INACCESSIBLE_BOOT_DEVICE. Same driver (intelide).

Idempotent. Everything else (the NIC, the Voodoo3) XP finds by itself on the
first boot on the board; the Voodoo3 binds to XP's own in-box 3dfx driver
(3dfxvs2k.inf), which is the test bed's reference driver.
"""
import argparse
import asyncio
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[4]
sys.path.insert(0, str(REPO))
from client.retro_protocol import RetroConnection  # noqa: E402

CAB = r"C:\WINDOWS\Driver Cache\i386\sp3.cab"
STAGE = r"C:\vcr\up"
ENTRY = ('multi(0)disk(0)rdisk(0)partition(1)\\WINDOWS="XP UP ACPI-PIC (86Box)" '
         '/noexecute=optin /fastdetect /kernel=ntosup.exe /hal=halacpi.dll')
REG = ('REGEDIT4\r\n\r\n[HKEY_LOCAL_MACHINE\\SYSTEM\\CurrentControlSet\\Control\\'
       'CriticalDeviceDatabase\\pci#ven_8086&dev_7111]\r\n"Service"="intelide"\r\n'
       '"ClassGUID"="{4D36E96A-E325-11CE-BFC1-08002BE10318}"\r\n\r\n')


async def main_async(a):
    c = RetroConnection(a.host, a.port)
    await c.connect("retro-agent-secret", timeout=20)

    async def run(cmd, payload=None, timeout=180):
        if payload is None:
            st, d = await c.send_command(cmd, timeout=timeout)
        else:
            st, d = await c.send_command(cmd, binary_payload=payload, timeout=timeout)
        return d.decode("latin1", "replace")

    try:
        await run(rf"MKDIR {STAGE}")
        for f in ("ntoskrnl.exe", "halacpi.dll"):
            print(await run(rf'EXECW 170 cmd /c expand -F:{f} "{CAB}" {STAGE}'))
        print(await run(rf"EXEC cmd /c copy /Y {STAGE}\halacpi.dll C:\WINDOWS\system32\halacpi.dll"))
        print(await run(rf"EXEC cmd /c copy /Y {STAGE}\ntoskrnl.exe C:\WINDOWS\system32\ntosup.exe"))
        await run(rf"UPLOAD {STAGE}\piix4.reg", REG.encode())
        await run(rf"EXEC regedit /s {STAGE}\piix4.reg")
        print(await run(r"REGREAD HKLM SYSTEM\CurrentControlSet\Control\CriticalDeviceDatabase"
                        r"\pci#ven_8086&dev_7111"))
        ini = await run(r"EXEC cmd /c type C:\boot.ini")
        lines = [ln for ln in ini.replace("\r", "").split("\n")]
        if any("/hal=halacpi.dll" in ln for ln in lines):
            print("boot.ini already has the uniprocessor entry")
        else:
            out = []
            for ln in lines:
                out.append(ln)
                if ln.strip().lower() == "[operating systems]":
                    out.append(ENTRY)
            text = "\r\n".join(ln for ln in out if ln is not None).rstrip("\r\n") + "\r\n"
            text = text.replace("timeout=30", "timeout=3")
            await run(r"EXEC cmd /c attrib -s -h -r C:\boot.ini")
            await run(r"UPLOAD C:\boot.ini", text.encode())
            await run(r"EXEC cmd /c attrib +s +h +r C:\boot.ini")
        print(await run(r"EXEC cmd /c type C:\boot.ini"))
    finally:
        await c.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("--port", type=int, default=19910)
    asyncio.run(main_async(ap.parse_args()))


if __name__ == "__main__":
    main()
