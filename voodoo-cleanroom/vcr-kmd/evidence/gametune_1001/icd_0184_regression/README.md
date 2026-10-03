# ICD 0.1.84 regression sweep, `.124`, 2026-10-03 10:28-10:38

0.1.84 hands Glide the drawable's top-level window when the drawable is a
WS_CHILD. Every other game keeps the identical path. Eight OpenGL titles were
run through `lan_sweep.py` from their desktop shortcuts, each checked at 30 s
and 60 s:

| title | verdict |
|---|---|
| Quake II | PASS (console quit) |
| Quake III Arena (ioquake3) | PASS |
| Return to Castle Wolfenstein | PASS |
| Serious Sam - The First Encounter | PASS |
| Serious Sam - The Second Encounter | PASS |
| Tribes 2 Solo and LAN | PASS |
| Descent 3 | CHECK, close path only: alive at both samples, then forced closed. Its menus cannot be driven, as in the 10-02 re-sweep |
| Hexen II | CHECK, close path only: alive at both samples; WM_CLOSE raises its own "Confirm Exit" box |

No FAIL, and the board was healthy after each title. PNG frames are not
committed; `sweep.json` holds the samples.
