---
name: gui-install
description: Install GUI software that needs button-clicking (installer wizards) on one or many retro fleet boxes in parallel, driven in real time via the agent's CLICKSHOT/SCREENDIFF delta protocol. Use for any .exe installer with no silent switch (mod installers, driver setups, game installers).
---

# gui-install — real-time, parallel GUI installer automation

Drives Windows installer wizards on the retro fleet by clicking buttons, watching
the result in real time, and finishing the install (relocate/verify/cleanup). Works
across many boxes **in parallel**. Built for installers with **no reliable silent
switch** (Clickteam/Wise/InstallShield mod & driver setups).

## What makes it real-time (the important part)

The old loop reconnected + auth'd + pulled a 2 MB BMP **per action** — hundreds of ms
of pure overhead each click. This skill removes both costs:

1. **One persistent authenticated connection per box** (`FastUI` holds it open).
2. **Delta frames** — the agent's `SCREENDIFF` / `CLICKSHOT` return only the 64×64
   tiles that changed, reconstructed into a local framebuffer. A click's result is a
   few KB, not a full screenshot.
3. **`CLICKSHOT x y [right|dbl] [settle_ms]`** (agent **v1.18.0+**) — click, settle,
   and return the visual delta in **one round trip**. This is the core primitive:
   `SCREENDIFF FULL` once for a baseline, then `CLICKSHOT` in a tight loop.

Client: `fastui.py` → `FastUI(ip)`; `.baseline()`, `.clickshot(x,y)`, `.refresh()`,
`.winlist()`, `.key(spec)`, `.image()` (PIL). Mechanical helpers: `install_lib.py`.

> If a target is still on agent < v1.18.0, `CLICKSHOT` errors — fall back to
> `UICLICK` + `SCREENDIFF` (two round trips) via `FastUI.refresh()`, or update the
> agent first (it auto-updates to v1.18.0 on restart from the share).

## Workflow

1. **Discover & scope.** Find online targets and their GPU/resolution/`%ProgramFiles%`
   /`SystemDrive` (`scan_fleet`-style VIDEODIAG+SYSINFO). Skip any box that's **actively
   running a benchmark/fullscreen game** — installing adds CPU/IO that skews driver
   work, and a Glide/D3D fullscreen surface makes GDI screenshots garbled/unusable.
2. **Stage the installer.** Single-file `copy /Y` from the share (works even for
   >32 MB, which UPLOAD can't do). `install_lib.stage_from_share()`.
3. **Launch + walk.** `LAUNCH C:\inst.exe`, then read `WINLIST` to anchor the window,
   and drive it with `CLICKSHOT`. Button offsets are stable within a fixed-size
   installer window, but the window **centers per-resolution**, so compute from the
   `WINLIST` rect or confirm each screen with the returned delta frame (the model
   reads it). Match dialogs by **title**, not just class, so you never grab a phantom
   `#32770`.
4. **Wait for completion** by polling the target folder's byte total until stable
   across 2 checks (`install_lib.poll_until_stable()`), not by guessing a sleep.
5. **Relocate + verify + cleanup** (`install_lib.relocate_verified()`): move the
   installed payload to its final home, verify key files + **file-count parity**, and
   only then delete the source. **Never clean up unconditionally.**
6. **Parallel:** run steps 2-5 for each box concurrently (`install_lib.parallel_map`).
   The click-walk (step 3) is LLM-in-the-loop; parallelize the mechanical stages and
   walk boxes that share a resolution with the same coordinates.

## Relocation gotchas (hard-won on the fleet)

- **`%ProgramFiles%` is not always C:.** Dual-boot boxes run XP on **D:**, so an
  installer defaults to `D:\Program Files\...`. Check `echo %ProgramFiles%` and look on
  the **right drive**; a poll on the wrong drive reads "-1"/missing though the install
  succeeded.
- **Same-volume `move` is instant and reliable.** Prefer installing where the final
  home lives so relocation is a same-volume `move`.
- **Cross-volume is painful:** cmd `move` across volumes can throw "Access is denied",
  and **`xcopy` is broken on several fleet XP boxes (RC=0 but copies nothing).** Plain
  `copy` works. `install_lib.tree_copy_via_batch()` recreates the tree with
  `mkdir`+`copy *.*` per subdir and verifies file-count parity before deleting.
- **xcopy of a directory tree over the SMB share HANGS on XP** — never relay a folder
  box→share→box; re-stage the installer per box instead. Single-file `copy` to/from the
  share is fine.
- **Verify before delete, always.** Confirm target key files + `dir /s /b | find /c`
  count equals the source before removing anything.

## Files

- `fastui.py` — persistent-connection real-time client (SCREENDIFF/CLICKSHOT). Also a
  CLI: `python3 fastui.py <ip> shot|click|bench ...`.
- `install_lib.py` — reusable mechanical helpers (stage, poll-until-stable, verified
  relocate, tree-copy-via-batch, parallel_map).
- `recipe_specialists.py` — worked example: The Specialists 3.0 (Clickteam) end-to-end
  (stage → walk → move to `Half-Life\ts` → verify). Copy its shape for other installers.

## Related

- Agent commands live in `agent/src/screen.c` (SCREENDIFF/CLICKSHOT) and
  `agent/src/input.c` (UICLICK/UIKEY/UIDRAG). Bump + publish the agent per CLAUDE.md
  when you change them.
- Half-Life mod specifics: memory `hl-mod-install-method`. Fleet map & share layout:
  `legit-game-library-pipe`.
