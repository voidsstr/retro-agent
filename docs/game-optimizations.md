# Retro Fleet Game Optimizations Registry

A running registry of per-game tweaks that make titles run correctly and fast on
the retro fleet. Each entry states the symptom, the root cause, the exact change,
and which machines it has been applied to. Deep write-ups live in
[`case-studies/`](case-studies/); this file is the quick-reference index and the
fleet rollout tracker.

Conventions used throughout:

- Paths use the 8.3 short form (`C:\Games\COMMAN~1\RA2`) because the real folder
  names contain spaces and `&`, which break `EXEC`/`LAUNCH` on the agents.
- "no SSE2" machines are anything pre-Pentium-4 (Pentium II/III, early Athlon).
  Any binary built with SSE2 dies with `STATUS_ILLEGAL_INSTRUCTION`
  (`0xC000001D`) the instant that code runs, with no crash log. See
  [`machines/192.168.1.133-P3-DUAL.md`](machines/192.168.1.133-P3-DUAL.md).

---

## OPT-001 — Red Alert 2 / Yuri's Revenge: fullscreen + fast on GDI-class GPUs

**Games:** Command & Conquer Red Alert 2 + Yuri's Revenge (CnCNet "Win10 Fixed"
repack), installed at `C:\Games\COMMAN~1\RA2` on every fleet box that has it.

**Symptoms addressed:**

1. **Black screen / silent no-launch** on no-SSE2 CPUs. The repack ships an
   *aqrit* DirectDraw wrapper (`ddraw.dll`, ~27 KB, SSE2-compiled) and an SSE2
   CnCNet `wsock32.dll` (~49.6 KB). On a Pentium III both fault with
   `0xC000001D` the moment they load — no Application-log entry, no `except.txt`,
   so it looks like a video hang.
2. **In-game slowness** once it does render. cnc-ddraw's only viable renderer on
   DX7/DX8 GPUs (GeForce2/3/4, Voodoo, TNT) is `renderer=gdi`, a CPU blit. If the
   game renders 800x600 and cnc-ddraw upscales it to a larger desktop
   (e.g. 1024x768) it does that stretch **on the CPU every frame**, which drags a
   Pentium III to a crawl.
3. **Windowed / not filling the screen**, or minimize-to-black when the player
   Alt-Tabs to the Retro Chat window on the same machine.

