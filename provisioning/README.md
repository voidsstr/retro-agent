# Provisioning helpers

> ## Onboarding was REMOVED in agent v1.71.0
>
> There used to be an `ONBOARD` command here, driven by `onboard.json` ->
> `gen_onboard.py` -> `onboard.cmd` / `onboard_9x.bat`, published with
> `push_onboard.py`. It mapped the share, unzipped a hand-maintained list of
> games, applied the theme, and set `HKLM\Software\RetroAgent\Onboarded`.
>
> **GAMESYNC does all of it, from the staged library, which is the source of
> truth for what a game actually is.** The onboarding list was a second,
> divergent inventory maintained by hand: a title staged properly in
> `Games-Library/` still did not reach a box until somebody remembered to add
> it to `onboard.json`, zip it, and push the control files. GAMESYNC copies the
> installed tree, merges `install.reg`, builds a desktop shortcut per
> `launch.txt` line with the icon the library specifies, stages the wallpapers
> and parks the icons.
>
> **The hardware gating that made onboarding worth having did not go away — it
> got much better.** Onboarding exported four coarse booleans (`ONB_GPU3D`,
> `ONB_CPUFAST`, `ONB_RAM64`, `ONB_RAM128`) derived from
> `GetSystemInfo(wProcessorLevel) >= 6` and a substring search of the adapter
> name, and a batch file gated on them. That could not tell an 845 MHz Pentium
> III from a 3.1 GHz Core i5 (both "family >= 6"), could not see a clock at all,
> could not see video RAM, and treated a Voodoo 2 and a GeForce 8400 GS as the
> same "has 3D" fact.
>
> It is replaced by **`HWPROFILE`** (`agent/src/hwprofile.c`) - CPUID vendor,
> family/model/stepping, real clock, real RAM, instruction-set bits, the ACTIVE
> display adapter's PCI ids and video RAM, OS level, DirectX, and a disc-mounter
> capability - feeding the **capability gate** in `agent/shared/gamegate.h`,
> which decides per title against the library's own `requires.json`. See
> [`scripts/gamegate/README.md`](../scripts/gamegate/README.md) and
> [`scripts/gamegate/SCHEMA.md`](../scripts/gamegate/SCHEMA.md).
>
> Nothing needs migrating on a live box. The `Onboarded` registry value is now
> simply ignored; leaving it set does nothing.

## What is still here

```
provisioning/
  retro_unzip.js    Shell.Application unzip shim - there is no unzip tool on
                    9x/XP, and xcopy HANGS on a NETMAP'd SMB share on XP.
                    STILL IN USE: provisioning/ddk/*.py and the game-install
                    skill both stage it and drive it with cscript. Do not
                    delete it with the onboarding files.
  games/            per-game launcher fragments kept for reference
  ddk/              Windows DDK staging for the driver lanes (see ddk/README.md)
  win98/            Win98-specific provisioning bits
```

## Where the work moved

| was | is now |
|---|---|
| `ONBOARD` agent command | `GAMESYNC START` (and it runs by itself on a fresh image) |
| `onboard.json` game list | the staged library, `Games-Library/<Title>/` |
| `ONB_*` capability flags | `HWPROFILE` + `Games-Library/<Title>/requires.json` |
| `push_onboard.py` | nothing to push; the library IS the payload |
| theme + wallpaper + icons | the agent's `retrowall` thread, every startup |
| `Onboarded` registry flag | `C:\RETRO_AGENT\gamesync.done` |
