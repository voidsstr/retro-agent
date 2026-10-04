| shortcut | verdict | GDI mode seen | processes | board OK after |
|---|---|---|---|---|
| Carmageddon 2 | CHECK: had to be forced closed; close keys withheld - game not focused | 640x480 | CARMA2_HW.EXE | yes |
| Counter-Strike 1.6 | PASS | 1280x960 | hl.exe | yes |
| Deathmatch Classic | PASS | 1280x960 | hl.exe | yes |
| Descent - Rebirth engine | PASS | 320x200 1280x960 | d1x-rebirth.exe | yes |
| Descent 3 | CHECK: had to be forced closed; close keys withheld - game not focused | 640x480 | main.exe | yes |
| Descent II - original Win95 engine | CHECK: dialog: Electronic Registration Card | 1280x1024 | DESCENTW.EXE, REGCARD.EXE | yes |
| Descent II | CHECK: had to be forced closed | 1280x960 | d2x-rebirth.exe | yes |
| Deus Ex | PASS | 1280x960 | DEUSEX.EXE | yes |
| Half-Life - Blue Shift | CHECK: started no process; Dr. Watson entry | 1280x1024 |  | yes |
| Half-Life - Opposing Force | PASS | 1280x960 | hl.exe | yes |
| Half-Life Deathmatch - Fleet Server | PASS | 1280x960 | find.exe, hl.exe, netstat.exe | yes |
| Half-Life | CHECK: had to be forced closed | 1280x960 | hl.exe | yes |
| Hexen II - 3dfx Voodoo | PASS | 1024x768 | glh2.exe | yes |
| Hexen II | PASS | 1024x768 | glh2.exe | yes |
| Hidden and Dangerous Deluxe | PASS | 1280x960 | HDE.exe | yes |
| Jedi Academy - Multiplayer | PASS | 1280x960 | jamp.exe | yes |
| Jedi Academy | PASS | 1280x960 | jasp.exe | yes |
| Quake - 3dfx Voodoo | PASS | 1280x960 | GLQUAKE.EXE | yes |
| Quake - Dissolution of Eternity | PASS | 1280x960 | GLQUAKE.EXE | yes |
| Quake - Scourge of Armagon | PASS | 1280x960 | GLQUAKE.EXE | yes |
| Quake II - Ground Zero | PASS | 1280x960 | quake2.exe | yes |
| Quake II - The Reckoning | PASS | 1280x960 | quake2.exe | yes |
| Quake II - ThreeWave CTF | PASS | 1280x960 | quake2.exe | yes |
| Quake II | PASS | 1280x960 | quake2.exe | yes |
| Quake III - Team Arena | PASS | 1280x960 | ioquake3.x86.exe | yes |
| Quake III Arena (retail 1.32c) | PASS | 1280x960 | quake3.exe | yes |
| Quake III Arena | PASS | 1280x960 | ioquake3.x86.exe | yes |
| Quake | PASS | 1280x960 | GLQUAKE.EXE | yes |
| Return to Castle Wolfenstein | PASS | 1280x960 | WolfSP.exe | yes |
| RTCW Multiplayer | PASS | 1280x960 | WolfMP.exe | yes |
| Serious Sam - The First Encounter | PASS | 1280x960 1280x1024 | SeriousSam.exe | yes |
| Serious Sam - The Second Encounter | PASS | 1280x960 1280x1024 | SeriousSam.exe | yes |
| Shogo - Mobile Armor Division | PASS | 1280x960 | Client.exe | yes |
| Shogo - via launcher (fallback) | CHECK: dialog: Shogo: Mobile Armor Division | 1280x1024 | Shogo.exe | yes |
| SiN - Wages of SiN | PASS | 1280x960 | sin.exe | yes |
| SiN | PASS | 1280x960 | sin.exe | yes |
| Soldier of Fortune II - Multiplayer | CHECK: had to be forced closed; close keys withheld - game not focused | 1280x960 | sof2mp.exe | yes |
| Soldier of Fortune II | CHECK: dialog: New Video card detected; had to be forced closed | 640x480 1280x1024 | SoF2.exe | yes |
| Soldier of Fortune | PASS | 1280x960 | SoF.exe | yes |
| Team Fortress Classic | PASS | 1280x960 | hl.exe | yes |
| Tom Clancy's Rainbow Six | CHECK: error window: RAINBOW SIX ERROR; dialog: RAINBOW SIX ERROR | 640x480 | RainbowSix.exe | yes |
| Tribes 2 Solo and LAN | PASS | 800x600 | Tribes2.exe, rubyintersect.dll | yes |
| Turok 2 - Multiplayer | PASS | 1280x1024 | Turok2MPEnglish.exe | yes |
| Turok 2 - Seeds of Evil | PASS | 1280x1024 | Turok2English.exe | yes |
| Unreal Gold | CHECK: had to be forced closed; close keys withheld - game not focused | 1024x768 | Unreal.exe | yes |
| Unreal Tournament - 3dfx Voodoo | PASS | 640x480 | UnrealTournament.exe | yes |
| Unreal Tournament 2003 | PASS | 640x480 1280x1024 | UT2003.exe | yes |
| Unreal Tournament 2004 | PASS | 1280x960 1280x1024 | UT2004.exe | yes |
| Unreal Tournament 436 | PASS | 1280x960 | UnrealTournament.exe | yes |

## Frames

The sweep took two `SCREENSHOT 1` frames of every shortcut, at 30 s and 60 s
(98 PNGs, 640x480, 39 MB). The repo keeps them as five contact sheets,
`contact_1_of_5.jpg` .. `contact_5_of_5.jpg` (1.2 MB). Each tile is captioned
with its shortcut and verdict. The full-size PNGs are on the dev host in
`~/.retro-fleet/evidence/v56k-gametune_1001-sweep_baseline-20261001_192225/`,
under the names `sweep.json` gives them. They were copied there and compared
byte for byte on 2026-10-04.

**Read the thumbnails with care.** `SCREENSHOT` is a GDI capture. For a Glide
title it reads 2D video memory, not the image on the monitor, so most Glide
tiles show desktop icons, stripes or noise while the game was rendering
normally. The verdicts come from:

- the processes running;
- the dialogs;
- the board check.

The scanned-out frame comes from `vcrctl fbshot`.
