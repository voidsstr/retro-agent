# Case Study 003: Red Alert 2 Black Screen on a Pentium III — the SSE2 Wrapper Trap and the Minimize-to-Black Bug

## Problem Statement

A user on the retro fleet asked, over the Retro Chat bridge, to "just get Red
Alert 2 to run" on their machine (P3-DUAL, 192.168.1.133). The game launched but
showed only a **black screen with a mouse cursor** and never reached the menu.
What looked like a video-driver problem turned out to be two unrelated issues
stacked on top of each other: a **DirectDraw wrapper compiled with SSE2
instructions the Pentium III cannot execute**, and — once that was fixed — the
well-known **cnc-ddraw minimize-to-black bug**. Neither is a driver fault, and
the machine already had the newest possible driver for its GPU.

## Environment

- **Machine**: hostname P3-DUAL, 192.168.1.133, agent v1.5.0 on port 9898
- **CPU**: **dual Pentium III** (`processor_level 6`, family 586) — has SSE but
  **no SSE2** (SSE2 debuted with the Pentium 4)
- **RAM**: 1023 MB
- **OS**: Windows XP SP3 (5.1.2600)
- **GPU**: NVIDIA GeForce4 Ti 4600, ForceWare 93.71 (6.14.10.9371) — the last
  NVIDIA driver that supports the GeForce4 Ti series on XP. There is nothing
  newer to install; this was never a driver-version problem.
- **Game**: a "Win10 Fixed" CnCNet repack of Red Alert 2 + Yuri's Revenge at
  `C:\Games\Command & Conquer Red Alert 2 + Yuri's Revenge [Win10 Fixed] - V2\RA2`
  (short path `C:\Games\COMMAN~1\RA2` — the folder name contains `&`, so the 8.3
  short path avoids `command`-interpreter breakage in `EXEC`/`LAUNCH`).

## Phase 1: What the Repack Ships

`DIRLIST` of the game folder showed a CnCNet repack designed for modern Windows:

- `ddraw.dll.aqrit-off` + `aqrit.cfg` — aqrit's bitpatch DirectDraw wrapper,
  shipped **disabled**. Config had `ForceDirectDrawEmulation=1` and
  `SingleProcAffinity=1`.
- `wsock32.dll` + `wsock32.dll.cncnet-sse2` — CnCNet spawner (SSE2 variant kept
  as a backup — a hint that SSE2 was already known to be a problem here).
- `Ra2.exe` (loader) → spawns `game.exe` (the actual base RA2 engine).
- Launcher batch files `[1] Launch C&C Red Alert 2.bat` etc. The first-run batch
  sets `__COMPAT_LAYER=WIN95` and runs `ra2.exe -speedcontrol`.

`ra2.ini` `[Video]` was already sane: `VideoBackBuffer=no`, `ScreenWidth=1024`,
`ScreenHeight=768`. The RA2 **menu/shell is locked to 800x600**; only the
in-game battlefield uses the ScreenWidth/Height values.

## Phase 2: Reproducing and Mis-diagnosing

Initial launches of `Ra2.exe` left `game.exe` **resident at ~58 MB but black**.
On a dual-CPU box this looks exactly like the classic RA2 multi-processor timer
hang, so the first fixes tried were:

1. Enable the aqrit wrapper (`copy ddraw.dll.aqrit-off ddraw.dll`) — its
   `SingleProcAffinity=1` should pin the game to one core. **Still black.**
2. `start /affinity 1` to force single-CPU at the OS level. **Still black.**
3. `__COMPAT_LAYER=WIN95` + `-speedcontrol`. **Still black.**

Screenshots were pure black throughout, but that is **expected** for DirectDraw
exclusive-fullscreen: a GDI screen-capture of an exclusive DDraw primary surface
returns black even when the monitor shows the game. So the black screenshots did
not, on their own, prove the game was broken — the process was alive.

