# Voodoo 2 (and Voodoo 2 SLI) on a fleet box

A Voodoo 2 is a **3D-only passthrough card**. Its INF is `Class=MEDIA`, so it
**never appears in `VIDEODIAG`** or in any display-class enumeration — the box's
2D card stays the display adapter. Detect it against the raw PCI enum instead:

```
REGREAD HKLM SYSTEM\CurrentControlSet\Enum\PCI     # NT / 2000 / XP
REGREAD HKLM Enum\PCI                              # Win9x — different path!
```

> **`VEN_1102&DEV_0002` is a Creative Sound Blaster Live!, not a Voodoo 2.**
> 3dfx is vendor `121A`; Creative is `1102`. Matching on `DEV_0002` alone
> reports a Voodoo 2 on any box with an SB Live (e.g. `.240`, which has two).
> Locked in by `tests/python/test_voodoo2_install.py`.

## Which driver

| OS | Use | Where |
|---|---|---|
| **XP / 2000** | 3dfx Win2K kit **1.02.00** | `Z:\Files\Drivers\3DFX\WinXP\Voodoo2_1.02.00_Win2K\` |
| Win9x | 3dfx **3.02.02** reference, or FastVoodoo2 4.6 | `…\3DFX\Win9x\voodoo2-30202.zip`, `fastv2-win9x-v46.zip` |

Most **Win9x Voodoo 2 drivers do not work on XP at all** — only the Win2K kit
does. (Of the 9x drivers, only 3.02.02 and 3.03.00 run without downclocking,
and those are for 9x.)

## The trap: service start types

`Voodoo2.inf` registers its three kernel services with `StartType = 2`
(`SERVICE_AUTO_START`):

```
AddService = fxgpio,  0x00000002, fxgpio_Service_Inst
AddService = fxptl,   0x00000002, fxptl_Service_Inst
AddService = Ntremap, 0x00000002, Ntremap_Service_Inst
```

The Win2K display driver is **core-level** and **fails silently on XP** at auto
start. All three must be moved to `Start = 1` (system) and the box rebooted:

```
HKLM\SYSTEM\CurrentControlSet\Services\{fxgpio,fxptl,Ntremap}\Start = 1
```

Symptom if you skip it: the driver reports as installed, and nothing renders.
`install_voodoo2.py` does this fix-up automatically.

> **`REGWRITE` takes five tokens — `<root> <path> <name> <type> <data>`.**
> The value name is a separate argument from the path and the data comes last:
> `REGWRITE HKLM SYSTEM\CurrentControlSet\Services\fxgpio Start REG_DWORD 1`.
> Writing `...\fxgpio\Start 1 REG_DWORD` instead makes the agent **create a
> subkey** named `Start`, put a value named `1` in it, and answer `OK` with the
> real `Start` untouched. It looks like it worked. Always read the value back.

## SLI

- **The cards must match.** The official 3dfx drivers do **not** support
  mixed/mismatched SLI — different manufacturers and/or different RAM size
  (8 MB vs 12 MB) will not run in SLI. Use FastVoodoo2 (9x only) for a
  mismatched pair, or fit matching cards.
- **Fit one card first**, install, verify it renders, power off, *then* add the
  second card and the SLI ribbon. Installing both at once makes a driver fault
  indistinguishable from a cabling fault.
- **Cabling:** 2D card VGA out → Voodoo 2 #1 passthrough **in**; Voodoo 2 #1
  **out** → monitor. The second card gets no VGA connection, only the ribbon.
- `VoodooControl 1.82b` (`…\3DFX\Utilities\voodoocontrol_v182b\setup.exe`)
  exposes SLI state and per-game tweaks.

## Usage

```bash
# what's actually in the box (safe, read-only)
python3 scripts/voodoo2/install_voodoo2.py <ip> --detect-only

# install from the share copy, then fix the start types
python3 scripts/voodoo2/install_voodoo2.py <ip> --from-share

# or push a local extracted kit
python3 scripts/voodoo2/install_voodoo2.py <ip> --driver ~/v2w2k
```

The install needs `drvupd.exe` staged alongside the kit (XP has no `devcon`
and no CLI way to rescan the PnP tree):

```bash
i686-w64-mingw32-gcc -O2 -o drvupd.exe tools/drvupd.c -lsetupapi -lcfgmgr32
```

The script does **not** reboot — that needs explicit user approval.

Fleetbook: `voodoo-2-and-voodoo-2-sli-on-a-fleet-windows-xp-box-driver-d`.
