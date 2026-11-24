# The Specialists 3.0 on the CounterStrike16 tree - evidence (2026-11-23)

- `vm-before-patch-dll-differs.png` - XP build VM, staged tree + TS 3.0: refused by specialists-server (ReHLDS) with "Your .dll [cl_dlls\client.dll] differs from the server's."
- `vm-after-patch-motd.png`, `vm-after-patch-spawned.png` - same, with the goldsrc4554 hw.dll/sw.dll patch: joined :27017, MOTD, spawned (`-soft`, the VM has no GL).
- `vm-cs16-patched-engine-team-select.png` - CS 1.6 from the patched engine still joins :27015.
- `box133-ts-ingame-fullscreen-1280x960.png` - .133 (XP, GeForce4 Ti) after a library GAMESYNC (failed_files 0, titles_total 117 = library): the "The Specialists - Fleet Server" shortcut's batch, OpenGL, fullscreen 1280x960, in-game on 192.168.1.132:27017 (server log: entered the game).