Key negative evidence: **no `except.txt`** (RA2's internal crash handler) and
**no Application-log error** were produced. The game was exiting/hanging
*cleanly*, not throwing an exception. That ruled out a normal crash and pointed
at video initialization silently failing.

## Phase 3: cnc-ddraw and the Illegal-Instruction Smoking Gun

The community-standard fix for "RA2 runs but black in fullscreen" is
[cnc-ddraw](https://github.com/FunkyFr3sh/cnc-ddraw) in **windowed** mode — no
exclusive fullscreen mode-set to fail, and (bonus) a normal window is
screenshot-able so the render can be verified remotely.

The latest cnc-ddraw (v7.1.0, Dec 2024) was deployed via the two-frame `UPLOAD`
protocol (the MCP `retro_command` is text-only and can't carry a binary payload;
the Python `RetroConnection` client was used instead). Result: `game.exe` now
**exited instantly** instead of hanging.

Capturing the exit code was the breakthrough:

```
EXEC cmd /v:on /c "cd /d C:\Games\COMMAN~1\RA2 & start /wait game.exe & echo EXITCODE=!errorlevel!"
-> EXITCODE=-1073741795
```

`-1073741795` = **0xC000001D = STATUS_ILLEGAL_INSTRUCTION**.

Modern cnc-ddraw (and the aqrit wrapper the repack shipped) are compiled with
**SSE2** instructions. The **Pentium III has no SSE2** — the moment `game.exe`
called into the wrapper's DirectDrawCreate, it hit an opcode the CPU can't
execute and was terminated. Because it's an OS-level illegal-instruction kill,
it leaves **no entry in the Application event log** and **no `except.txt`**,
which is exactly why every earlier attempt "silently" black-screened.

## Phase 4: The i486 Build

cnc-ddraw publishes CPU-specific build variants. The newest **SSE2-free** build
is `cnc-ddraw_win2000_i486_mingw.zip` (attached to release **v6.6.0.0**, Jun
2024). An i486 target predates SSE entirely.

It was verified SSE2-free **before** deploying, by disassembling on the Linux
host:

```bash
i686-w64-mingw32-objdump -d ddraw.dll | grep -i xmm    # -> empty (no SSE/SSE2 at all)
```

(The only `movsd` matches were the x87-era `rep movsd` string instruction, not
the SSE2 scalar-double `movsd` — no `xmm` operands anywhere.)

Deployed `ddraw.dll` (i486) + a minimal `ddraw.ini`, launched `Ra2.exe`, and the
**RA2 main menu rendered and animated** — first time the game was ever visible.

## Phase 5: The Minimize-to-Black Bug

The user still reported black. The clue was their phrasing: *"it's still open but
minimized."* The desktop wallpaper is black, so a **minimized** game = a black
screen with a cursor — indistinguishable, to the user, from the original hang.

This is a documented cnc-ddraw + RA2 behavior: the game
[comes back black / as a tiny unresponsive window after being minimized](https://forums.cncnet.org/topic/7350-have-to-restart-game-if-minimized/)
([cnc-ddraw #448](https://github.com/FunkyFr3sh/cnc-ddraw/issues/448)). It bites
hard here specifically because **the user chats with the operator from the same
machine** — every time they Alt-Tab to the Retro Chat window, RA2 loses focus
and minimizes to black.

The fix is `noactivateapp=true`, which stops the game minimizing on focus loss.
Verified by stealing focus to another window via `UICLICK` and confirming (via
`WINLIST`) that the RA2 window **stayed on-screen** (rect stayed positive instead
of collapsing to the `-32000,-32000` minimized coordinates) and was still
rendered in the screenshot.

### The `tshack` trap

cnc-ddraw's shipped `ddraw.ini` has per-game sections (`[game]`, `[ra2]`,
`[gamemd]`, `[ra2md]`) that set **`tshack=true`** (a Tiberian-Sun/RA2 engine
hack). On this GeForce4 + i486-GDI combination, enabling `tshack` sent rendering
to a **black/undisplayed surface** — the screenshot went fully black again even
though the window was present and "visible." Using a plain global `[ddraw]`
section (no per-game override, so `tshack` stays default-off) rendered correctly.
**Do not enable `tshack` on this machine.**

### Renderer choice

`renderer=gdi` is the only viable option. The GeForce4 Ti is OpenGL 1.4 / DX8
hardware, so cnc-ddraw's `opengl` (needs GL 2.0 / GLSL) and `direct3d9`
renderers can't initialize. GDI is the CPU-blit renderer and it displays fine.

## The Working Configuration

- **`ddraw.dll`**: cnc-ddraw **i486** build from
  `cnc-ddraw_win2000_i486_mingw.zip` (v6.6.0.0). SSE2-free — mandatory on the P3.
- **`ddraw.ini`** (global `[ddraw]` section, no per-game override):

  ```ini
  [ddraw]
  renderer=gdi
  windowed=true
  fullscreen=false
  border=true
  maintas=false
  boxing=false
  width=800
  height=600
  maxfps=60
  vsync=false
  singlecpu=true
  nonexclusive=true
  noactivateapp=true
  adjmouse=true
  savesettings=0
  resizable=false
  ```

- Launch normally via `[1] Launch C&C Red Alert 2.bat` (it loads the local
  `ddraw.dll`/`ddraw.ini` automatically). No `__COMPAT_LAYER` or `/affinity`
  wrapper needed — `singlecpu=true` handles the dual-CPU pinning.

Verified end-to-end: menu renders and animates, mouse input reaches the game
(clicked into Options and back), and the window survives focus loss.

> Note: the `width=800/height=600/border=true` window-size request was ignored —
> cnc-ddraw kept a screen-filling borderless window. Cosmetic only; it renders
> and plays. Left as-is.

## Timeline

| Step | Action | Result |
|------|--------|--------|
| 1 | SYSINFO / VIDEODIAG | P3 dual-CPU, GeForce4 Ti, driver already newest (93.71) |
| 2 | Enable aqrit wrapper, `/affinity 1`, WIN95 compat, `-speedcontrol` | Still black; no crash log |
| 3 | Deploy latest cnc-ddraw (v7.1.0), capture exit code | `0xC000001D` illegal instruction — SSE2 on a non-SSE2 CPU |
| 4 | Deploy i486 (SSE2-free) cnc-ddraw, verified via objdump | **Menu renders** for the first time |
| 5 | User: "open but minimized" → add `noactivateapp=true` | Survives focus loss, no more minimize-to-black |
| 6 | Found `tshack=true` blacks it out → use global section only | Renders correctly, input verified |

## Lessons Learned

1. **A Pentium III has SSE but not SSE2.** Any DLL/EXE built with SSE2 faults
   with `STATUS_ILLEGAL_INSTRUCTION` (0xC000001D) the instant that code runs.
   The kill leaves **no Application-log entry and no app crash dump**, so it
   reads as a silent black-screen hang. This applies to any pre-Pentium-4 fleet
   box (P2/P3/early Athlon).

2. **Capture the exit code before theorizing.**
   `start /wait <exe> & echo !errorlevel!` (with `cmd /v:on`) turned a vague
   "black screen" into a precise `0xC000001D`. One command collapsed hours of
   guesswork.

3. **Prefer i486/i686 / "win2000" build variants for old CPUs**, and verify
   before deploying: `i686-w64-mingw32-objdump -d file.dll | grep -i xmm` should
   be empty. Don't trust a "supports Windows 95" claim to mean "no SSE2."

4. **A black screenshot is not proof of failure.** DirectDraw exclusive-
   fullscreen always captures as black. Check whether the process is *resident*
   (`tasklist`) and whether it left a crash artifact before concluding anything.

5. **`renderer=gdi` is the only cnc-ddraw renderer for DX8-era GPUs** (GeForce4,
   Voodoo, TNT2). OpenGL/Direct3D9 renderers need newer hardware.

6. **Do not enable cnc-ddraw `tshack` on the GeForce4/GDI path** — it renders to
   an undisplayed surface here. Use a plain global `[ddraw]` section.

7. **`noactivateapp=true` is essential when the player chats from the same
   machine.** RA2 + cnc-ddraw minimizes to black on focus loss; every Alt-Tab to
   the Retro Chat window otherwise looks like the game re-broke.

8. **Listen to the user's exact words.** "Open but minimized" was the entire
   diagnosis for Phase 5 — the render pipeline was already fine; the window was
   just hidden.

9. **The driver was never the problem.** 93.71 is the final GeForce4 Ti driver
   for XP; there was nothing to "update." Rule out driver-version as a cause
   early when the box is already at the last supported release.
</content>
