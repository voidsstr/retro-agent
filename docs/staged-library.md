# Staged library — what is staged, where it went, and whether it was tested

**GENERATED — do not edit by hand.** Regenerate with
`python3 scripts/fleet/gen-staged-library.py`; `--check` fails if it is stale.

A hand-written version of this was never going to survive: the library went
38 → 54 titles in a single session, two graphics cards were swapped mid-session,
and the machines are powered on and off continuously. The same argument settled
`docs/fleet-inventory.md`, whose hand-maintained predecessor was wrong about most
of the fleet.

Source of truth is `~/.retro-fleet/fleetbook.db`. Query it directly with
`scripts/fleet/compat.py` (`matrix`, `status --box .143`, `gaps`, `summary`).

Generated 2026-09-01 01:33.

## The machines

| box | host | CPU | RAM | GPU | OS |
|---|---|---|---|---|---|
| `192.168.1.123` | NSC-B20C188E96D | 2403 MHz | 2047 MB | ATI Radeon HD 3850 AGP (512 MB) | Windows XP |
| `192.168.1.124` | NSC-CABE14B7486 | 845 MHz | 511 MB | NVIDIA GeForce2 GTS/GeForce2 Pro ( | Windows XP |
| `192.168.1.133` | P3-DUAL | 701 MHz | 255 MB | NVIDIA GeForce4 Ti 4600 (128 MB) | Windows XP |
| `192.168.1.143` | 1GHZ | 1000 MHz | 511 MB | NVIDIA GeForce 6800 (128 MB) | Windows XP |
| `192.168.1.145` | DELL | 3093 MHz | 3316 MB | NVIDIA GeForce 8400GS (512 MB) | Windows XP |
| `192.168.1.171` | NSC-5B996B81319 | 2793 MHz | 509 MB | Intel(R) 82865G Graphics Controlle | Windows XP |
| `192.168.1.240` | USER-41EA3B3330 | 2403 MHz | 1022 MB | RADEON X800 Series (256 MB) | Windows XP |
| `192.168.1.243` | N5R5L9 | 165 MHz | 127 MB | Cirrus Logic 5436 PCI (0 MB) | Windows 98 |
| `192.168.1.246` | ADMIN-PC | 3093 MHz | 3317 MB | AMD Radeon HD 5450 (512 MB) | Windows 7 |

## Deployment and test state

Each cell is **deploy / runs**:

| | deploy | | runs |
|---|---|---|---|
| `+` | deployed | `V` | **verified** — seen rendering fullscreen, screenshot kept |
| `G` | gated — the box cannot run it | `r` | starts; rendering not characterised |
| `s` | skipped — did not fit on the disk | `X` | failed |
| `~` | marginal (allowed) | `.` | **untested — nobody has looked** |
| `-` | absent | `-` | not applicable |

**`gated` and `skipped` are different facts.** The first means the hardware
cannot run it and carries the limiting number; the second means there was no
room. Conflating them once told an operator a Pentium 1 "cannot run" a game it
merely had no space for.

| title | `.123` | `.124` | `.133` | `.143` | `.145` | `.171` | `.240` | `.243` | `.246` | verified |
|---|---|---|---|---|---|---|---|---|---|---|
| AliensVsPredator | +r | +r | +r | +r | +. | +r | +r | G. | +r | 0 |
| BF1942 | +- | +X | +X | +X | +. | +. | +X | G. | +X | 0 |
| Carmageddon1 | +V | +V | +V | +V | +. | +V | +V | G. | +V | 7 |
| Carmageddon2 | +V | +V | +V | +V | +. | +r | +V | G. | +V | 6 |
| CounterStrike16 | +V | +V | +V | +V | +. | +V | +V | G. | +r | 6 |
| Descent1 | +V | +V | +V | +V | +. | +V | +V | +. | +V | 7 |
| Descent2 | +r | +V | +V | +V | +. | +V | +V | G. | +r | 5 |
| Descent3 | +V | +V | +V | +V | +. | +V | +V | G. | +V | 7 |
| DeusEx | +V | +r | +V | +V | +. | +r | +r | G. | +V | 4 |
| Doom3 | +V | G. | G. | G. | .. | G. | +V | G. | +r | 2 |
| FarCry | +V | G. | G. | .V | .. | G. | +V | G. | +V | 4 |
| Generals | .. | .. | ~. | .. | .. | ~. | .. | .. | .. | 0 |
| HalfLife-BlueShift | +X | +X | +X | +X | +. | +. | +X | G. | +r | 0 |
| HalfLife-DMC | +V | +V | +V | +V | +. | +r | +V | G. | +r | 5 |
| HalfLife-Deathmatch | +V | +r | +V | +V | +. | +r | +V | G. | +r | 4 |
| HalfLife-OpposingForce | +X | +X | +X | +X | +. | +. | +X | G. | +X | 0 |
| HalfLife-TFC | +V | +V | +V | +V | +. | +r | +V | G. | +r | 5 |
| HalfLife1 | +V | +V | +V | +V | +. | +V | +V | G. | +r | 6 |
| Halo | +r | G. | G. | G. | .. | G- | +V | G. | +V | 2 |
| HexenII | +r | +V | +V | +V | +. | +V | +V | +. | +r | 5 |
| HiddenAndDangerous | +V | +V | +V | +V | +. | +V | +V | G. | +V | 7 |
| JediAcademy | +- | +V | +V | +V | +. | +. | +V | G. | +V | 5 |
| JediKnightDF2 | +r | +r | +V | +r | +. | +r | +r | G. | +V | 2 |
| JediKnightMotS | +V | +r | +V | +V | +. | +V | +r | ~. | +V | 5 |
| MasterOfOrionII | +r | .V | +V | .V | .. | .V | +r | .. | +r | 4 |
| MaxPayne | +- | .V | +V | .V | .. | ~V | +X | G. | +V | 5 |
| Quake1 | +V | +V | +V | +V | +. | +V | +V | +. | +V | 7 |
| Quake2Complete | +V | +V | +V | +V | +. | +V | +V | G. | +V | 7 |
| Quake3-TeamArena | +V | +V | +V | +V | +. | +V | +V | G. | +V | 7 |
| Quake3Arena | +V | +V | +V | +V | +. | +. | +V | G. | +V | 6 |
| RedAlert2 | +V | +r | +V | +V | +. | +V | +V | G. | +V | 6 |
| RedFaction | +- | +r | +V | +V | +. | +V | +X | G. | +V | 4 |
| RedneckRampage | +V | +V | +V | +r | +. | +V | +r | G. | +r | 4 |
| ReturnToCastleWolfenstein | +V | +V | +V | +V | .. | +V | +V | G. | +V | 7 |
| SeriousSamFirstEncounter | +- | +V | +V | +V | .. | +V | +V | G. | +X | 5 |
| SeriousSamSecondEncounter | +- | +V | +V | +V | .. | +. | +V | G. | +r | 4 |
| ShadowWarrior | +V | .r | +V | .V | .. | .V | +r | .. | +V | 5 |
| Shogo | +V | +V | +V | +V | +. | +V | +r | G. | +V | 6 |
| SiNGold | +V | +V | +V | +V | +. | +V | +V | G. | +r | 6 |
| SoldierOfFortune | +V | +V | +V | +V | +. | +. | +r | G. | +r | 4 |
| SoldierOfFortune2 | +V | +V | +V | +V | +. | +V | +V | G. | +r | 6 |
| StarCraft | +- | +r | +V | +V | +. | +r | +V | +. | +V | 4 |
| SystemShock2 | +- | +V | +V | +V | +. | +V | +V | G. | +V | 6 |
| Thief2 | +V | +V | +V | +V | +. | +V | +V | G. | +V | 7 |
| ThiefGold | +V | +V | +V | +V | +. | +V | +V | G. | +V | 7 |
| TiberianSun | +V | +r | +V | +V | +. | +V | +r | G. | +V | 5 |
| Turok2 | +V | .V | +V | .V | .. | .V | +V | G. | +V | 7 |
| UT2004 | +V | +V | +V | +V | +. | +V | +V | G. | +V | 7 |
| UnrealGold | +V | +V | +V | +X | +. | +V | +V | G. | +V | 6 |
| UnrealTournament | +V | +X | +X | +X | +. | +V | +V | G. | +V | 4 |
| UnrealTournament436 | +V | +V | +V | +V | +. | +V | +r | G. | +V | 6 |
| WarcraftII | +r | .r | +V | .V | .. | .r | +r | G. | +r | 2 |
| WarcraftOrcsAndHumans | +V | .V | +V | .X | .. | .V | +V | G. | +V | 6 |
| YurisRevenge | +V | +V | +V | +V | +. | +. | +V | G. | +V | 6 |

**54 titles × 9 machines = 486 cells — 260 verified, 133 untested.**

## Titles with a blocker recorded

| title | box | blocker |
|---|---|---|
| AliensVsPredator | `192.168.1.123` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.123` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.123` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.123` | tunnel proven both ends; the front end ignores click *and* key |
| FarCry | `192.168.1.123` | server hosts unattended; CryEngine takes DirectInput exclusively |
| HalfLife-BlueShift | `192.168.1.123` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| Halo | `192.168.1.123` | **JOINING is automated; HOSTING is not.** `halo.exe -connect <ip>:<port>` skips the menu, so a c |
| HiddenAndDangerous | `192.168.1.123` | launcher bug fixed; stops at profile creation |
| JediAcademy | `192.168.1.123` | the box has NO disc mounter and NO optical drive at all (HWPROFILE disc_mount=false, wmic logica |
| MasterOfOrionII | `192.168.1.123` | that menu entry is mouse-only |
| MaxPayne | `192.168.1.123` | `MaxPayne.exe` imports no `WS2_32`, `WSOCK32` or `DPLAYX` at all. |
| RedFaction | `192.168.1.123` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.123` | IPX tunnel proven; the Build gather never happens |
| ShadowWarrior | `192.168.1.123` | the in-game gather |
| Shogo | `192.168.1.123` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.123` | multiplayer refused **even with the disc** — see below |
| StarCraft | `192.168.1.123` | cannot start on .123 at all: disc-image title, box has no optical drive and no mounter, shortcut |
| SystemShock2 | `192.168.1.123` | the menu offers New Game / Load / Options / Credits / |
| Turok2 | `192.168.1.123` | host works and is listed in the joiner's browser; join fails |
| WarcraftII | `192.168.1.123` | its 8-bit DirectDraw surface is **uncapturable by GDI on both XP and Win7**, so the agent cannot |
| WarcraftOrcsAndHumans | `192.168.1.123` | campaign/network screen is mouse-only |
| AliensVsPredator | `192.168.1.124` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.124` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.124` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.124` | tunnel proven both ends; the front end ignores click *and* key |
| HalfLife-BlueShift | `192.168.1.124` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| HalfLife-OpposingForce | `192.168.1.124` | gearbox\dlls\opfor.dll against the staged WON engine - the engine and console start, then ANY ma |
| HiddenAndDangerous | `192.168.1.124` | launcher bug fixed; stops at profile creation |
| JediAcademy | `192.168.1.124` | NOT the disc - the image and launcher are staged and proven on .143 and .246. This box's DAEMON  |
| MaxPayne | `192.168.1.124` | `MaxPayne.exe` imports no `WS2_32`, `WSOCK32` or `DPLAYX` at all. |
| RedFaction | `192.168.1.124` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.124` | IPX tunnel proven; the Build gather never happens |
| Shogo | `192.168.1.124` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.124` | multiplayer refused **even with the disc** — see below |
| SystemShock2 | `192.168.1.124` | the menu offers New Game / Load / Options / Credits / |
| AliensVsPredator | `192.168.1.133` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.133` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.133` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.133` | tunnel proven both ends; the front end ignores click *and* key |
| HalfLife-BlueShift | `192.168.1.133` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| HiddenAndDangerous | `192.168.1.133` | launcher bug fixed; stops at profile creation |
| JediAcademy | `192.168.1.133` | disc image and launcher staged; this box has a DAEMON Tools unit but the title was not exercised |
| MasterOfOrionII | `192.168.1.133` | that menu entry is mouse-only |
| MaxPayne | `192.168.1.133` | `MaxPayne.exe` imports no `WS2_32`, `WSOCK32` or `DPLAYX` at all. |
| RedFaction | `192.168.1.133` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.133` | IPX tunnel proven; the Build gather never happens |
| ShadowWarrior | `192.168.1.133` | the in-game gather |
| Shogo | `192.168.1.133` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.133` | multiplayer refused **even with the disc** — see below |
| SystemShock2 | `192.168.1.133` | the menu offers New Game / Load / Options / Credits / |
| Turok2 | `192.168.1.133` | host works and is listed in the joiner's browser; join fails |
| WarcraftII | `192.168.1.133` | its 8-bit DirectDraw surface is **uncapturable by GDI on both XP and Win7**, so the agent cannot |
| WarcraftOrcsAndHumans | `192.168.1.133` | campaign/network screen is mouse-only |
| AliensVsPredator | `192.168.1.143` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.143` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.143` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.143` | tunnel proven both ends; the front end ignores click *and* key |
| HalfLife-BlueShift | `192.168.1.143` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| HiddenAndDangerous | `192.168.1.143` | launcher bug fixed; stops at profile creation |
| MaxPayne | `192.168.1.143` | `MaxPayne.exe` imports no `WS2_32`, `WSOCK32` or `DPLAYX` at all. |
| RedFaction | `192.168.1.143` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.143` | IPX tunnel proven; the Build gather never happens |
| Shogo | `192.168.1.143` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.143` | multiplayer refused **even with the disc** — see below |
| SystemShock2 | `192.168.1.143` | the menu offers New Game / Load / Options / Credits / |
| Turok2 | `192.168.1.143` | host works and is listed in the joiner's browser; join fails |
| AliensVsPredator | `192.168.1.145` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.145` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.145` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.145` | tunnel proven both ends; the front end ignores click *and* key |
| HalfLife-BlueShift | `192.168.1.145` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| HiddenAndDangerous | `192.168.1.145` | launcher bug fixed; stops at profile creation |
| JediAcademy | `192.168.1.145` | box offline all session (its cable is in the Win98 box) - untested, not failed |
| RedFaction | `192.168.1.145` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.145` | IPX tunnel proven; the Build gather never happens |
| Shogo | `192.168.1.145` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.145` | multiplayer refused **even with the disc** — see below |
| SystemShock2 | `192.168.1.145` | the menu offers New Game / Load / Options / Credits / |
| AliensVsPredator | `192.168.1.171` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.171` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.171` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.171` | tunnel proven both ends; the front end ignores click *and* key |
| HalfLife-BlueShift | `192.168.1.171` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| HiddenAndDangerous | `192.168.1.171` | launcher bug fixed; stops at profile creation |
| JediAcademy | `192.168.1.171` | box was offline for the whole session - untested, not failed |
| MaxPayne | `192.168.1.171` | `MaxPayne.exe` imports no `WS2_32`, `WSOCK32` or `DPLAYX` at all. |
| RedFaction | `192.168.1.171` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.171` | IPX tunnel proven; the Build gather never happens |
| Shogo | `192.168.1.171` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.171` | multiplayer refused **even with the disc** — see below |
| SystemShock2 | `192.168.1.171` | the menu offers New Game / Load / Options / Credits / |
| AliensVsPredator | `192.168.1.240` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.240` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.240` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.240` | tunnel proven both ends; the front end ignores click *and* key |
| FarCry | `192.168.1.240` | server hosts unattended; CryEngine takes DirectInput exclusively |
| HalfLife-BlueShift | `192.168.1.240` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| Halo | `192.168.1.240` | **JOINING is automated; HOSTING is not.** `halo.exe -connect <ip>:<port>` skips the menu, so a c |
| HiddenAndDangerous | `192.168.1.240` | launcher bug fixed; stops at profile creation |
| JediAcademy | `192.168.1.240` | NOT the disc - the image and launcher are staged and proven on .143 and .246. This box's DAEMON  |
| MasterOfOrionII | `192.168.1.240` | that menu entry is mouse-only |
| MaxPayne | `192.168.1.240` | `MaxPayne.exe` imports no `WS2_32`, `WSOCK32` or `DPLAYX` at all. |
| RedFaction | `192.168.1.240` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.240` | IPX tunnel proven; the Build gather never happens |
| ShadowWarrior | `192.168.1.240` | the in-game gather |
| Shogo | `192.168.1.240` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.240` | multiplayer refused **even with the disc** — see below |
| SystemShock2 | `192.168.1.240` | the menu offers New Game / Load / Options / Credits / |
| Turok2 | `192.168.1.240` | host works and is listed in the joiner's browser; join fails |
| WarcraftII | `192.168.1.240` | its 8-bit DirectDraw surface is **uncapturable by GDI on both XP and Win7**, so the agent cannot |
| WarcraftOrcsAndHumans | `192.168.1.240` | campaign/network screen is mouse-only |
| AliensVsPredator | `192.168.1.246` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.246` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.246` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.246` | tunnel proven both ends; the front end ignores click *and* key |
| CounterStrike16 | `192.168.1.246` | FULLSCREEN stalls the join: with -full the client logs 'Connection accepted by <server>' and nev |
| FarCry | `192.168.1.246` | server hosts unattended; CryEngine takes DirectInput exclusively |
| HalfLife-BlueShift | `192.168.1.246` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| HalfLife1 | `192.168.1.246` | The HalfLife1 tree's engine is WON hl.exe 1.1.0.8 = network protocol 45, and the fleet Half-Life |
| Halo | `192.168.1.246` | **JOINING is automated; HOSTING is not.** `halo.exe -connect <ip>:<port>` skips the menu, so a c |
| HiddenAndDangerous | `192.168.1.246` | launcher bug fixed; stops at profile creation |
| MasterOfOrionII | `192.168.1.246` | that menu entry is mouse-only |
| MaxPayne | `192.168.1.246` | `MaxPayne.exe` imports no `WS2_32`, `WSOCK32` or `DPLAYX` at all. |
| RedFaction | `192.168.1.246` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.246` | IPX tunnel proven; the Build gather never happens |
| ShadowWarrior | `192.168.1.246` | the in-game gather |
| Shogo | `192.168.1.246` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.246` | multiplayer refused **even with the disc** — see below |
| SystemShock2 | `192.168.1.246` | the menu offers New Game / Load / Options / Credits / |
| Turok2 | `192.168.1.246` | host works and is listed in the joiner's browser; join fails |
| WarcraftII | `192.168.1.246` | its 8-bit DirectDraw surface is **uncapturable by GDI on both XP and Win7**, so the agent cannot |
| WarcraftOrcsAndHumans | `192.168.1.246` | campaign/network screen is mouse-only |

