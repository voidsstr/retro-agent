# 3dfx Control Panel (`3dfxctl.exe`) — what it controls, build, deploy, verify

A Win32 settings panel for a 3dfx card that knows **which driver stack** drives
it, writes only values that stack reads, and applies them **without a reboot**.

| lane | detected by | writes |
|---|---|---|
| **our stack** (clean-room: `vcr-kmd` kernel pair + our h5 Glide + our MesaFX ICD) — the V5 6000 in `.124` | the display driver answers `VCR_ESC_INFO` (`include/vcr_ioctl.h`) | the Glide registry key, the user environment, 4 kernel `Diag` switches, the desktop refresh, **the graphics clock** (live, through the driver's `VCR_ESC_CLOCK`) |
| **vintage** (`3dfxvs`: retro-3dfx H5 source / AmigaMerlin) | `Services\3dfxvs` exists and the primary adapter is a 3dfx | `...\3dfxvs\Device0`, `\D3D`, `\glide` — the first 3dfxctl's rows, unchanged |

`3dfxctl.exe /vcr` or `/vintage` forces a lane (testing only);
`3dfxctl.exe /report FILE` writes what the panel sees and exits (for the agent);
`3dfxctl.exe /startup` is the logon half of "use this clock again" (no window, below).

Tabs: **Overview** (driver stack, monitor, presets, alerts) · **3D & Glide** ·
**Anti-aliasing & SLI** · **OpenGL** · **Display & 2D** · **Clock** (2.1.0; only
when the driver answers `VCR_ESC_CLOCK` for a Voodoo) · **Advanced** (where
settings live, overrides found elsewhere, driver details, the panel's log). A
header shows the card, chips, memory per chip (and the 256/128 MB VBIOS mode),
the desktop mode and refresh, SLI and AA state and the graphics clock, refreshed
every 2 s. Every
control has a tooltip and a "when it applies" tag. Presets (Maximum quality /
Maximum speed / Driver defaults) only fill the tabs; nothing is written until
**Apply** / **OK**. Every write is read back and listed in an Apply summary
and in `3dfxctl.log` beside the exe.

## Our stack: every setting, where it lives, who reads it, when

Line numbers: our h5 Glide fork `glide-devel-sezero` @ `e767d89` (the
SLIAA-GUARD build on `.124`, `voodoo-cleanroom/build/retro3dfx-glide/glide3x/h5/`),
our MesaFX ICD 0.1.75 (`voodoo-cleanroom/build/retro3dfx-gl/src/mesa/drivers/glide/`),
`voodoo-cleanroom/vcr-kmd/` at this commit. `tests/python/test_3dfxctl_settings.py`
re-checks every name against these sources.

**Where our Glide reads the registry** (`minihwc/minihwc.c` `hwcGetenv` 9547-9594,
`getRegPath` 1403-1409): the process environment first, then
`HKCU\<key>`, then `HKLM\<key>`, text values only, where `<key>` is
`SYSTEM\CurrentControlSet\Services\3dfxvs\Device0\glide` **if
`Services\3dfxvs\Device0` exists** (it does on `.124`: AmigaMerlin's service key
is still there) and `...\Services\banshee\Device0\glide` otherwise. The panel
decides the key the same way and says which on the Overview tab. It writes
`HKLM`; a copy in `HKCU\<key>` or the environment **wins**, and the panel shows
such overrides in red and can remove them. Our ICD reads its own settings with
plain `getenv()` only — never the registry — so those live in the user
environment (`HKCU\Environment`).

| control | writes | read by | takes effect |
|---|---|---|---|
| Vertical sync | `HKCU\Environment` `FX_GLIDE_SWAPINTERVAL` = absent / 0 / 1 / 2 | ICD `fxapi.c:761` (getenv); Glide `gpci.c:1868` → `gglide.c:2749` (grBufferSwap) | games started from the desktop / Start menu after Apply ¹ |
| Frames queued ahead | `HKCU\Environment` `FX_GLIDE_SWAPPENDINGCOUNT` = absent / 0-3 | ICD `fxapi.c:744`; Glide `gpci.c:1887` → `gglide.c:2762` | same ¹ |
| Fullscreen refresh rate | `HKLM\<key>` `FX_GLIDE_REFRESH` = absent (Auto) / a rate the driver lists | Glide `minihwc.c:4675` (`hwcInitVideo`, every board open; a rate the mode lacks falls back to the driver default, `win_mode.c:297-350`) | next game launch |
| Mipmap dithering | `HKLM\<key>` `FX_GLIDE_LOD_DITHER` = absent / 1 | Glide `gpci.c:1850` → `gtex.c:2549` | next game launch |
| 16-bit alpha blending | `HKLM\<key>` `SSTH3_ALPHADITHERMODE` = absent / 3 | Glide `gpci.c:1837` → `gglide.c:3900` (dither subtraction; off at 32 bpp) | next game launch |
| 16-bit video filter | `HKLM\<key>` `SSTH3_OVERLAYMODE` = absent / 2 / 3 / -1 | Glide `minihwc.c:4978` (`hwcInitVideo`) | next game launch |
| Glide gamma | `HKLM\<key>` `SSTH3_RGAMMA`, `SSTH3_GGAMMA`, `SSTH3_BGAMMA` (one value to all three; absent = 1.00) | Glide `gpci.c:1895-1897` → `gsst.c:2425` (at board open) | next game launch |
| Lock gamma to the Glide gamma | `HKLM\<key>` `FX_GLIDE_USE_APP_GAMMA` = absent / 0 | Glide `gpci.c:1899` → `gsst.c:3718, 3755, 3811` | next game launch |
| Skip the 3dfx splash plugin | `HKCU\Environment` `FX_GLIDE_NO_PLUGIN` = absent / 1 (only offered when `3dfxspl3.dll` is installed) | Glide `gsst.c:1162` (getenv, exactly "1") | same ¹ |
| SLI / anti-aliasing | `HKLM\<key>` `SSTH3_SLI_AA_CONFIGURATION` (see AA below) + `Diag\SliAA` | Glide `gpci.c:1770` (`h5SliAaConfigEnv`, `minihwc/h5sliaa.h:33`); ICD `fxapi.c:579` via `grGetRegistryOrEnvironmentStringExt` = `hwcGetenv` (`diget.c:969`); kernel `vcrmp_multi.c:253` (`sli_aa_allowed`, every SLI/AA request) | next game launch |
| OpenGL gamma | `HKCU\Environment` `FX_GAMMA` (absent = 1.30) | ICD `fxapi.c:858` | same ¹ |
| OpenGL dithering | `HKCU\Environment` `FX_DITHER` = absent (4x4) / 0 (2x2) | ICD `fxapi.c:876` | same ¹ |
| Texture sharpness | `HKCU\Environment` `FX_LOD_BIAS` = absent (-0.5) / -1.0 / 0 / 0.5 | ICD `fxsetup.c:566, 612` | same ¹ |
| Desktop refresh rate | the display mode (`ChangeDisplaySettingsEx`, `CDS_UPDATEREGISTRY`), only rates the driver lists for the current resolution/depth, none when the list is not filtered by the monitor | vcr-kmd mode set | **now**: one paced switch, a 15 s keep-or-revert dialog, read back (current, saved, and the driver's `cur_hz`) |
| Text / Pattern fills / Straight lines on the 2D engine | `HKLM\...\Services\vcrmp\Diag` `Accel2DText` / `Accel2DPattern` / `Accel2DLine` = absent / 1 (REG_DWORD) | kernel `vcrmp.c:436-438` (`fill_info`, answering `IOCTL_VCR_INFO`, `vcrmp.c:741-747`) → display `vcrdd_2d.c:446-448` (`VcrDd2dInit`, called from `DrvEnableSurface`, `vcrdd.c:524`) | **now**: two paced switches (below), confirmed from the driver's `VCR_ESC_2D_STATS` |
| Graphics clock (the Clock tab) | nothing in the registry: `VCR_ESC_CLOCK` SET/RESTORE, 133-200 MHz, presets Stock / 150 / 175 / 183 | vcr-kmd `vcrmp_clock.c` `VcrCoreClock` (the master chip, <= 5 MHz steps, read back); every SLI enable copies it to the other chips (`vcrmp_sli.c` `init_slave`) | **now**: one press of Set; an overclock asks to be kept and goes back by itself after 15 s; chips 1-3 at the next game start |

¹ The environment is per process and copied from the parent: Explorer re-reads
it on the `WM_SETTINGCHANGE("Environment")` the panel broadcasts, so a game you
start from the desktop or the Start menu after Apply gets it. A program started
by something that was already running — **the retro agent's `LAUNCH`/`EXEC`
included** — keeps that parent's old environment until the parent restarts.

### "Without reboot", group by group

- **Glide registry values**: read by Glide at `grGlideInit`
  (`_GlideInitEnvironment`, `diglide.c:349`) or at each board open
  (`hwcInitVideo`) — the next game you start. No reboot.
- **Environment values**: the next game started from the desktop / Start menu
  (¹). No reboot, no logoff.
- **`Diag\SliAA`**: read at every SLI/AA request — the next Glide/OpenGL game.
- **`Diag\Accel2D*`**: read when the display driver builds a new surface.
  **A same-mode `ChangeDisplaySettingsEx(CDS_RESET)` is NOT one** — measured on
  XP SP3 with vcr-kmd in the QEMU test bed (2026-09-28): the recorder showed
  only `DrvAssertMode(FALSE/TRUE)`, no `DrvEnableSurface`, so the switches were
  never read. So on Apply the panel passes through another mode the driver
  lists **at the same resolution** (nearest lower refresh, else nearest higher,
  else the other of 16/32 bpp) and back: two switches through
  `tools/vcr_pace.h` (≥ 3 s apart, box-wide lock), the first as a temporary
  `CDS_FULLSCREEN` mode (so XP reverts it if the panel dies). The recorder then
  showed `DrvEnablePDEV` + `DrvEnableSurface` at the pass-through mode and again
  at the original one. If no such mode exists, or a Glide program holds the
  display, nothing is switched and the summary says the switches apply at the
  next mode change (a fullscreen game starting or ending) or the next boot.
- **Desktop refresh**: applied at once (one paced switch), 15 s to keep it.
- **Graphics clock**: applied at once on the Clock tab's own **Set** button (the
  bottom Apply/OK never touch it). Chip 0 moves the moment Set is pressed; chips
  1-3 are given chip 0's clock at every game start on all four chips - between
  games they idle at their reset word (0x0C01) or the last game's clock, and the
  tab says which. Refused while a 3D game holds the card (the driver refuses as
  well: `VCR_CLOCK_R_EXCLUSIVE`).
- Nothing the panel offers needs a reboot. Boot-time switches are not offered.

### The Clock tab - live, kept or put back, and "use this clock again"

- **Range 133-200 MHz** (the driver takes 120-219): below is nothing worth
  having, above it the 4-chip board's memory - which runs from the same clock -
  is a fifth past its rating. The value label shows what the PLL really gives
  (`vcr_clock.h`, the driver's own rule): 150 -> 149.7 MHz.
- **An overclock goes back by itself.** Above stock (`ctl_clock_confirm`) a
  topmost "keep this clock?" counts down 15 s with **Go back** as the default
  button, then sets the previous clock again (RESTORE if that was stock). Stock
  and below never ask - that is the direction a person goes to get away from a
  problem. **Stock** ends on the VBIOS's exact word, not a recomputed one.
- **"Use this clock again after Windows restarts"** (off by default) saves the
  card's clock in `HKLM\SOFTWARE\3dfxctl` `StartupClock`, stamped with Windows'
  own record of its last orderly shutdown (`HKLM\SYSTEM\CurrentControlSet\
  Control\Windows` `ShutdownTime` -> `StartupSeen`), and adds
  `HKLM\...\Run\3dfxctlClock` = `"<exe>" /startup`. At logon `/startup`
  re-applies the clock **only if Windows' stamp moved since** - i.e. the session
  that ran the clock ended in a clean shutdown - and consumes the stamp BEFORE the
  clock moves, so a crash in the next session is caught the same way. If the stamp
  did not move (a crash, a lock-up, the power) or either stamp is missing, it
  forgets the saved clock, removes the Run value, and leaves a note the Clock tab
  shows ("Last logon: NOT re-applied: ..."). Equality only, never order: `.124`'s
  clock resets after a power loss. **A logon is not a boot:** the Run value fires
  at a logoff/logon too, with the stamp unmoved - so when the card STILL RUNS the
  saved clock (its word is the saved clock's; the VBIOS puts its own back at every
  power-on) `/startup` does nothing at all. The first 2.1.0 build judged that
  case a crash, forgot the saved clock and wrote that the card was at stock. Measured on `.124`: an agent REBOOT through
  `safe-reboot.py` writes `ShutdownTime` too (04:50:56 and 05:01:18 UTC, two
  reboots). A clock set later while the box is ticked is saved only once KEPT.

### Anti-aliasing (experimental — read this)

`SSTH3_SLI_AA_CONFIGURATION` as the panel offers it on the V5 6000 (4 chips):

| value | mode | status on `.124` |
|---|---|---|
| 5 (or absent / 2) | SLI, all four chips | validated: fill 1124.6 Mpix/s = AmigaMerlin, Quake II 201 fps @640x480 (2026-09-26) |
| 0 | single chip | validated: Quake II 146.8 fps, whole stack ours (2026-09-26) |
| 6 | 2x AA | EXPERIMENTAL: runs (618.6 Mpix/s) but a ghost / double image (supervised, 2026-09-27; Glide treats 6 exactly as 3) |
| 7 | 4x AA | EXPERIMENTAL: **froze the whole PC** in Glide's open (2026-09-26, confounded, not re-run) |
| 8 | 8x AA | EXPERIMENTAL: never run on this board |
| 1 | single chip + 2x AA | **never offered**: refused by our guarded Glide on > 2 chips; an unguarded Glide froze `.124` with it |
| 3, 4 | 2-chip AA values | **never offered on 4 chips**: our ICD renders without AA (`fxapi.c:589-606`) while Glide forces AA (`h5sliaa.h:33-58`) — 6/7 mean the same to both |

AA modes are **not listed** until "Allow EXPERIMENTAL anti-aliasing modes" is
ticked, which shows a warning (the freeze and ghost history, and every
`glide3x.dll` without our `RETRO3DFX_SLIAA_GUARD` in the system folder and
`C:\Games\*`, `D:\Games\*`) whose **default button is No**. Choosing an AA
mode and pressing Apply writes the value **and** arms the kernel's AA kill
switch `Diag\SliAA = 1`; choosing SLI / single chip / default deletes
`Diag\SliAA`. "Disarm AA now" deletes it at once. No preset selects AA, the
confirmation is asked again every session, and an armed switch turns the header
red. Hidden overrides of the chip mode (`FX_GLIDE_AA_SAMPLE`,
`FX_GLIDE_NUM_CHIPS` in the environment or either Glide key) are shown and can
be removed. 2-chip boards get 0/2 and 3/4, a 1-chip board 0 and 1 — both
unproven on our stack and labelled so.

### Deliberately not offered (and pinned by the tests)

- **Boot-time kernel switches** `D3D32`, `Reset3D`, `AllowPoke`, `Accel2D`,
  `D3D`, `TexPortFlush` (read once in FindAdapter — a reboot each) and the
  **diagnostics** `SliPersistAll`, `SliAAVendorRecipe`, `SliAAReadback`,
  `FlipDeadline`, `DebugPort`, `EdidFilter`, `Ddc`, `Mon*`, `Disable` … The
  panel's writer refuses every Diag name but the four; it deletes values,
  never a key (that key holds the phase history and the kill switches).
- `FX_GLIDE_LOD_BIAS`: only offsets a game's own `grTexLodBiasValue()` call
  (`gtex.c:2455`) — for most Glide games it does nothing.
- `SSTH3_GRXCLOCK` / `SSTH3_MEMCLOCK`: read, but the PLL programming is compiled
  out of the Windows build (`HWC_ACCESS_DDRAW=1`, the `#if` at `minihwc.c:2360`).
  The clock is offered on the Clock tab instead, through the driver.
- `Diag\CoreClock`: the driver's clock kill switch - the panel never writes it.
- `FX_GLIDE_ANALOG_SLI`: forced to 1 on a 4-way board (`h5sliaa.h` `h5SliAaAnalog`).
- `FX_GLIDE_NO_SPLASH`: on Win32 `grSplash` draws only through `3dfxspl3.dll`;
  "Skip the splash plugin" (`FX_GLIDE_NO_PLUGIN`) is the switch that works.
- `FX_GLIDE_SLI_BAND_HEIGHT`: changes the SLI request the kernel programs; never measured.
- `FX_GLIDE_AA_TOGGLE_KEY` & co.: switch AA live, past every gate.
- ICD compatibility switches (`FX_NO_PALETTED_TEXTURE`, `FX_SGIS_MULTITEXTURE`,
  `FX_NO_MULTITEXTURE`): per-game workarounds belong in the title's launcher,
  not machine-wide.

## Build

```bash
cd scripts/3dfx/3dfxctl
make            # i686-w64-mingw32-gcc, static, GUI; resources via windres
```
Imports only kernel32, user32, gdi32, advapi32, comctl32 and msvcrt — every
name checked against the real XP SP3 export tables in `xp_exports/`
(`tests/python/test_3dfxctl_panel.py`); `uxtheme.dll` is loaded at run time.
The comctl32 v6 manifest (XP visual styles) and the icon (`make icon` →
`make_icon.py`, DIB entries XP can read) are resources. Rebuilds when
`vcr_pace.h` or `vcr_ioctl.h` change.

## The agent deploys it (agent 1.94.0+)

Every XP/2000 box with a present Banshee/Voodoo3/4/5 gets `3dfxctl.exe` and
`3dfxlogo.ico` (the 3dfx logo - `make_icon.py` builds `3dfxctl.ico` from
`3dfx_logo_src.ico`, 3dfx's own artwork) copied from the share's
`Utility\Retro Automation\3dfx\` at agent start, plus a "3dfx Control Panel"
desktop shortcut wearing the logo that GAMESYNC keeps. To ship a new build:
`make`, then publish BOTH files there (`push_3dfxctl.py <ip>` does it through a
box's Z:, or `sharewrite.py put`); each box picks it up at its next agent start.
`FxPanelBoot` in `HKLM\Software\RetroAgent` says what the last start did.

## Deploy to `.124` and verify (the owning session; nothing here was run on `.124`)

Preconditions: the V5 6000 on vcr-kmd, the user at the box (the panel is for a
person; AA must never be armed unattended — see the memory note "AA tests need
the user at the box").

1. **Install** (no display or driver change):
   ```bash
   python3 scripts/3dfx/3dfxctl/push_3dfxctl.py 192.168.1.124 --deploy
   ```
   It refuses while a `3dfxctl.exe` runs there, UPLOADs, copies to
   `C:\RETRO_AGENT\3dfxctl.exe`, **DOWNLOADs it back and compares md5**, makes
   "3dfx Control Panel" shortcuts in the All Users Start Menu and on the All
   Users desktop (icon from the exe; GAMESYNC sweeps desktop `.lnk`s it did
   not write, so the desktop one lasts until the next GAMESYNC run — the Start
   Menu one stays), and prints `3dfxctl.exe /report`. Check the report says
   `lane: vcr-kmd (our stack)`, 4 chips, 64 MB per chip, the Glide key
   (`...\3dfxvs\Device0\glide` on `.124`), `system glide3x.dll` guard state,
   `Diag\SliAA: absent`, and lists the current values (the vendor panel left
   `FX_GLIDE_REFRESH=75` and `SSTH3_SLI_AA_CONFIGURATION=5` there).
2. **Look at it**: the user opens it from the Start Menu (or `LAUNCH
   C:\RETRO_AGENT\3dfxctl.exe`, then `SCREENSHOT 0`). Check the header (card,
   `4 x VSA-100, 64 MB each = 256 MB (the 256 MB VBIOS mode)`, desktop mode, SLI
   idle, AA off), each tab, the tooltips, and that the Advanced tab's override
   list is what `/report` said.
3. **Glide values** (e.g. Mipmap dithering on, 16-bit video filter "4x1",
   Glide gamma 1.20) → Apply. Verify the post-condition, not the summary:
   `REGREAD HKLM SYSTEM\CurrentControlSet\Services\3dfxvs\Device0\glide` →
   `FX_GLIDE_LOD_DITHER=1`, `SSTH3_OVERLAYMODE=2`, `SSTH3_RGAMMA/GGAMMA/BGAMMA=1.20`.
   Then a Glide app reads them at its next start: a glidelab run
   (`voodoo-cleanroom/vcr-kmd/tools/glidelab_run.py 192.168.1.124 fill --cfg 5 ...`)
   or Quake II — the gamma is visible; `glidelab_run.py --collect` reads every
   Glide registry location back.
4. **Environment values** (Vertical sync On, OpenGL gamma, …) → Apply. Verify
   `REGREAD HKCU Environment` (as the console user). To see them in a game,
   start it **from the desktop icon** (`UICLICK` on it) — an agent `LAUNCH`
   inherits the agent's old environment (¹). Vsync On: a Quake II timedemo from
   the desktop icon is capped near the refresh rate (e.g. ≤ 85 fps at 85 Hz)
   instead of ~170-200.
5. **2D switches** (Text on the 2D engine) → Apply. Expect the monitor to
   re-sync twice, ≥ 3 s apart (paced). The summary must say `[OK] display
   passed through ... and is back at ...` and `[OK] the driver now reports text
   ON`. Cross-check: `REGREAD HKLM SYSTEM\CurrentControlSet\Services\vcrmp\Diag`
   → `Accel2DText=1`; `vcrctl log` shows `DrvEnableSurface` twice around the
   Apply; `C:\vcr\lastswitch.dat` was stamped. Turn it off again the same way.
   (A vcr-kmd older than the switch reports `[UNCONFIRMED]`/"predates this
   switch" instead — that is the honest answer, not a failure of the panel.)
6. **Desktop refresh**: pick another listed rate → Apply → the keep-or-revert
   dialog; let it count down and check `DISPLAYCFG get` is back at the old rate,
   then repeat and press Keep. Never pick a rate the driver does not list (the
   panel cannot).
7. **AA — only with the user at the box, per the v56k plan**: tick "Allow
   EXPERIMENTAL", read the warning, Yes; pick 2x AA → Apply; verify
   `SSTH3_SLI_AA_CONFIGURATION=6` and `EXEC reg query
   HKLM\SYSTEM\CurrentControlSet\Services\vcrmp\Diag /v SliAA` → `0x1`; the
   header is red. Then choose SLI → Apply (or "Disarm AA now") and verify
   `SliAA` is **gone** (`reg query` says it cannot find the value) and the value
   is 5. Never `REGDELETE` the Diag key.
8. **Clock** (the Clock tab; verified on `.124` 2026-09-29, evidence
   `evidence/20260929/`): Cool 150 -> Set: the message quotes the word read back
   (0000f929) and `C:\vcr\vcrctl.exe clock` agrees; Quick 175 -> Set -> leave the
   "keep this clock?" alone: 15 s later the card reads the old clock again; again
   and **Keep it**: it stays (0000da1d); Stock -> Set: 0000e721 exactly. Tick "use
   this clock again": `REGREAD HKLM SOFTWARE\3dfxctl` shows `StartupClock` and
   `StartupSeen` = Windows' current `ShutdownTime`, and the Run value exists.
   `EXEC C:\RETRO_AGENT\3dfxctl.exe /startup` in the SAME session must do
   NOTHING (the card still runs the clock: a logon, not a boot) - all three values
   stay. To see the crash path without crashing: `vcrctl clock restore` (the card
   back at the VBIOS's word, as after a reset), then `/startup`: the stamp has not
   moved, so the three values are gone and `StartupClockLast` says "NOT re-applied:
   Windows did not shut down cleanly ...". Across a real `safe-reboot.py` it must
   say "re-applied ..." and the card read the clock.
   Leave the box at Stock with the box unticked.
9. `DOWNLOAD C:\RETRO_AGENT\3dfxctl.log` — every change the panel made, with
   its read-back and pace-gate result.

Rollback: delete `C:\RETRO_AGENT\3dfxctl.exe` and the two shortcuts; the
values it wrote are listed in `3dfxctl.log` ("Driver defaults" removes every
value it manages).

## Vintage lane

The first 3dfxctl's 14 rows (clock, vsync Glide/D3D, refresh override, desktop
and Glide gamma LUTs with the desktop wash-out clamp, SLI/AA, band height,
overlay filter, alpha dither, LOD bias, FIFO size, tiling, verbose log) against
`...\3dfxvs\Device0` (`REG_DWORD`/`REG_BINARY`), `\D3D` (`SSTH3_*`) and `\glide`
(`FX_GLIDE_*`), with that driver's own "boot / mode set / game start" rule.
Only their names are checked against the vintage H5 tree (the W2K driver spells
its `SSTH3_` names as `RK_PREFIX"..."`); their readers were not re-audited in
this change. Its AA values also need the experimental confirmation.

## Tests

`tests/native/test_3dfxctl_logic.c` (the table and every decision, true source),
`tests/python/test_3dfxctl_settings.py` (every name against the Glide / ICD /
kernel source that reads it), `tests/python/test_3dfxctl_panel.py` (pace gate,
Diag writes, AA gating, XP imports, manifest, icon, a fresh build),
`tests/python/test_3dfxctl_clock.py` (the Clock tab: the escape only, the range,
keep-or-revert, the logon re-apply's stamp order).
Evidence from the QEMU test bed: `evidence/`.
