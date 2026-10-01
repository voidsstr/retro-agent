# Staged library — what is staged, where it went, and whether it was tested

**GENERATED — do not edit by hand.** Regenerate with
`python3 scripts/fleet/gen-staged-library.py`; `--check` fails if it is stale.

A hand-written version of this was never going to survive: the library went
38 → 124 titles in a single session, two graphics cards were swapped mid-session,
and the machines are powered on and off continuously. The same argument settled
`docs/fleet-inventory.md`, whose hand-maintained predecessor was wrong about most
of the fleet.

Source of truth is `~/.retro-fleet/fleetbook.db`. Query it directly with
`scripts/fleet/compat.py` (`matrix`, `status --box .143`, `gaps`, `summary`).

Generated 2026-09-30 21:13.

## The machines

| box | host | CPU | RAM | GPU | OS |
|---|---|---|---|---|---|
| `192.168.1.110` | NSC-5C5396FAF9D | 2793 MHz | 1150 MB | RADEON 9500 PRO / 9700 (128 MB) | Windows XP |
| `192.168.1.123` | NSC-B20C188E96D | 2403 MHz | 2047 MB | ATI Radeon HD 3850 AGP (512 MB) | Windows XP |
| `192.168.1.124` | NSC-C543575F526 | 2004 MHz | 255 MB | vcr-kmd Voodoo 5 6000 (open driver | Windows XP |
| `192.168.1.133` | P3-DUAL | 701 MHz | 255 MB | NVIDIA GeForce4 Ti 4600 (128 MB) | Windows XP |
| `192.168.1.143` | 1GHZ | 1000 MHz | 511 MB | NVIDIA GeForce 6800 (128 MB) | Windows XP |
| `192.168.1.145` | DELL | 3092 MHz | 3316 MB | NVIDIA GeForce 8400GS (512 MB) | Windows XP |
| `192.168.1.171` | NSC-5B996B81319 | 2793 MHz | 509 MB | NVIDIA GeForce FX 5500 (256 MB) | Windows XP |
| `192.168.1.184` | NSC-CABE14B7486 | 845 MHz | 511 MB | NVIDIA GeForce2 GTS/GeForce2 Pro ( | Windows XP |
| `192.168.1.185` | NSC-AB862B3CF23 | 1152 MHz | 511 MB | 3dfx Voodoo5 (32 MB) | Windows XP |
| `192.168.1.186` | NSC-6FE8BDE7351 | 2621 MHz | 3071 MB | NVIDIA GeForce 9400 GT  (1024 MB) | Windows XP |
| `192.168.1.191` | NSC-AF6CF7A80BC | 1152 MHz | 511 MB | AMIGAMERLIN 3.1-R11 For Voodoo 5 6 | Windows XP |
| `192.168.1.195` | ADMIN-PC | ? MHz | ? MB | ? | ? |
| `192.168.1.197` | ADMIN-PC | 3093 MHz | 3317 MB | AMD Radeon HD 5450 (512 MB) | Windows 7 |
| `192.168.1.240` | USER-41EA3B3330 | 2403 MHz | 1022 MB | Radeon X1600 Series (256 MB) | Windows XP |
| `192.168.1.243` | N5R5L9 | 165 MHz | 127 MB | Cirrus Logic 5436 PCI (0 MB) | Windows 98 |
| `192.168.1.246` | ADMIN-PC | 3093 MHz | 3317 MB | AMD Radeon HD 5450 (512 MB) | Windows 7 |
| `192.168.1.249` | WHITEBEAST | 4292 MHz | 65097 MB | NVIDIA GeForce RTX 4080 SUPER (409 | Windows 8 |

## Deployment and test state

Each cell is **deploy / runs**:

| | deploy | | runs |
|---|---|---|---|
| `+` | deployed | `V` | **verified** — seen rendering fullscreen, screenshot kept |
| `G` | gated — the box cannot run it | `r` | starts; rendering not characterised |
| `s` | skipped — did not fit on the disk | `X` | failed |
| `~` | marginal (allowed) | `.` | **untested — nobody has looked** |
| `-` | absent | `-` | not applicable |
| `.` | **nobody has looked** — no deploy record for this box at all | | |

A row that is `..` all the way across is a title that IS in the library and
has never been deployed to anything — staged, but nowhere yet. That is a
different fact from `-` (absent: we looked, it is not there) and from `G`
(gated: the hardware cannot run it), and it is what a title staged while the
fleet was powered down looks like.

**`gated` and `skipped` are different facts.** The first means the hardware
cannot run it and carries the limiting number; the second means there was no
room. Conflating them once told an operator a Pentium 1 "cannot run" a game it
merely had no space for.

| title | `.110` | `.123` | `.124` | `.133` | `.143` | `.145` | `.171` | `.184` | `.185` | `.186` | `.191` | `.195` | `.197` | `.240` | `.243` | `.246` | `.249` | verified |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| AliensVsPredator | +. | +r | +r | +r | +r | +. | +r | +. | +. | +. | +. | +. | +. | +r | G. | +r | +. | 0 |
| BF1942 | +. | +- | +X | +X | +X | +. | +. | +. | +. | +. | +. | +. | +. | +X | G. | +X | +. | 0 |
| Carmageddon1 | +. | +V | +V | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +V | G. | +V | +. | 7 |
| Carmageddon2 | +. | +V | +V | +V | +V | +. | +r | +. | +. | +. | +. | +. | +. | +V | G. | +V | +. | 6 |
| CounterStrike16 | +. | +V | +V | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +V | G. | +r | +. | 6 |
| DOS-AloneInTheDark | .. | G. | G. | .. | -. | -. | -. | .. | .. | .. | .. | G. | -. | -. | +. | .. | -. | 0 |
| DOS-BlakeStoneAOG | .. | G. | G. | .. | -. | -. | -. | .. | .. | .. | .. | G. | -. | -. | +. | .. | -. | 0 |
| DOS-BlakeStonePS | .. | G. | G. | .. | -. | -. | -. | .. | .. | .. | .. | G. | -. | -. | +. | .. | -. | 0 |
| DOS-DestructionDerby2 | .. | G. | G. | .. | -. | -. | -. | .. | .. | .. | .. | G. | -. | -. | +. | .. | -. | 0 |
| DOS-Doom | .. | G. | G. | .. | -. | -. | -. | .. | .. | .. | .. | G. | -. | -. | +. | .. | -. | 0 |
| DOS-Duke3D | .. | G. | G. | .. | -. | -. | -. | .. | .. | .. | .. | G. | -. | -. | +. | .. | -. | 0 |
| DOS-GTA | .. | G. | G. | .. | -. | -. | -. | .. | .. | .. | .. | G. | -. | -. | +. | .. | -. | 0 |
| DOS-Heretic | .. | G. | G. | .. | -. | -. | -. | .. | .. | .. | .. | G. | -. | -. | +. | .. | -. | 0 |
| DOS-Hexen | .. | G. | G. | .. | -. | -. | -. | .. | .. | .. | .. | G. | -. | -. | +. | .. | -. | 0 |
| DOS-Powerslave | .. | G. | G. | .. | -. | -. | -. | .. | .. | .. | .. | G. | -. | -. | +. | .. | -. | 0 |
| DOS-ROTT | .. | G. | G. | .. | -. | -. | -. | .. | .. | .. | .. | G. | -. | -. | +. | .. | -. | 0 |
| DOS-Screamer2 | .. | G. | G. | .. | -. | -. | -. | .. | .. | .. | .. | G. | -. | -. | G. | .. | -. | 0 |
| DOS-SpearMissionPacks | .. | G. | G. | .. | -. | -. | -. | .. | .. | .. | .. | G. | -. | -. | +. | .. | -. | 0 |
| DOS-SpearOfDestiny | .. | G. | G. | .. | -. | -. | -. | .. | .. | .. | .. | G. | -. | -. | +. | .. | -. | 0 |
| DOS-Wolf3D | .. | G. | G. | .. | -. | -. | -. | .. | .. | .. | .. | G. | -. | -. | +. | .. | -. | 0 |
| DOS-WreckinCrew | .. | .. | G. | .. | .. | .. | .. | .. | .. | .. | .. | G. | .. | .. | .. | .. | .. | 0 |
| Daggerfall | .. | +. | +. | .V | .V | .. | .. | -. | .. | .. | .. | .. | +. | .. | G. | -. | -. | 2 |
| Descent1 | +. | +V | +V | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +V | +. | +V | -. | 7 |
| Descent2 | +. | +r | +V | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +V | G. | +r | -. | 5 |
| Descent3 | +. | +V | +V | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +V | G. | +V | -. | 7 |
| DeusEx | +. | +V | +r | +V | +V | +. | +r | +. | +. | +. | +. | +. | +. | +r | G. | +V | -. | 4 |
| Doom3 | .. | +V | G. | G. | G. | .. | G. | -. | .. | .. | .. | .. | +. | GV | G. | .r | -. | 2 |
| FarCry | .. | +V | G. | G. | .V | .. | G. | -. | .. | .. | .. | .. | +. | GV | G. | .V | -. | 4 |
| Flight-A10TankKiller | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-A320Airbus | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-AcesOverEurope | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-AcesPacific | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-B17 | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-Battlehawks1942 | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-BlueAngels | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | G. | .. | -. | 0 |
| Flight-BlueMax | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-ChuckYeager | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-Comanche2 | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-EF2000 | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-Epic | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-F117A | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-F14 | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-F15SE3 | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-F19 | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-Falcon3 | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-FighterDuel | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-FlightCD | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-FrontierElite2 | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-Gunship2000 | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-JanesATF | .. | -. | -. | .. | .. | .. | .. | .. | .. | .. | .. | .. | -. | .. | +. | .. | -. | 0 |
| Flight-LHX | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-Longbow | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-MSFS50 | .. | -. | -. | .. | .. | .. | .. | .. | .. | .. | .. | .. | -. | .. | +. | .. | -. | 0 |
| Flight-MSFS51 | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | G. | .. | -. | 0 |
| Flight-Overlord | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-PacificAirWar1942 | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-PacificStrike | .. | -. | -. | .. | .. | .. | .. | .. | .. | .. | .. | .. | -. | .. | +. | .. | -. | 0 |
| Flight-Privateer | .. | -. | -. | .. | .. | .. | .. | .. | .. | .. | .. | .. | -. | .. | +. | .. | -. | 0 |
| Flight-RedBaron | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-Retribution | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | G. | .. | -. | 0 |
| Flight-RighteousFire | .. | -. | -. | .. | .. | .. | .. | .. | .. | .. | .. | .. | -. | .. | +. | .. | -. | 0 |
| Flight-SWOTL | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-StuntIsland | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-Su27Flanker | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-TerminalVelocity | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-TheirFinestHour | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-TieFighter | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-Tornado | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-USNF | .. | -. | -. | .. | .. | .. | .. | .. | .. | .. | .. | .. | -. | .. | +. | .. | -. | 0 |
| Flight-Werewolf | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-WingCommander1 | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-WingCommander2 | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Flight-XWing | .. | G. | G. | .. | -. | G. | -. | .. | .. | .. | .. | G. | -. | G. | +. | .. | -. | 0 |
| Generals | .. | .. | .. | ~. | .. | .. | ~. | .. | .. | .. | .. | .. | .. | .. | .. | .. | .. | 0 |
| HalfLife-BlueShift | +. | +X | +X | +X | +X | +. | +. | +. | +. | +. | +. | +. | +. | +X | G. | +r | -. | 0 |
| HalfLife-DMC | +. | +V | +V | +V | +V | +. | +r | +. | +. | +. | +. | +. | +. | +V | G. | +r | -. | 5 |
| HalfLife-Deathmatch | +. | +V | +r | +V | +V | +. | +r | +. | +. | +. | +. | +. | +. | +V | G. | +r | -. | 4 |
| HalfLife-OpposingForce | +. | +X | +X | +X | +X | +. | +. | +. | +. | +. | +. | +. | +. | +X | G. | +X | -. | 0 |
| HalfLife-TFC | +. | +V | +V | +V | +V | +. | +r | +. | +. | +. | +. | +. | +. | +V | G. | +r | -. | 5 |
| HalfLife1 | +. | +V | +V | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +V | G. | +r | -. | 6 |
| Halo | .. | +r | +. | G- | G- | .. | G- | -. | .. | .. | .. | .. | +. | GV | G. | .V | -. | 2 |
| Halo2 | .. | +X | G. | G. | G. | .. | G. | -. | .. | .. | .. | .. | +. | G. | G. | .V | -. | 1 |
| HexenII | +. | +r | +V | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +V | +V | +r | -. | 6 |
| HiddenAndDangerous | +. | +V | +V | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +V | G. | +V | -. | 7 |
| JediAcademy | +. | +- | +V | +V | +V | +. | +. | +. | +. | +. | +. | +. | +. | +V | G. | +V | -. | 5 |
| JediKnightDF2 | +. | +r | +r | +V | +r | +. | +r | +. | +. | +. | +. | +. | +. | +r | G. | +V | -. | 2 |
| JediKnightMotS | +. | +V | +r | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +r | G. | +V | -. | 5 |
| MasterOfOrionII | .. | +r | +V | .V | .V | .. | .V | +. | .. | .. | .. | .. | +. | .r | G. | .r | -. | 4 |
| MaxPayne | .. | +- | +V | .V | .V | .. | ~V | +. | .. | .. | .. | .. | +. | .X | G. | .V | -. | 5 |
| Postal | .. | +. | G. | G. | G. | .. | .. | -. | .. | .. | .. | .. | +. | .. | G. | -. | -. | 0 |
| Quake1 | +. | +V | +V | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +V | +V | +V | -. | 8 |
| Quake2Complete | +. | +V | +V | +V | +V | +V | +V | +. | +. | +. | +. | +. | +. | +V | G. | +V | -. | 8 |
| Quake2Win9x | .. | G. | G. | G. | G. | G. | G. | -. | .. | .. | .. | G. | -. | G. | +V | .. | -. | 1 |
| Quake3-TeamArena | +. | +V | +V | +V | +V | +V | +V | +. | +. | +. | +. | +. | +. | +V | G. | +V | -. | 8 |
| Quake3Arena | +. | +V | +V | +V | +V | +V | +. | +. | +. | +. | +. | +. | +. | +V | G. | +V | -. | 7 |
| RainbowSix | .. | +V | +. | .. | .. | .. | .. | -. | .. | .. | .. | .. | +. | .. | G. | +. | -. | 1 |
| RedAlert2 | +. | +V | +r | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +V | G. | +V | -. | 6 |
| RedFaction | +. | +- | +r | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +X | G. | +V | -. | 4 |
| RedneckRampage | +. | +V | +V | +V | +r | +. | +V | +. | +. | +. | +. | +. | +. | +r | G. | +r | -. | 4 |
| ReturnToCastleWolfenstein | +. | +V | +V | +V | +V | +V | +V | +. | +. | +. | +. | +. | +. | +V | G. | +V | -. | 8 |
| SeriousSamFirstEncounter | +. | +- | +V | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +V | G. | +X | -. | 5 |
| SeriousSamSecondEncounter | +. | +- | +V | +V | +V | +. | +. | +. | +. | +. | +. | +. | +. | +V | G. | +r | -. | 4 |
| SeriousSamTFE | .. | .. | .. | .. | .. | .. | .. | .. | .. | .. | .. | .. | .. | .. | .. | .. | .. | 0 |
| SeriousSamTSE | .. | .. | .. | .. | .. | .. | .. | .. | .. | .. | .. | .. | .. | .. | .. | .. | .. | 0 |
| ShadowWarrior | .. | +V | +r | .V | .V | .. | .V | +. | .. | .. | .. | .. | +. | .r | +. | .V | -. | 5 |
| Shogo | +. | +V | +V | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +r | G. | +V | -. | 6 |
| SiNGold | +. | +V | +V | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +V | G. | +r | -. | 6 |
| SoldierOfFortune | +. | +V | +V | +V | +V | +. | +. | +. | +. | +. | +. | +. | +. | +r | G. | +r | -. | 4 |
| SoldierOfFortune2 | +. | +V | +V | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +V | G. | +r | -. | 6 |
| StarCraft | +. | +- | +r | +V | +V | +r | +r | +. | +. | +. | +. | +. | +. | +V | G. | +V | -. | 4 |
| SystemShock2 | +. | +- | +V | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +V | G. | +V | -. | 6 |
| Thief2 | +. | +V | +V | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +V | G. | +V | -. | 7 |
| ThiefGold | +. | +V | +V | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +V | G. | +V | -. | 7 |
| TiberianSun | +. | +V | +r | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +r | G. | +V | -. | 5 |
| Tribes2 | +. | +. | +. | +. | +. | +. | +. | +. | +. | +. | +. | +. | +. | +. | G. | +. | -. | 0 |
| Turok2 | .. | +V | +V | .V | .V | .. | .V | +. | .. | .. | .. | .. | +. | .V | G. | .V | -. | 7 |
| UT2003 | +. | +V | +. | +. | +V | +V | +V | +. | .. | +. | .. | .. | +V | +V | G. | .. | -. | 6 |
| UT2004 | +. | +V | +V | +V | +V | +V | +V | +. | +. | +. | +. | +. | +V | +V | G. | +V | -. | 9 |
| UnrealGold | +. | +V | +V | +V | +X | +. | +V | +. | +. | +. | +. | +. | +. | +V | G. | +V | -. | 6 |
| UnrealTournament | +. | +V | GX | +X | +X | +. | +V | +. | .. | +. | .. | +. | +. | +V | G. | +V | -. | 4 |
| UnrealTournament436 | +. | +V | +V | +V | +V | +. | +V | +. | +. | +. | +. | +. | +. | +r | +. | +V | -. | 6 |
| WarcraftII | .. | +r | +r | .V | .V | .. | .r | +. | .. | .. | .. | .. | +. | .r | G. | .r | -. | 2 |
| WarcraftOrcsAndHumans | .. | +V | +V | .V | .X | .. | .V | +. | .. | .. | .. | .. | +. | .V | G. | .V | -. | 6 |
| YurisRevenge | +. | +V | +V | +V | +V | +. | +. | +. | +. | +. | +. | +. | +. | +V | G. | +V | -. | 6 |

**124 titles × 17 machines = 2108 cells — 279 verified, 1732 untested.**

## Titles with a blocker recorded

| title | box | blocker |
|---|---|---|
| AliensVsPredator | `192.168.1.110` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.110` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.110` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.110` | tunnel proven both ends; the front end ignores click *and* key |
| HalfLife-BlueShift | `192.168.1.110` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| HiddenAndDangerous | `192.168.1.110` | launcher bug fixed; stops at profile creation |
| RedFaction | `192.168.1.110` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.110` | IPX tunnel proven; the Build gather never happens |
| Shogo | `192.168.1.110` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.110` | multiplayer refused **even with the disc** — see below |
| SystemShock2 | `192.168.1.110` | the menu offers New Game / Load / Options / Credits / |
| AliensVsPredator | `192.168.1.123` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.123` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.123` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.123` | tunnel proven both ends; the front end ignores click *and* key |
| Daggerfall | `192.168.1.123` | single-player only by design; the GOG DOSBox build staged |
| FarCry | `192.168.1.123` | server hosts unattended; CryEngine takes DirectInput exclusively |
| HalfLife-BlueShift | `192.168.1.123` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| Halo | `192.168.1.123` | **JOINING is automated; HOSTING is not**, and as of 2026-09-01 the **CD keys were also duplicate |
| Halo2 | `192.168.1.123` | halo2.exe loads and holds ~362 MB and creates a fullscreen 'Halo 2' window, but the window never |
| HiddenAndDangerous | `192.168.1.123` | launcher bug fixed; stops at profile creation |
| JediAcademy | `192.168.1.123` | the box has NO disc mounter and NO optical drive at all (HWPROFILE disc_mount=false, wmic logica |
| MasterOfOrionII | `192.168.1.123` | that menu entry is mouse-only |
| MaxPayne | `192.168.1.123` | `MaxPayne.exe` imports no `WS2_32`, `WSOCK32` or `DPLAYX` at all. |
| RedFaction | `192.168.1.123` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.123` | IPX tunnel proven; the Build gather never happens |
| ShadowWarrior | `192.168.1.123` | the in-game gather |
| Shogo | `192.168.1.123` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.123` | multiplayer refused **even with the disc** — see below |
| StarCraft | `192.168.1.123` | no disc_mount capability - .123 is the only fleet box with NO optical drive AND no image mounter |
| SystemShock2 | `192.168.1.123` | the menu offers New Game / Load / Options / Credits / |
| Turok2 | `192.168.1.123` | host works and is listed in the joiner's browser; join fails |
| WarcraftII | `192.168.1.123` | its 8-bit DirectDraw surface is **uncapturable by GDI on both XP and Win7**, so the agent cannot |
| WarcraftOrcsAndHumans | `192.168.1.123` | campaign/network screen is mouse-only |
| AliensVsPredator | `192.168.1.124` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.124` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.124` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.124` | tunnel proven both ends; the front end ignores click *and* key |
| Daggerfall | `192.168.1.124` | single-player only by design; the GOG DOSBox build staged |
| HalfLife-BlueShift | `192.168.1.124` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| HalfLife-OpposingForce | `192.168.1.124` | gearbox\dlls\opfor.dll against the staged WON engine - the engine and console start, then ANY ma |
| Halo | `192.168.1.124` | **JOINING is automated; HOSTING is not**, and as of 2026-09-01 the **CD keys were also duplicate |
| HiddenAndDangerous | `192.168.1.124` | launcher bug fixed; stops at profile creation |
| JediAcademy | `192.168.1.124` | NOT the disc - the image and launcher are staged and proven on .143 and .246. This box's DAEMON  |
| MasterOfOrionII | `192.168.1.124` | that menu entry is mouse-only |
| MaxPayne | `192.168.1.124` | `MaxPayne.exe` imports no `WS2_32`, `WSOCK32` or `DPLAYX` at all. |
| RedFaction | `192.168.1.124` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.124` | IPX tunnel proven; the Build gather never happens |
| ShadowWarrior | `192.168.1.124` | the in-game gather |
| Shogo | `192.168.1.124` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.124` | multiplayer refused **even with the disc** — see below |
| SystemShock2 | `192.168.1.124` | the menu offers New Game / Load / Options / Credits / |
| Turok2 | `192.168.1.124` | host works and is listed in the joiner's browser; join fails |
| WarcraftII | `192.168.1.124` | its 8-bit DirectDraw surface is **uncapturable by GDI on both XP and Win7**, so the agent cannot |
| WarcraftOrcsAndHumans | `192.168.1.124` | campaign/network screen is mouse-only |
| AliensVsPredator | `192.168.1.133` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.133` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.133` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.133` | tunnel proven both ends; the front end ignores click *and* key |
| Daggerfall | `192.168.1.133` | single-player only by design; the GOG DOSBox build staged |
| HalfLife-BlueShift | `192.168.1.133` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| Halo | `192.168.1.133` | cpu_features sse2 (.133 P3 has SSE but no SSE2; .143 Athlon Thunderbird has neither) - the gate  |
| HiddenAndDangerous | `192.168.1.133` | launcher bug fixed; stops at profile creation |
| JediAcademy | `192.168.1.133` | disc image and launcher staged; this box has a DAEMON Tools unit but the title was not exercised |
| MasterOfOrionII | `192.168.1.133` | that menu entry is mouse-only |
| MaxPayne | `192.168.1.133` | `MaxPayne.exe` imports no `WS2_32`, `WSOCK32` or `DPLAYX` at all. |
| RedFaction | `192.168.1.133` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.133` | IPX tunnel proven; the Build gather never happens |
| Shogo | `192.168.1.133` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.133` | multiplayer refused **even with the disc** — see below |
| SystemShock2 | `192.168.1.133` | the menu offers New Game / Load / Options / Credits / |
| WarcraftII | `192.168.1.133` | its 8-bit DirectDraw surface is **uncapturable by GDI on both XP and Win7**, so the agent cannot |
| AliensVsPredator | `192.168.1.143` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.143` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.143` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.143` | tunnel proven both ends; the front end ignores click *and* key |
| Daggerfall | `192.168.1.143` | single-player only by design; the GOG DOSBox build staged |
| HalfLife-BlueShift | `192.168.1.143` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| Halo | `192.168.1.143` | cpu_features sse2 (.133 P3 has SSE but no SSE2; .143 Athlon Thunderbird has neither) - the gate  |
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
| Halo | `192.168.1.145` | **JOINING is automated; HOSTING is not**, and as of 2026-09-01 the **CD keys were also duplicate |
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
| AliensVsPredator | `192.168.1.184` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.184` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.184` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.184` | tunnel proven both ends; the front end ignores click *and* key |
| HalfLife-BlueShift | `192.168.1.184` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| HiddenAndDangerous | `192.168.1.184` | launcher bug fixed; stops at profile creation |
| MasterOfOrionII | `192.168.1.184` | that menu entry is mouse-only |
| MaxPayne | `192.168.1.184` | `MaxPayne.exe` imports no `WS2_32`, `WSOCK32` or `DPLAYX` at all. |
| RedFaction | `192.168.1.184` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.184` | IPX tunnel proven; the Build gather never happens |
| ShadowWarrior | `192.168.1.184` | the in-game gather |
| Shogo | `192.168.1.184` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.184` | multiplayer refused **even with the disc** — see below |
| SystemShock2 | `192.168.1.184` | the menu offers New Game / Load / Options / Credits / |
| Turok2 | `192.168.1.184` | host works and is listed in the joiner's browser; join fails |
| WarcraftII | `192.168.1.184` | its 8-bit DirectDraw surface is **uncapturable by GDI on both XP and Win7**, so the agent cannot |
| WarcraftOrcsAndHumans | `192.168.1.184` | campaign/network screen is mouse-only |
| AliensVsPredator | `192.168.1.185` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.185` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.185` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.185` | tunnel proven both ends; the front end ignores click *and* key |
| HalfLife-BlueShift | `192.168.1.185` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| HiddenAndDangerous | `192.168.1.185` | launcher bug fixed; stops at profile creation |
| RedFaction | `192.168.1.185` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.185` | IPX tunnel proven; the Build gather never happens |
| Shogo | `192.168.1.185` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.185` | multiplayer refused **even with the disc** — see below |
| SystemShock2 | `192.168.1.185` | the menu offers New Game / Load / Options / Credits / |
| AliensVsPredator | `192.168.1.186` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.186` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.186` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.186` | tunnel proven both ends; the front end ignores click *and* key |
| HalfLife-BlueShift | `192.168.1.186` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| HiddenAndDangerous | `192.168.1.186` | launcher bug fixed; stops at profile creation |
| RedFaction | `192.168.1.186` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.186` | IPX tunnel proven; the Build gather never happens |
| Shogo | `192.168.1.186` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.186` | multiplayer refused **even with the disc** — see below |
| SystemShock2 | `192.168.1.186` | the menu offers New Game / Load / Options / Credits / |
| AliensVsPredator | `192.168.1.191` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.191` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.191` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.191` | tunnel proven both ends; the front end ignores click *and* key |
| HalfLife-BlueShift | `192.168.1.191` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| HiddenAndDangerous | `192.168.1.191` | launcher bug fixed; stops at profile creation |
| RedFaction | `192.168.1.191` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.191` | IPX tunnel proven; the Build gather never happens |
| Shogo | `192.168.1.191` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.191` | multiplayer refused **even with the disc** — see below |
| SystemShock2 | `192.168.1.191` | the menu offers New Game / Load / Options / Credits / |
| AliensVsPredator | `192.168.1.195` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.195` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.195` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.195` | tunnel proven both ends; the front end ignores click *and* key |
| HalfLife-BlueShift | `192.168.1.195` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| HiddenAndDangerous | `192.168.1.195` | launcher bug fixed; stops at profile creation |
| RedFaction | `192.168.1.195` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.195` | IPX tunnel proven; the Build gather never happens |
| Shogo | `192.168.1.195` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.195` | multiplayer refused **even with the disc** — see below |
| SystemShock2 | `192.168.1.195` | the menu offers New Game / Load / Options / Credits / |
| AliensVsPredator | `192.168.1.197` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.197` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.197` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.197` | tunnel proven both ends; the front end ignores click *and* key |
| Daggerfall | `192.168.1.197` | single-player only by design; the GOG DOSBox build staged |
| FarCry | `192.168.1.197` | server hosts unattended; CryEngine takes DirectInput exclusively |
| HalfLife-BlueShift | `192.168.1.197` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| Halo | `192.168.1.197` | **JOINING is automated; HOSTING is not**, and as of 2026-09-01 the **CD keys were also duplicate |
| HiddenAndDangerous | `192.168.1.197` | launcher bug fixed; stops at profile creation |
| MasterOfOrionII | `192.168.1.197` | that menu entry is mouse-only |
| MaxPayne | `192.168.1.197` | `MaxPayne.exe` imports no `WS2_32`, `WSOCK32` or `DPLAYX` at all. |
| RedFaction | `192.168.1.197` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.197` | IPX tunnel proven; the Build gather never happens |
| ShadowWarrior | `192.168.1.197` | the in-game gather |
| Shogo | `192.168.1.197` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.197` | multiplayer refused **even with the disc** — see below |
| SystemShock2 | `192.168.1.197` | the menu offers New Game / Load / Options / Credits / |
| Turok2 | `192.168.1.197` | host works and is listed in the joiner's browser; join fails |
| WarcraftII | `192.168.1.197` | its 8-bit DirectDraw surface is **uncapturable by GDI on both XP and Win7**, so the agent cannot |
| WarcraftOrcsAndHumans | `192.168.1.197` | campaign/network screen is mouse-only |
| AliensVsPredator | `192.168.1.240` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.240` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.240` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.240` | tunnel proven both ends; the front end ignores click *and* key |
| FarCry | `192.168.1.240` | server hosts unattended; CryEngine takes DirectInput exclusively |
| HalfLife-BlueShift | `192.168.1.240` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| Halo | `192.168.1.240` | **JOINING is automated; HOSTING is not**, and as of 2026-09-01 the **CD keys were also duplicate |
| HiddenAndDangerous | `192.168.1.240` | launcher bug fixed; stops at profile creation |
| JediAcademy | `192.168.1.240` | NOT the disc - the image and launcher are staged and proven on .143 and .246. This box's DAEMON  |
| MaxPayne | `192.168.1.240` | `MaxPayne.exe` imports no `WS2_32`, `WSOCK32` or `DPLAYX` at all. |
| RedFaction | `192.168.1.240` | root cause fixed (`UpdateRate`); join unproven |
| RedneckRampage | `192.168.1.240` | IPX tunnel proven; the Build gather never happens |
| Shogo | `192.168.1.240` | dedicated server stands up; client menu renders intermittently |
| SoldierOfFortune | `192.168.1.240` | multiplayer refused **even with the disc** — see below |
| SystemShock2 | `192.168.1.240` | the menu offers New Game / Load / Options / Credits / |
| ShadowWarrior | `192.168.1.243` | the in-game gather |
| AliensVsPredator | `192.168.1.246` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.246` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.246` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.246` | tunnel proven both ends; the front end ignores click *and* key |
| CounterStrike16 | `192.168.1.246` | FULLSCREEN stalls the join: with -full the client logs 'Connection accepted by <server>' and nev |
| FarCry | `192.168.1.246` | server hosts unattended; CryEngine takes DirectInput exclusively |
| HalfLife-BlueShift | `192.168.1.246` | `liblist.gam` declares `type "SP Mission"`, `maps\` |
| HalfLife1 | `192.168.1.246` | The HalfLife1 tree's engine is WON hl.exe 1.1.0.8 = network protocol 45, and the fleet Half-Life |
| Halo | `192.168.1.246` | **JOINING is automated; HOSTING is not**, and as of 2026-09-01 the **CD keys were also duplicate |
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
| AliensVsPredator | `192.168.1.249` | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black |
| BF1942 | `192.168.1.249` | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works |
| Carmageddon1 | `192.168.1.249` | tunnel proven both ends; the front end ignores click *and* key |
| Carmageddon2 | `192.168.1.249` | tunnel proven both ends; the front end ignores click *and* key |

