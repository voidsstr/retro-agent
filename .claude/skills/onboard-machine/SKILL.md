---
name: onboard-machine
description: Onboard a fleet retro PC (Windows 98 or XP) ON DEMAND over the chat — stage the hardware-appropriate game set + desktop/wallpaper and mark it Onboarded. Onboarding no longer runs automatically at agent startup (it saturated slow boxes), so trigger it deliberately with this skill. Use when the user says to onboard/set up/provision a machine, "run onboarding on <box>", finish setting up a freshly-installed agent, or stage games+wallpaper on a box.
---

# Onboard a machine (on demand, Win98 + XP)

Onboarding maps the file share, stages a **hardware-appropriate** set of games
(games the box can't run are skipped), stages the wallpaper/desktop bundle, and
marks the box `Onboarded`. As of agent **v1.16.0 it is NOT run automatically at
startup** — on old, slow hardware (e.g. a Pentium-1 Compaq Deskpro 2000) the
first-boot share-copy/extract saturated the CPU for minutes and made the agent
look hung. It is now triggered on demand via the **`ONBOARD`** agent command,
which this skill drives. The agent's own boot path stays lightweight.

Companion: the agent binary + `install_agent.bat` are published to the share
per the main build/deploy docs; this skill runs *after* the agent is installed
and reachable.

## Step 1 — Confirm the box is reachable and identify the OS

From chat, `mcp__retro__retro_list_machines` (or connect directly) to confirm
the target is online. Note the OS — onboarding auto-selects the batch dialect:
- **Win98** → `onboard_9x.bat` (COMMAND.COM dialect), run via `command.com /c`
- **NT/XP** → `onboard.cmd` (cmd.exe dialect), run via `cmd /c`
The agent picks the right one; you don't specify it.

> Very slow hardware caveat: a genuine Pentium-1/Win98 box takes a long time to
> even complete the auth handshake, and onboarding will peg it for several
> minutes. Expect sluggish/absent responses *during* onboarding — that's normal
> and is exactly why onboarding is on demand. Don't hammer it with probes.

## Step 2 — Make sure the payload is on the share (one-time per fleet)

The batches + unzip shim must be published to
`\\192.168.1.122\files\Utility\Retro Automation\Onboard\`. If they aren't (or
you changed the game list in `provisioning/onboard.json`), regenerate and push:

```bash
python3 provisioning/gen_onboard.py                 # writes onboard.cmd + onboard_9x.bat
python3 provisioning/push_onboard.py <online-agent-ip>   # publishes both + retro_unzip.js
```

Per-game ZIPs go in the share's `Games\` dir. A game whose ZIP is missing is
reported `[MISS]` and the box is NOT marked Onboarded (it retries next trigger).

## Step 3 — Trigger onboarding over the chat

Send the `ONBOARD` command to the target with the generic command tool:

- Chat / brain: `mcp__retro__retro_command` with `host=<ip>`, `command=ONBOARD`
  (add `command=ONBOARD force` to re-run a box already marked Onboarded).
- Direct: `await conn.command_text("ONBOARD")` via `client/retro_protocol`.

`ONBOARD` returns immediately (`onboarding started`) and does the work in a
background thread on the box, so it won't tie up the connection. `ONBOARD` is
not a gated/destructive verb, so the brain can issue it without `confirm=true`.

## Step 4 — What it does (hardware gating)

The agent detects the box's hardware and sets `ONB_*` flags the batch uses to
gate each game (games it can't run are `[HWSKIP]`'d, which does NOT block
completion). Flags: `gpu3d` (a 3D adapter — 3dfx/NVIDIA/ATI/Intel), `cpufast`
(CPU family ≥ 6, i.e. Pentium Pro/II/III/4+, not a plain Pentium 1), `ram64`,
`ram128`. Outcomes:
- **Pentium-1 + 2D video (Deskpro 2000)** → **no games** (all HWSKIP), just the
  wallpaper/desktop. Correct — none of the core games run on that hardware.
- **Win98/XP + Voodoo/3D + PII+** → the full set.
- **2D-only PII+** → CPU games, skips the 3D-only ones (UT, Quake III).

Wallpaper is staged from the share and the agent's retrowall thread applies the
theme on every boot. On success it writes `Onboarded=1` and stops re-running.

## Step 5 — Watch progress / verify

Onboarding logs to `C:\RETRO_AGENT\onboard.log` on the box. Once the box isn't
saturated, check it (`retro_command` → `DOWNLOAD C:\RETRO_AGENT\onboard.log`, or
`type` it). Look for `[ok]`/`[skip]`/`[HWSKIP]`/`[MISS]` per game and
`ONBOARDING COMPLETE`. Confirm the marker with
`REGREAD HKLM Software\RetroAgent` (`Onboarded` = 1). The agent's own
`agent.log` (mirrored to the share's `agent logs\<host>-agent.log`) shows the
`ONBOARD command received` + capability detection line.

## Notes

- Idempotent: re-triggering skips already-installed games and re-stages
  wallpaper; use `ONBOARD force` to re-run a box already marked Onboarded.
- If a box shows `[MISS]` for games, the ZIPs aren't on the share yet — stage
  them and re-trigger; it won't mark Onboarded until nothing is missing.
- This replaces the old auto-on-first-boot behavior for BOTH Win98 and XP.