**Root cause:** SSE2 code on a non-SSE2 CPU (#1); per-frame CPU upscaling under
the GDI renderer (#2); wrong cnc-ddraw window/fullscreen flags (#3).

**Fix — the known-good stack (SSE2-free, GDI, native-res exclusive fullscreen):**

1. Replace `ddraw.dll` with the **cnc-ddraw i486 build** (`371,200` bytes, from
   `cnc-ddraw_win2000_i486_mingw.zip`, release v6.6.0.0). i486 predates SSE
   entirely, so it is verified SSE2-free (`objdump -d ddraw.dll | grep xmm` is
   empty) and runs on *every* CPU on the fleet, P3 through Sandy Bridge.
2. On **no-SSE2** machines only, replace the CnCNet SSE2 `wsock32.dll` with a
   **stock XP `wsock32.dll`** (`22,528` bytes). (On SSE2 machines the CnCNet
   spawner is fine and is left in place for online play.)
3. Write this `ddraw.ini` (the decisive part is the **exclusive fullscreen at the
   game's native 800x600** — the display switches to 800x600 so a CRT/LCD fills
   the screen itself and there is **zero per-frame CPU scaling**):

   ```ini
   [ddraw]
   renderer=gdi
   windowed=false
   fullscreen=false
   border=false
   maintas=false
   boxing=false
   width=0
   height=0
   maxfps=60
   vsync=false
   singlecpu=true
   nonexclusive=false
   noactivateapp=true
   adjmouse=true
   savesettings=0
   resizable=false
   ```

   Key flags:
   - `windowed=false` + `fullscreen=false` + `nonexclusive=false` +
     `width=0/height=0` = **real exclusive fullscreen at the game's own
     resolution** (800x600). This is what makes it both fullscreen *and* fast —
     no GDI stretch. (`windowed=true, fullscreen=true, width=1024` filled the
     screen but CPU-upscaled and was slow; `windowed=false, fullscreen=true`
     rendered 1:1 but did not fill the screen. The combo above is the one that
     wins on both counts.)
   - `renderer=gdi` — the only renderer that initializes on DX7/DX8 GPUs.
   - `singlecpu=true` — pins the dual/quad boxes to one core (RA2 is
     single-threaded; multi-CPU otherwise causes timer issues). Replaces the old
     `start /affinity` / `__COMPAT_LAYER=WIN95` hacks.
   - `noactivateapp=true` — stops minimize-to-black when the player Alt-Tabs to
     the chat window on the same machine.
   - **Never set `tshack=true`** on GeForce/GDI — it renders to an undisplayed
     surface (black). Use a plain global `[ddraw]` section with no per-game
     override.

**Verification (remote):** while the game runs, `DISPLAYCFG get` should report
`{width:800, height:600}` — proof the exclusive mode-switch happened. An exclusive
DirectDraw `SCREENSHOT` may read back black even when the monitor shows the game;
`DISPLAYCFG get` plus a resident `game.exe` in `PROCLIST` is the reliable remote
check. (On this GDI path the screenshot did come back non-black and edge-to-edge.)

**Notes on capable machines:** on a modern GPU (Radeon HD, Intel HD) the game
could instead use a hardware cnc-ddraw renderer, but RA2 is a locked-800x600,
16-bit engine — GDI at 800x600 is effortless for a P4/Core, and the i486 build is
the one binary that is safe on the whole fleet. The stack above is deployed
uniformly for consistency and known-good reliability.

Full narrative: [`case-studies/003-red-alert-2-pentium3-sse2-cnc-ddraw.md`](case-studies/003-red-alert-2-pentium3-sse2-cnc-ddraw.md).

Staged copies of the two binaries live on the share at
`Z:\Utility\Retro Automation\ra2-opt\` (`ddraw_i486.dll`, `wsock32_stock.dll`)
so any fleet box can pull them locally.

### Fleet rollout status

| IP | Host | CPU / SSE2 | GPU | Action | Status |
|----|------|-----------|-----|--------|--------|
| 192.168.1.133 | P3-DUAL | dual Pentium III / no | GeForce4 Ti 4600 | full stack (i486 ddraw + stock wsock32 + ini) | DONE — verified fast+fullscreen |
| 192.168.1.124 | ADMIN | Pentium III Coppermine / no | GeForce2 GTS | full stack (i486 ddraw + stock wsock32 + ini) | DONE — was crashing on SSE2, now runs 800x600 exclusive fullscreen |
| 192.168.1.123 | 2004-XP | Pentium 4 / yes | Radeon HD 3850 AGP | i486 ddraw + ini (kept SSE2 wsock32) | DONE — 800x600 exclusive fullscreen verified |
| 192.168.1.145 | DELL | Sandy Bridge quad / yes | Intel HD Graphics | i486 ddraw + ini (kept SSE2 wsock32) | DONE — 800x600 exclusive fullscreen verified |
| 192.168.1.143 | 1GHZ | 586-class / — | — | RA2 not installed | n/a |

Applied 2026-07-09. Per-machine notes:

- **.124 (ADMIN)** was the important find — a single Pentium III Coppermine (no
  SSE2) + GeForce2 GTS, i.e. the same trap as P3-DUAL. It was silently crashing
  RA2 (`0xC000001D`) on both the aqrit SSE2 `ddraw.dll` and the SSE2
  `wsock32.dll`. Got the full stack (i486 ddraw + stock wsock32 + ini). Also set
  `ra2.ini` to 800x600 — this is the *weakest* box on the fleet (single P3, 383 MB
  RAM), so 800x600 both fills the screen and cuts the software-render load.
- **.123 / .145** are SSE2 boxes with capable GPUs (Radeon HD 3850 / Intel HD)
  that already ran RA2, but were standardized onto the same known-good stack for
  consistency and to eliminate any future SSE2 risk. Their SSE2 CnCNet
  `wsock32.dll` was left in place (needed for online). Both had `ra2.ini` moved
  from 1024x768 to 800x600 so the menu *and* battlefield fill the screen.
- Originals are backed up next to each game as `ddraw.dll.aqrit-bak` (and, on the
  no-SSE2 boxes, `wsock32.dll.cncnet-sse2`). To revert: copy those back.
- **Preference note:** on the fast boxes (.123/.145) `ra2.ini` can be set back to
  `ScreenWidth=1024/ScreenHeight=768` for more battlefield view — in-game still
  fills the screen (exclusive switches to 1024x768); only the 800x600 menu would
  then have black borders. On the 1080p LCD box (.145) RA2 is inherently soft
  (a low-res game upscaled by the panel); a hardware cnc-ddraw renderer
  (`renderer=opengl`, windowed fullscreen) would look crisper there if desired.

---

## OPT-002 — Red Alert 2 / Yuri's Revenge: LAN "Network" play via IPX

**Symptom:** the in-game **Network** (LAN) button is dead — the lobby won't
connect, no games can be hosted or seen.

**Root cause:** retail RA2's "Network" mode is an **IPX/SPX** LAN, *not*
winsock/TCP. The CnCNet repack normally tunnels networking over TCP/IP via its
SSE2 `wsock32.dll` spawner, but on no-SSE2 boxes that spawner is removed (see
OPT-001), and **IPX is not installed by default on XP**. With no IPX transport,
the Network lobby has nothing to run on. This is *independent* of the wsock32
swap — even the stock repack "Network" needs IPX; the repack expected you to play
online through the (SSE2) CnCNet client instead.

**Fix:** install the **NWLink IPX/SPX/NetBIOS Compatible Transport Protocol** on
every machine that will join a LAN match. XP already ships the driver + INF
(`nwlnkipx.sys`, `nwlnknb.sys`, `%windir%\inf\netnwlnk.inf`), so no install CD is
needed. Stock installs have **no headless net-config tool** (`snetcfg`/`netcfg`
absent), so install through the GUI:

  Network Connections (`control ncpa.cpl`) -> right-click the LAN adapter ->
  Properties -> **Install...** -> **Protocol** -> Add... ->
  **NWLink IPX/SPX/NetBIOS Compatible Transport Protocol** -> OK -> Close.

Verify with `ipxroute config` — the LAN adapter should appear bound with a frame
type. Then RA2 -> **Network** loads the IPX lobby: **New** hosts a game, **Join**
joins one.

**Frame-type gotcha:** the default frame type is **802.2 (auto)**. Every machine
in a match must use the **same** IPX frame type or they won't see each other's
games. Keep them all on the default (or force one common type on all).

### Fleet rollout status (IPX for LAN)

| IP | Host | IPX installed | Notes |
|----|------|---------------|-------|
| 192.168.1.133 | P3-DUAL | YES (802.2) | verified — Network lobby loads, host/join available |
| 192.168.1.123 | 2004-XP | YES (802.2) | installed, `ipxroute` confirms 802.2 — matches .133 |
| 192.168.1.145 | DELL | no | agent offline; needs IPX + 802.2 when back up |
| 192.168.1.124 | ADMIN | no | agent offline; needs IPX + 802.2 when back up |

A LAN game needs at least two IPX-enabled machines, so the opponents must get the
same protocol before an actual match can run. **Why "not seeing anyone" happens:**
the opponent machine has no IPX (only .133 had it initially), OR the two machines
are on different IPX frame types. Auto-detect on a quiet LAN lands on 802.2 for
everyone, so leaving all machines on the default (802.2) is the simplest way to
keep them matched; if `ipxroute config` ever shows a machine on a different frame,
pin it to 802.2. Frame type is stored at
`HKLM\SYSTEM\CurrentControlSet\Services\NwlnkIpx\Parameters\Adapters\{nic-guid}\PktType`
(`FF`=auto; a pinned value only takes effect after a reboot / adapter re-enable).

**Agent-stability caveat:** launching RA2 in the OPT-001 exclusive-fullscreen mode
makes the `retro_agent` remote control **unreachable on the non-GeForce4 boxes**
(.123 Radeon HD 3850, .124 GeForce2, .145 Intel HD all dropped off on RA2 launch;
only .133's GeForce4 Ti survives it). The game and Windows keep running — it's the
remote agent that stops responding — but it means these machines can't be
remote-driven while RA2 is up.

Root cause (from reading `agent/src/main.c` + `screen.c`): it is **not** the agent
looping on the display — the accept loop only does `select()`/`accept()` and never
touches GDI, and screenshots run on demand in per-connection threads. The port
dies with **no command sent**, right on RA2's `ChangeDisplaySettings` to the
exclusive 800x600 primary. So the OPT-001 exclusive mode-set is stalling the
box/driver externally on the non-GeForce4 GPUs (errno 111 = port momentarily
closed, then recovers — a driver stall or an agent crash the service auto-restarts;
.133's GeForce4 Ti driver is immune). `.124` instead went errno 113 (no route =
powered off / hung), a separate matter.

**Mitigation:** on capable GPUs (Radeon HD, Intel HD) use **non-exclusive
windowed-fullscreen** instead of exclusive — `windowed=true, fullscreen=true,
nonexclusive=true` (upscales to fill; trivial cost on a P4/Core). It does **no**
exclusive mode-set, so the agent stays reachable while RA2 runs, and the display
stays screenshot-able. Keep true exclusive (OPT-001) only where native-res speed
matters on a weak GPU (the GeForce2/GeForce4 P3 boxes), accepting that the agent
goes dark while the game is up there.
</content>
</invoke>
