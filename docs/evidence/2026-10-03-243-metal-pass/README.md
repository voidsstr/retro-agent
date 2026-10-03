# .243 (the P1) - on-the-metal pass of its 3dfx and DOS shortcuts, 2026-10-03

Every title below had only been run in the Win98 build VM (86Box, an emulated
Voodoo 2) until this pass. `metal243.py` (here) launched each desktop
shortcut's own `.bat` on the real Pentium 166 + Voodoo 2, waited for it to
load (35-75 s), recorded the game process and any dialog, then quit it - the
engine's console `quit` where it has one, else PROCKILL - closed what was
left and handed a Glide title's screen back to the 2D card (`glreset9x`).

**What this proves and what it does not:** each title starts, stays up and
raises no error on the real machine. The agent cannot photograph the Voodoo
2's output (it drives the monitor directly through its passthrough), so the
picture itself was judged by the person at the box, not captured here. The
compat matrix records these as `runs`, not `verified`.

| shortcut | game process | dialogs | how it was closed |
|---|---|---|---|
| Quake - 3dfx Voodoo | glquake.exe | none | console quit |
| Quake - 3dfx Voodoo 800x600 | glquake.exe | none | console quit |
| Quake - Scourge of Armagon - 3dfx Voodoo | glquake.exe | none | console quit |
| Quake - Dissolution of Eternity - 3dfx Voodoo | glquake.exe | none | console quit |
| Quake - DOS | DOS box | none | DOS box closed |
| Quake - DOS 360x480 | DOS box | none | DOS box closed |
| Quake - Scourge of Armagon - DOS | DOS box | none | DOS box closed |
| Quake - Dissolution of Eternity - DOS | DOS box | none | DOS box closed |
| Quake II | quake2.exe | none | console quit |
| Quake II - 3dfx Voodoo | quake2.exe | none | console quit |
| Quake II - The Reckoning - 3dfx Voodoo | quake2.exe | none | console quit |
| Quake II - Ground Zero - 3dfx Voodoo | quake2.exe | none | console quit |
| SiN - 3dfx Voodoo | sin.exe | none | console quit |
| SiN - Wages of SiN - 3dfx Voodoo | sin.exe | none | console quit |
| Half-Life - 3dfx Voodoo | hl.exe | none | console quit |
| Team Fortress Classic - 3dfx Voodoo | hl.exe | none | console quit |
| Deathmatch Classic - 3dfx Voodoo | hl.exe | none | console quit |
| Hexen II - 3dfx Voodoo | glh2.exe | none | PROCKILL glh2.exe |
| Unreal Gold - 3dfx Voodoo | unreal.exe | none | console quit |
| Carmageddon 2 - 3dfx Voodoo | carma2_hw.exe | none | PROCKILL carma2_hw.exe |
| Wing Commander Prophecy - 3dfx Voodoo | prophecy.exe | none | PROCKILL prophecy.exe |
| Wing Commander Secret Ops - 3dfx Voodoo | secretops.exe | none | PROCKILL secretops.exe |
| Die by the Sword - 3dfx Voodoo | windie.exe | none | PROCKILL windie.exe |
| Star Wars Rogue Squadron 3D - 3dfx Voodoo | rogue squadron.exe | none | PROCKILL rogue squadron.exe |
| X-Wing | DOS box | none | DOS box closed |
| TIE Fighter | DOS box | none | DOS box closed |
| Descent - DOS | DOS box | none | DOS box closed |
| Descent II - DOS | DOS box | none | DOS box closed |

**TIE Fighter** left its DOS box within the 35 s wait in the pass: it printed
`Error in Sound Config! Run setmuse!`. Its SETMUSE.INI matches the library and
the SB16 is present at 220/5/1. After a clean restart the same shortcut ran
(full-screen DOS box, no error): the pass had just ended five DirectSound
games by PROCKILL (Hexen II, Carmageddon 2, both Wing Commanders, Die by the
Sword, Rogue Squadron), and on Win98 a killed DirectSound program can leave
the card held until a restart. A test-procedure artefact, not a staging fault -
but the next pass should quit those games through their menus, or test the
DOS titles first.
