# cs16-noblood — vanilla Counter-Strike 1.6, minus the blood

A server-side mod. Players join with a **completely stock, unmodified CS 1.6
client** — including the non-Steam "BCS 1.6 Romania" build the retro fleet runs —
and simply never see blood. Nothing is downloaded to the client, no client cvar
is touched, and everything else plays exactly like vanilla.

Deployed on **whitebeast (192.168.1.82) UDP 27017**, alongside the untouched
vanilla server on 27016. See [`../README.md`](../README.md) for the host layout.

## How it works

Blood in GoldSrc is not drawn by the server — it is drawn by the *client*, in
response to a temp entity the server broadcasts. Every temp entity arrives as
engine message `SVC_TEMPENTITY` (23), whose first byte is the `TE_*` type. The
plugin hooks that message and drops the three blood types before they leave the
server, so the client is never told to draw blood and there is nothing to render
or decal onto a wall.

| TE id | Name | What it is |
|---|---|---|
| 101 | `TE_BLOODSTREAM` | directional blood spurt |
| 103 | `TE_BLOOD` | Half-Life-style blood stream |
| 115 | `TE_BLOODSPRITE` | CS's hit puff **and** the decal it leaves |

This is deliberately the narrowest possible cut. Bullet holes
(`TE_GUNSHOTDECAL`, 109), sparks, smoke, ricochets, glass and explosions all use
their own `TE_*` types and are untouched — **never blanket-block
`SVC_TEMPENTITY`**, that would delete every visual effect in the game.

### Why not just set the client cvars

`violence_hblood` / `violence_ablood` / `violence_hgibs` / `violence_agibs` are
**client** cvars; a server cannot set them. A plugin could `stuffcmd` them, but
that silently rewrites a player's own settings, persists after they leave, and
is trivially undone. Dropping the temp entity is authoritative and leaves the
client's config alone.

## Layout

```
cs16-noblood/
  plugin/noblood.sma    the source — this is the mod
  dist/noblood.amxx     compiled artifact, committed (the server box has no
                        compiler in its deploy path; same rationale as the
                        tracked retro_agent.exe)
  cfg/server.cfg        the 27017 server config
  README.md
```

## Requirements on the server

The mod needs Metamod + AMX Mod X under a stock HLDS `cstrike` tree:

| Component | Version used | Where |
|---|---|---|
| HLDS | SteamCMD app 90 | `F:\gameservers\cs16-noblood\` |
| Metamod-P | v1.21p109 win32 | `cstrike\addons\metamod\dlls\metamod.dll` |
| AMX Mod X | 1.10.0-git5479 (base + cstrike) | `cstrike\addons\amxmodx\` |

Two wiring steps make it load:

1. `cstrike\liblist.gam` — `gamedll` points at Metamod, not the CS dll:
   ```
   gamedll "addons\metamod\dlls\metamod.dll"
   ```
   **A HLDS `app_update`/validate silently reverts this line** and the mod then
   goes quiet with no error. Re-check it after every update.
2. `cstrike\addons\metamod\plugins.ini`:
   ```
   win32	addons\amxmodx\dlls\amxmodx_mm.dll
   ```

`cstrike\addons\amxmodx\configs\plugins.ini` deliberately lists **only**
`noblood.amxx`. The stock AMXX plugin set (admin, mapchooser, timeleft,
adminchat…) adds commands, vote menus and chat output that would stop this
server reading as vanilla.

## Building

Needs `amxxpc.exe`, which ships inside the AMXX base zip:

```bat
cd <hlds>\cstrike\addons\amxmodx\scripting
copy <repo>\scripts\game-servers\cs16-noblood\plugin\noblood.sma .
amxxpc.exe noblood.sma -o..\plugins\noblood.amxx
```

Then copy the resulting `noblood.amxx` back into `dist/` and commit it.

## Verifying it — without eyeballing a screen

The plugin registers a server command that counts what it has dropped, so you
can prove the hook is live over rcon instead of squinting at a monitor:

```
rcon noblood_version   ->  "noblood_version" is "1.0.0"
rcon amxx plugins      ->  CS 1.6 No Blood  1.0.0  noblood.amx  running
rcon noblood_stats     ->  [noblood] v1.0.0 active - dropped N blood effects ...
```

`noblood_stats` reads 0 until someone actually shoots a player. **A non-zero
counter after a firefight is the proof.** Run the same command against the
vanilla server on 27016 and it does not exist at all — that is the A/B.

Verified on 2026-08-11: Metamod-P `1 plugins, 1 running`; AMXX
`CS 1.6 No Blood 1.0.0 … running`; `noblood_stats` responding.
**Not yet verified visually with a real client** — the retro box carrying the
BCS 1.6 client (.124) was powered off at the time.
