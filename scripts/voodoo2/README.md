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

- **What the driver ACTUALLY checks** (read from 3dfx's own source,
  `glide3x/cvg/init/sli.c:87-96`) is three things, and RAM size is not one of
  them:

  | checked | notes |
  |---|---|
  | `numberTmus` | both must have 2 — universal on Voodoo 2 |
  | `fbiBoardID` | the PCB design strap, `(fbiInit5 >> 5) & 0xf` |
  | `fbiVideoStruct` | video/DAC configuration |

  The `fbiMemSize` and `tmuMemSize` comparisons are **commented out**, in 3dfx's
  own 1999 initial checkin, with the note that the init code normalises to the
  smaller board. So **an 8 MB + 12 MB pair does SLI**, running as 2 × 8 MB. The
  widely-repeated "different RAM blocks SLI" is false.

  "Same brand" is a proxy, not the rule: different vendors usually strap a
  different `fbiBoardID`, which is what actually fails. Two differently-badged
  cards on the same reference design will pair fine.

- **A mismatched board ID can be forced — but only with our driver.**
  `SSTV2_MISMATCHED_SLI` bypasses the `fbiBoardID` check. Verified by `strings`:
  **present** in `voodoo-cleanroom/out/glide3x_cvg.dll` (it comes from the
  koolsmoky/sezero lineage), **absent** from the stock 3dfx 3.03.00
  `glide3x.dll` the fleet installs. `fbiVideoStruct` must still match.
- **Fit one card first**, install, verify it renders, power off, *then* add the
  second card and the SLI ribbon. Installing both at once makes a driver fault
  indistinguishable from a cabling fault.
- **Cabling:** 2D card VGA out → Voodoo 2 #1 passthrough **in**; Voodoo 2 #1
  **out** → monitor. The second card gets no VGA connection, only the ribbon.
- **Identify a board before buying its partner.** Set `SSTV2_INITDEBUG=1` and
  `SSTV2_INITDEBUG_FILE=C:\\dac.txt`, then run *any* Glide program — the stock
  retail `glide3x.dll` has this compiled in. It dumps the board's real identity:

  ```
  sst1DeviceInfo: Board ID: 9
  sst1DeviceInfo: FbiConfig:0x2, TmuConfig:0xca54
  sst1DeviceInfo: FBI Revision:4, TMU Revison:4, Num TMUs:2
  sst1DeviceInfo: FBI Memory:4, TMU[0] Memory:4, TMU[1] Memory:4
  sst1DeviceInfo: Dac Type: ICS ICS5342
  sst1DeviceInfo: SLI Detected:0
  ```

  > **The PCI subsystem ID is useless here.** The Voodoo 2 "Chuck" FBI chip has
  > no subsystem registers at all (Spec r1.16 §6 marks config `0x14-0x3b`
  > Reserved), and no expansion ROM, so **every** Voodoo 2 reports
  > `SUBSYS_00000000&REV_02` — Creative, Diamond, STB, Canopus, reference,
  > whitebox alike. It identifies nothing. The **brand is not determinable in
  > software**; read the PCB silkscreen (3dfx reference boards carry a
  > `111-xxxx-xxx` part number). Known `fbiBoardID` values from 3dfx's source:
  > `0x2` = 4400 8-layer bringup, `0x3` = early 4-layer 4220, `0x10` =
  > `CANOPUS_ID`. Everything else, including 9, is unnamed.

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

## Keeping games ON the card (2026-08-30)

A Voodoo 2 is a **3D-only passthrough card**, and two things silently take a game
off it. Both were live on `.171`'s UnrealGold, which looked like it was crashing
and was really running on the *software rasterizer* at 100% CPU.

**A game-local `glide2x.dll` shadows the real driver.** Game-local DLLs win over
`system32`. Identify by **size**: **226,304** is the real 3dfx Glide (reports
`Glide 2.56.00.0459`); **1,310,720** is the **nGlide wrapper** (reports `Glide
2.60`) which translates to Direct3D and does not touch the card. On `.171` nGlide
does not even work — `grSstOpen failed (2, 3)` — so the game got neither.

**Unreal-engine games fall out of Glide on the first focus change.** Unreal's own
splash grabs the foreground after the viewport has gone fullscreen; the resulting
`WM_KILLFOCUS` → `EndFullscreen` switches to `WindowedRenderDevice`, and the card
cannot render windowed. Point that at GlideDrv too and Unreal *re-opens* Glide
instead of falling back to `SoftDrv`.

Mode limits worth remembering: the card is **16bpp only**, and a single 4 MB-FBI
card cannot exceed **640x480** once the game asks for three colour buffers.

```bash
python3 scripts/voodoo2/fix_glide_games.py --host 192.168.1.171 --check
python3 scripts/voodoo2/fix_glide_games.py --host 192.168.1.171 --apply
```

Diagnosing: a GDI `SCREENSHOT` **cannot see this card** (3D leaves via the
passthrough cable), so judge by CPU-time delta and the game's own log. Full
write-up: [`docs/machines/192.168.1.171-NSC-5B996B81319.md`](../../docs/machines/192.168.1.171-NSC-5B996B81319.md).
