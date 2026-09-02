---
name: player-profile
description: Track a player profile - who plays on the fleet, their cross-game preferences, and their per-game configurations (cvars, ini keys, key binds) - and capture those configs off a machine or push them onto any machine. Use when the user says "make a profile for <player>", "save my Quake 3 config", "what are my settings", "put my config on <box>", "copy my binds to the other machines", "set my sensitivity/fov/resolution for <game>", or asks who plays on the fleet.
---

# Player profiles and per-game configurations

A **player profile** is one person: a handle, cross-game preferences, and a
**game profile** per title holding that player's settings and key binds. A
profile is authored once and pushed to any box, so "my Quake 3 config" follows
the player around the fleet instead of living on whichever machine they last
sat at.

Everything is stored by **`scripts/retro_playerprofile.py`** in SQLite at
`~/.retro-fleet/players.db` - the same shape as the fleetbook, and it is the
only interface. Do not hand-edit the DB.

```
players     handle, display name, notes
prefs       cross-game key/value (mouse_dpi, preferred_res, vsync, ...)
profiles    one per (player, game)
settings    the cvars / ini keys of a game profile
binds       key -> action, kept separate from settings
gamedefs    where a game's config file lives on a box, and its format
deployments the audit trail: every capture from / apply to a machine
```

## The normal cycle

**1. Does the player exist?** `list` first - do not create a duplicate under a
different spelling. Handles are case-insensitive and stored lowercase.

```bash
python3 scripts/retro_playerprofile.py list
python3 scripts/retro_playerprofile.py create voidsstr --name "Void" --notes "primary operator"
```

**2. Cross-game preferences** - things that are true of the player, not of one
title. Keep these human, not cvar names; they are the reference you consult when
setting up a NEW game for that player.

```bash
python3 scripts/retro_playerprofile.py set voidsstr mouse_dpi=800 preferred_res=1024x768 vsync=on
```

**3. Per-game settings and binds.**

```bash
python3 scripts/retro_playerprofile.py game-set voidsstr quake3 cg_fov=110 sensitivity=3.5 com_maxfps=125
python3 scripts/retro_playerprofile.py bind voidsstr quake3 MOUSE2 "+zoom"
python3 scripts/retro_playerprofile.py game-show voidsstr quake3
```

**4. Capture what is already on a box** - far better than retyping a config the
player spent years tuning:

```bash
python3 scripts/retro_playerprofile.py capture voidsstr quake3 --host 192.168.1.133
```

**5. Push it to a machine.** Always `--dry-run` first and show the user the
config; `--dry-run` does not touch the box at all.

```bash
python3 scripts/retro_playerprofile.py apply voidsstr quake3 --host 192.168.1.185 --dry-run
python3 scripts/retro_playerprofile.py apply voidsstr quake3 --host 192.168.1.185
```

**6. Log it to the fleetbook**, because it is a change to a machine:

```bash
python3 scripts/retro_fleetbook.py log --host 192.168.1.185 \
  --summary "applied player profile voidsstr/quake3 (12 settings, 6 binds)"
```

## Rules that matter

**Applying overwrites the player's config file on that box.** `apply` archives
the box's existing file into the deployment row first, so it is reversible:

```bash
python3 scripts/retro_playerprofile.py history --host 192.168.1.185
python3 scripts/retro_playerprofile.py restore <deployment-id>
```

Say plainly that you are overwriting a config before you do it. It is not
gated by `retro_command`'s destructive list, so the care has to come from you.

**Do not apply while the game is running.** The engine rewrites its config on
exit and will clobber what you just wrote. `PROCLIST` first; if the game is up,
tell the user to quit it.

**The config path is a default, and dual-boot boxes break it.** `gamedefs`
seeds the usual `C:\...` locations, but .124 keeps XP and its games on **D:**.
Check with `DIRLIST` before assuming, then either pass `--path` for the one
call, or fix it permanently:

```bash
python3 scripts/retro_playerprofile.py games
python3 scripts/retro_playerprofile.py gamedef quake3 --path 'D:\Quake3\baseq3\q3config.cfg'
```

**The cvar command word is per game family and is preserved, not normalized.**
Quake 3 writes `seta cg_fov "110"`; GoldSrc (`cs16`, `halflife`) writes a bare
`rate "25000"`. Emitting the wrong form breaks the config silently. `import` and
`capture` record the original word; only override it with `--cmd` if you are
certain.

**ini games are patched, never rewritten.** UT99/UT2004/Deus Ex inis hold
hundreds of engine keys this tool does not model, so `apply` merges the
profile's keys into the box's own file and leaves the rest alone. That means
the game must already be installed - applying an ini profile to a box with no
ini fails on purpose rather than writing a stub that breaks the game.

**A per-game setting is not a driver setting.** Refresh rate, vsync at the
driver level, and Glide behaviour live in the driver stack, not in a player
profile - see the `voodoo3-driver-dev` / `voodoo5-driver-dev` skills and
fleetbook recipe #5 (Voodoo5 in-game refresh is pinned at 60 Hz regardless of
what a config says). Do not promise a player a refresh rate through this tool.

## Adding a game the tool does not know

`games` lists what is seeded (quake3, quake2, quake, cs16, halflife, ut99,
ut2004, openarena, deusex, seriousssam). For anything else, define it once -
find the real config path on a box with `DIRLIST` rather than guessing:

```bash
python3 scripts/retro_playerprofile.py gamedef thespecialists \
    --path 'C:\Half-Life\ts\userconfig.cfg' --format quake --cmd ''
```

`--format quake` covers any Quake/GoldSrc-family console config;
`--format ini` covers Unreal-engine style section/key files.

## Reporting back over retro chat

Plain ASCII, lead with the outcome. `show <handle>` and `game-show <handle>
<game>` are already formatted for a CRT - relay them close to verbatim rather
than re-prosing them.

## Tests

`tests/python/test_playerprofile.py`. Run `bash tests/run_all.sh` after any
change here; when you fix something in the tool, add the case that would have
caught it.
