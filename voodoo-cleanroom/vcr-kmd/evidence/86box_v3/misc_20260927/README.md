# Follow-ups build on the 86Box Voodoo3 bed (2026-09-27)

Build: worktree-vk-misc (f146917 flip counters at DestroyDDLocal, 63b4e4f tool
flags, 05c8771/f5e3688 docs, 674f915 review fixes incl. VcrDdRoom's early
return after a give-up). Deployed with deploy_box.py; booted clean (BootAttempts
0, LastDecline 0).

| check | result |
|---|---|
| ddlab flip 800x600x16, 60 frames, in the desktop's own mode | 62.0 flips/s, 0 mismatch, scanline 0 |
| ddlab flip 800x600x16, 600 frames, same mode | 61.5 flips/s, 0 mismatch, scanline 0 |
| recorder (`recorder_after_flips.txt`) | each session logged ONCE at DestroyDDLocal, in its own process: `DestroyDDLocal: pid 1456, flips of 1456 - logged` / `flips 60: retrace 56, deadline 3, pending 1 (DD local gone)`, then pid 784 / `flips 601: retrace 556, deadline 44, pending 1 (DD local gone)`. Before f146917 the two appeared merged ("flips 661") at the next mode change. This also settles the one unverified assumption: XP runs DestroyDDLocal in the releasing process. |
| gdilab | 0 bad |
| d3dprobe render windowed | 42 pass / 0 fail |

fast_frames (4 and 23) is the bed's guest-clock artefact analysed in f146917
(QPC reading a 10 ms timer period ahead for a moment), not early completion.
