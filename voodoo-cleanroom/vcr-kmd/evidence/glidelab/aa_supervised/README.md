# Supervised AA on the V5 6000 - 2026-09-27 (256 MB mode, user at the box)

Kernel: vcr-kmd master (install9 build) with the AA kill switch, the SLI/AA
shape refusal and the idle-slave sync fix. Glide: our h5 fork e767d89 (md5
38a891e8, SLIAA-GUARD + AA-TRACE). Monitor HP P1120.

| step | what | result |
|---|---|---|
| A | glidelab fill cfg 1, trace 2 (SliAA not armed) | Glide REFUSED it before any mode set or kernel write: `winopen: REFUSED - single chip with AA on a multi-chip board`. No wedge. |
| B | arm Diag\SliAA=1 + SliPersistAll=1, safe reboot, wait for the boot mark, glidelab fill cfg 3 (4 chips, SLI, 2-sample, analog) as the first Glide app, trace 2 | completed, no wedge: 618.6 Mpix/s (cfg 5: 1124.4). Trace in `stepB_cfg3/` (231 lines: the request, per-chip aaCtrl chips 0/2 0x01830204, 1/3 0x0081060c, sliCtrl 0/1 0x051f0020, 2/3 0x051f2020). |
| B' | glidelab edges (static thin slanted triangles, no LFB read) cfg 5 then cfg 3, 15 s and 30 s | the user: cfg 5 jagged (right, no AA); cfg 3 **a ghost / double image** - two overlapping copies. |

So in 2-sample AA both chips of each pair draw, and the analog merge adds two
images that are NOT aligned. Analysis of the cause (kernel video merge / vsync
offset, Glide sample offsets, vendor behaviour, against the vintage H5 source)
is the next step. SliAA and SliPersistAll were deleted again after the runs.
