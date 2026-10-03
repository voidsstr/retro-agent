# Targeted re-sweep after the tuning (`.124`, 2026-10-02 23:39 - 00:11)

The 20 shortcuts tuned on 2026-10-02, each started through its real desktop shortcut
(`lan_sweep.py --only`, regex in `only_regex.txt`) and closed by the sweep: WM_CLOSE, keys only
when the game has the focus, force after the grace. **0 FAIL; every game alive at 60 s; the board
healthy after every title.** The CHECKs are all in the CLOSE path, now visible because the sweep
records a dialog raised by its own WM_CLOSE (`dialogs_on_close`, new tonight).

| shortcut | verdict | alive at 60 s | dialog while closing | forced | board | note |
|---|---|---|---|---|---|---|
| Descent 3 | CHECK | yes | - | yes | ok | focus held by the agent console - WM_CLOSE, then forced |
| Deus Ex | CHECK | yes | Critical Error | yes | ok | UE1 on Glide: "Assertion failed: RenDev" in EndFullscreen <- WM_KILLFOCUS (the game's own exit is clean - tested per title) |
| Hexen II | CHECK | yes | Confirm Exit | no | ok | the game's own quit prompt - normal |
| Jedi Academy - Multiplayer | PASS | yes | - | no | ok |  |
| Jedi Academy | PASS | yes | - | no | ok |  |
| Quake II - Ground Zero | CHECK | yes | Error | no | ok | Quake II engine: "GLimp_EndFrame() - SwapBuffers() failed!" - its shutdown deletes the context before its last frame (known; console/menu quit is clean) |
| Quake II - The Reckoning | CHECK | yes | Error | no | ok | Quake II engine: "GLimp_EndFrame() - SwapBuffers() failed!" - its shutdown deletes the context before its last frame (known; console/menu quit is clean) |
| Quake II - ThreeWave CTF | CHECK | yes | Error | no | ok | Quake II engine: "GLimp_EndFrame() - SwapBuffers() failed!" - its shutdown deletes the context before its last frame (known; console/menu quit is clean) |
| Quake II | CHECK | yes | Error | no | ok | Quake II engine: "GLimp_EndFrame() - SwapBuffers() failed!" - its shutdown deletes the context before its last frame (known; console/menu quit is clean) |
| Quake III Arena (retail 1.32c) | PASS | yes | - | no | ok |  |
| Return to Castle Wolfenstein | PASS | yes | - | no | ok |  |
| RTCW Multiplayer | PASS | yes | - | no | ok |  |
| SiN - Wages of SiN | CHECK | yes | Error | no | ok | Quake II engine: "GLimp_EndFrame() - SwapBuffers() failed!" - its shutdown deletes the context before its last frame (known; console/menu quit is clean) |
| SiN | CHECK | yes | Error | no | ok | Quake II engine: "GLimp_EndFrame() - SwapBuffers() failed!" - its shutdown deletes the context before its last frame (known; console/menu quit is clean) |
| Soldier of Fortune II - Multiplayer | CHECK | yes | - | yes | ok | focus held by the agent console - WM_CLOSE, then forced |
| Soldier of Fortune II | CHECK | yes | - | yes | ok | focus held by the agent console - WM_CLOSE, then forced |
| Soldier of Fortune | PASS | yes | - | no | ok |  |
| Tribes 2 Solo and LAN | PASS | yes | - | no | ok |  |
| Unreal Gold | CHECK | yes | Critical Error | yes | ok | UE1 on Glide: "Assertion failed: RenDev" in EndFullscreen <- WM_KILLFOCUS (the game's own exit is clean - tested per title) |
| Unreal Tournament 436 | CHECK | yes | Critical Error | yes | ok | UE1 on Glide: "Assertion failed: RenDev" in EndFullscreen <- WM_KILLFOCUS (the game's own exit is clean - tested per title) |

GDI captures were not kept: on this driver they show the desktop surface, not an exclusive
Glide/OpenGL frame. `sweep.json` has every sample (alive processes, GDI mode, luma).
