# Why the rebuilt open Glide can't bring up the card on retail Windows XP

Traced 2026-07-15 on `.124` (real Voodoo3, WinXP, retail `3dfxvsm.sys`). After
fixing the DLL ABI so `glide3x.dll` actually loads (see `docs/3dfx-drivers.md`
and `build-glide.sh`), `gfxbench` runs, calls `grGlideInit()`, prints the debug
banner, then dies with a fatal `gd error (glide):` during hardware detection.
Root cause is **not** in our build — it's an open-Glide vs retail-driver
interface mismatch.

## The trace (how we found it)

- Built a **debug** `glide3x.dll` (`GDBG_INFO` compiled in) and registered a
  Glide error callback in the backend so the fatal error logs instead of
  popping the invisible `MessageBox(...) + exit(1)` that was hanging the process
  headlessly. Output stops right after `grGlideInit`'s banner
  (`gpci.c` "GLIDE DEBUG LIBRARY" / "DLL path").
- `grGlideInit` (`gpci.c:668`) calls `hwcInit(0x121a, 0x5)` (Voodoo3) then
  `hwcInit(0x121a, 0x3)` (Banshee); if both return NULL it `goto __errExit` and
  the init fails.
- `hwcInit` / `hwcMapBoard` (`minihwc.c`) branch on OS
  (`GetVersionEx` → `VER_PLATFORM_WIN32_NT`). Our build compiles the **NT path**
  (`-DHWC_EXT_INIT=1`), which accesses the board via
  **`ExtEscape(hdc, hwcEscape, HWCEXT_GETLINEARADDR, …)`** to the 3dfx *display
  driver*, and (`minihwc.c:~4188`) uses a **hardcoded registry path**
  `SYSTEM\CurrentControlSet\Services\3Dfx\Device0\glide`.
  (The `\\.\CONFIGMG` Configuration-Manager path in the same file is the **9x**
  branch, not used on NT.)

## Confirmed on the box

| Open Glide expects | Reality on `.124` (retail XP driver) |
|---|---|
| service `HKLM\...\Services\3Dfx\Device0\glide` | **absent** ("unable to find the specified registry key") |
| a display driver answering the `HWCEXT_*` `ExtEscape` codes | retail service is **`3dfxvs`** (`system32\DRIVERS\3dfxvsm.sys`) — different driver, does not implement the open Glide's HWCEXT escape interface |
| (9x fallback) `\\.\CONFIGMG` | N/A — Windows XP 5.1.2600 (no 9x Config Manager) |

So `hwcInit` can't obtain the board's linear address / map registers through the
retail driver → returns NULL → `grGlideInit` fatal error. **The open Glide's
Windows/NT hardware layer targets the 3dfx *reference* driver, not the retail
`3dfxvs`/AmigaMerlin/SFFT drivers.**

## What this means

- Our rebuilt `glide3x.dll` is a correct, ABI-compatible **API** library (loads,
  exports `grFoo`/`grFoo@N`, links against callers) — good as the fxD3D HAL's
  link target and for source modification. It is **not** a runnable Glide
  runtime on top of the retail XP 3dfx driver.
- It is **not** a bug we introduced — it's inherent to the open Glide's Windows
  hardware-access design (reference-driver HWCEXT escapes).

## Paths forward (pick per goal)

1. **Run/benchmark on real XP hardware now** → use the **retail `glide3x.dll`**
   (ships with the 3dfx driver package or with Glide games). It matches the
   retail driver's interface. `gfxbench`/`benchmark_runner` translate to `grFoo`
   calls that work with *any* glide3x.dll, so drop the retail runtime next to
   the exe. (`.124` currently has no `glide3x.dll` installed at all — that's why
   nothing Glide runs there yet.)
2. **Try our Glide on Win9x** (e.g. the Voodoo5 5500 on `.50`, Win98) — 9x is the
   open Glide's primary Windows target; the 9x path + an AmigaMerlin/reference
   driver may satisfy it. Untested.
3. **Port `minihwc` NT init to the retail driver** — teach the HWCEXT path to use
   the retail `3dfxvs` service + its escape codes (or a DirectDraw-only mapping
   via `HWC_ACCESS_DDRAW`). Real work; only worth it to ship our *modified* Glide
   on XP.
4. **fxD3D driver itself is unaffected** — the shipped display driver compiles
   Glide's `minihwc` init *into* the driver and *is* the hardware access; it
   doesn't `ExtEscape` to a separate driver. This finding only concerns running
   the standalone `glide3x.dll` on top of the retail driver.

## Verbose logging (for future digs)

- Build a debug DLL: `build-glide.sh --debug` (adds `DEBUG=1` → `GDBG_INFO` on).
- The backend registers `grErrorSetCallback` **before** `grGlideInit` so fatal
  errors are logged (to `C:\RETRO_AGENT\glide_err.log`) and don't hang on the
  MessageBox. Note: `gdbg_init` doesn't parse `GDBG_LEVEL` early enough to expose
  the level‑80 traces on this build, so only level‑0 (banner + error) shows on
  stdout; the source trace above closed the gap.
