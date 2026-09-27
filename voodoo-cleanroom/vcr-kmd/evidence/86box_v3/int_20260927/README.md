# The integrated vcr-kmd build on the 86Box Voodoo3 bed (2026-09-27)

`worktree-vk-int` at 5e4ca36 (master + the SLI/AA safety net, D3D32 hardening,
the flip refactor, glidelab tracing), `make all labs`, installed with the bed's
recipe (`deploy_box.py install 127.0.0.1 --port 19920 --hwid
'PCI\VEN_121A&DEV_0005' --rollback-dir "" --reboot-cmd
tools/86box/restart-guest.sh`). The bed ran 5be6a59 before. Only the 86Box
guest agent (127.0.0.1:19920) was contacted. Critic plan steps 5, 7 and 13.

Every run keeps its tool output (`*.txt`) and the recorder entries it
produced (`*.vcrlog.tsv`; decode with `tools/vcrlog.py decode`). The
`*_summary.jsonl` files are rebuilt from those logs by
`harness/resummarize.py` (window: seq > the `log_next_seq` read before the
run - that field is the LAST seq written, not the next). `harness/` holds the
bed-only drivers this session used.

## 1. Install and boot - PASS

| | before (5be6a59) | after (integration) |
|---|---|---|
| guest `vcrmp.sys` md5 | 7fe59d29... | **62a41695... = out/** |
| guest `vcrdd.dll` md5 | 11583d6e... | **8702d7e7... = out/** |
| DRIVER_ENTRY d = sizeof(VCR_EXT) | 0x1278 | **0x1388** |
| Diag BootAttempts / LastDecline | 0 / 0 | 0 / 0 |
| Diag DeclinedBoots (cumulative) | 1 | 1 (no new decline, over 3 installs this session) |
| stable-boot mark | - | SAFE_MARK_OK at 70.9 s |

`vcrctl info` says `build 1` for BOTH builds (`.buildnum` is 1 in each
worktree), so the build is identified by the file hashes and the
DRIVER_ENTRY struct size, not by `build`.

The first `deploy_box.py install` exited 1 with a traceback although the
install had worked: the guest's agent auto-updated (1.85.3 -> 1.85.4, from
the share, on its own) 20 s after it first answered PING and restarted under
the verify's REGREAD (ConnectionResetError). `deploy_box.py status` then exited
1 on a successful read: status() returns True and `isinstance(True, int)`.
Both fixed in `tools/deploy_box.py` (`status_after_boot`, `exit_code`) with
`test_deploy_box_exit_status_and_a_verify_that_survives_an_agent_restart`;
the two later installs (06, 07) verified cleanly with rc 0.

## 2. Regression vs the recorded baselines - PASS

| lab (800x600x16 unless noted) | baseline | integration build |
|---|---|---|
| gdilab | 0 bad | 0 bad (02, and 07 smoke) |
| ddlab blt | 2349 / 2326 / 2344 / 2424 blts/s, 0 bad | 2199 / 2403 / 2242 / 2285 / 2239 / 2205 / 2239 (mean 2259), 0 bad |
| ddlab blt, SAME SESSION, 5be6a59 reinstalled (06) | 2363 / 2290 / 2275 / 2169 / 2391 (mean 2298), 0 bad | -1.7 % vs that: within the run-to-run spread (2169-2403 across both) |
| ddlab flip, 120 frames | 63.8 flips/s at 60.4 Hz, 0 mismatch, **scanline 2293576** | 61.5 flips/s at 60.4 Hz, 0 mismatch, **scanline 0** |
| d3dprobe render windowed (16 bpp desktop) | 42/0 | 42/0 (02, 04 e8 control, 07 smoke) |
| d3dprobe render fullscreen 640x480x16 | 40/0 | 40/0 |
| d3dprobe render fullscreen 800x600x16 | 26/26 (older suite) | 40/0 |

The blt numbers read ~4 % under the older baselines, but no blt code changed
between the builds (the diff touches flip/wait/exclusive/CanCreateSurface
only) and the previous build, reinstalled in the same session, measured the
same (-1.7 %, inside the spread): host variance, not a regression.

## 3. Flip instrumentation (step 7) - no half rate; two open findings

All in `03_flip/`, 800x600, refresh chosen by XP (60.353 Hz from the PLL;
ddlab's vblank_hz 60.4). `desk16`: the original 16 bpp desktop, 16 then 32
bpp (a*) and reversed (b*). `desk32`: the desktop set to 800x600x32 (the .124
condition), 16 then 32 (c*) and reversed (d*); restored to 800x600x16@60
afterwards. 511/12 = flips, by retrace, by deadline; 511/13 = the deadline.

| run | desk | bpp | frames | flips/s | first-last | first ms | max ms | slow | 511/12 flips, retrace, deadline |
|---|---|---|---|---|---|---|---|---|---|
| a1+a2 | 16 | 16 | 60+600 | 61.9 / 62.4 | 61.0 / 62.3 | 1.68 / 1.68 | 21.3 / 25.8 | 0 / 2 | 661, 617, 43 (merged, see below) |
| a3 | 16 | 32 | 60 | 62.7 | 61.9 | 2.01 | 26.1 | 1 | 60, 51, 8 |
| a4 | 16 | 32 | 600 | 60.1 | 60.0 | 2.02 | 67.1 | 11 | 600, 535, 64 |
| b1 | 16 | 32 | 60 | 61.9 | 61.1 | 2.02 | 22.1 | 0 | 60, 56, 3 |
| b2 | 16 | 32 | 600 | 60.0 | 59.9 | 2.02 | 81.6 | 14 | 600, 544, 55 |
| b3 | 16 | 16 | 60 | 61.4 | 60.5 | 1.67 | 19.1 | 0 | 60, 57, 2 |
| b4 | 16 | 16 | 600 | 61.9 | 61.9 | 1.69 | 26.6 | 3 | 600, 571, 28 |
| c1 | 32 | 16 | 60 | 61.8 | 60.9 | 1.68 | 26.4 | 1 | 60, 56, 3 |
| c2 | 32 | 16 | 600 | 60.5 | 60.4 | **3.85** | 79.8 | 12 | 600, 542, 57 |
| c3 | 32 | 32 | 60 | 63.4 | 62.6 | 2.02 | 19.5 | 0 | 60, 57, 2 |
| c4 | 32 | 32 | 600 | 61.4 | 61.3 | 2.02 | 26.7 | 4 | 600, 571, 28 |
| d1 | 32 | 32 | 60 | 64.0 | 63.2 | 2.02 | 21.9 | 0 | 60, 53, 6 |
| d2 | 32 | 32 | 600 | 61.6 | 61.5 | 2.02 | 25.9 | 1 | 600, 573, 26 |
| d3 | 32 | 16 | 60 | 58.7 | 57.9 | 2.37 | 37.3 | 4 | 60, 49, 10 |
| d4 | 32 | 16 | 600 | 59.8 | 59.8 | 1.68 | 75.7 | 9 | 600, 563, 36 |

Every run: mismatch 0, lock_fail 0, **scanline 0** (GetScanLine fix; 5be6a59
still reads 2293576 in 06), deadline 18749 us (the nominal 60 Hz rule).

- **No half rate at either depth, on either desktop, in either order**:
  flips_s 58.7-64.0 against a 60.35 Hz refresh; retrace completions 89-96 % of
  600 frames. No long first frame either (1.67-2.37 ms; 3.85 ms once; the
  very first flip after a fresh boot took 22.4 ms, 07 smoke). The .124 16 bpp
  half rate does not reproduce on the bed - it stays a silicon question
  (plan step 16).
- The deadline completions (4-11 % of 600 frames, 2-10 of 60, in a loop that
  polls continuously) are the polling thread being descheduled across the
  vsync flag - inside a read (511/13's "longest read" reaches 5-57 ms of guest
  time in most runs) or between reads (b3 and c1 missed 2-3 with no read over
  0.5 ms). The retrace is missed and the deadline completes the flip - late,
  as designed.
- **OPEN - a same-mode DirectDraw session's counters are not logged at its
  end.** A ddlab run in the desktop's own mode logs "DirectDraw exclusive 1"
  but XP never calls SetExclusiveMode(0) on its release, and no mode leaves
  the screen, so its 511/12-13 appear only at the next mode change - merged
  with any later same-mode session (a1+a2 above: 661 = 1 carried + 60 + 600).
  Runs marked `--flush` in the harness force one paced switch (`vcrctl
  setmode 640 480 16`, reverted at exit) to log them. A driver fix would log
  at DestroyDDLocal (the process's DirectDraw object going away); not done
  here.
- **OPEN - completions outrun the refresh by 2-3 % at 600 frames.**
  flips_s_first_last 61.2-62.3 vs 60.35 Hz in no-work runs, and ddlab's new
  `fast_frames` (flip to flip under HALF a refresh) counts 22-29 per 600
  frames with min_frame_ms 1.0-1.6 ms (`03_flip/fastframes`). Each flip needs
  its own vsync to latch, so more completions than vsyncs means either some
  flips are called done before the chip latched them on this bed, or the
  guest clock sees a shorter frame than WaitForVerticalBlank measures
  (vblank_hz agrees with the PLL to 0.1 %, which argues against it). NOT
  introduced by this build: 5be6a59 shows the same (62.2 flips/s, 25 fast
  frames, 06). mismatch cannot see it (it reads the surface, not the
  scan-out). fast_frames tracks the deadline count (26/27, 29/33, 22/33),
  which fits a flip written just before a vsync after a late (deadline)
  completion - legitimate - but that alone cannot produce MORE completions
  than vsyncs. Next: fast_frames and flips_s_first_last on silicon (step 16),
  and an 86Box-side trace of the desktop_addr latch against the guest's writes.

### Deadline A/B (Diag\FlipDeadline) - the switch works; the bed cannot show the ratio

`03_flip/deadline_ab/`: 800x600x16, 600 frames, `--work-us` after every Flip.
FlipDeadline written, read back, and picked up by a forced mode set before
each B run; deleted (`reg delete ... /v`), read back absent and picked up
again before each A run. Final state: absent.

| work us | A: nominal (absent) | B: achieved (=1) | B/A |
|---|---|---|---|
| 17400 (1.05 frame) | 46.1 (A1), 49.0 (A2) flips/s; deadline 18749 us | 48.8; deadline **17086 us** | 1.06 / 1.00 |
| 16000 | 49.3; 53 retrace / 546 deadline | 50.5; 13 / 586 | 1.024 |
| 15000 | 50.4; 82 / 517 | 51.4; 23 / 576 | 1.020 |

The counters prove the switch took effect (18749 -> 17086 us = frame + 1/32
of 60.353 Hz; longest wait 10.5 ms -> 0 at 17.4 ms). The expected 0.889x vs
0.95x of the refresh (53.7 vs 57.3 flips/s) is NOT reproduced: the emulated
Pentium II spends ~3 ms per frame outside the busy work (Lock, the pattern
write, the check, the Flip call), which lands the first poll after both
deadlines at 17.4 ms and shrinks the gap at 15-16 ms; 29-121 slow frames per
run (descheduling) add noise the size of the effect (A1 vs A2: 46.1 vs 49.0).
Direction is right at every work value; the ratio needs silicon.

## 4. Direct3D refusals (step 5) - PASS; the layer is named

`04_d3d/`. 8876086c = D3DERR_INVALIDCALL, 88760091 = DDERR_INVALIDPIXELFORMAT.

| request | desktop | result | refusing layer (recorder) |
|---|---|---|---|
| caps | 32 bpp | hal_fullscreen R5G6B5 1, X8R8G8B8 0; every format 0 at the X8R8G8B8 adapter format | - |
| render windowed | 32 bpp | CreateDevice 8876086c, no hang | D3D8 runtime only (no 513/12, no context) |
| render fullscreen 640x480x32 | 32 bpp | CreateDevice 8876086c; exclusive 1 -> 0, **no mode set** | runtime only |
| caps | 16 bpp | R5G6B5: D16 format 1 match 1; D24X8/D24S8 format 0 match 0 | - |
| render fullscreen 640x480x32 | 16 bpp | CreateDevice 8876086c (rt32 baseline: same) | runtime only |
| render windowed `--zfmt d24s8` (16 bpp device, D24S8) | 16 bpp | CreateDevice 8876086c, zfmt 75 | runtime only - the HAL lists D16 alone, so the runtime refuses first |
| render fullscreen 640x480x16 `--zfmt d24s8` | 16 bpp | CreateDevice 8876086c, zfmt 75 | runtime only |
| control: render windowed `--zfmt d16` | 16 bpp | 42/0 | - |
| DirectDraw 7 Z surface, 32 bits (24+8), `ddlab zsurf` | 16 bpp | 88760091, not created | **511/11: CanCreateSurface: 32-bit Z refused (16-bit Z only: Banshee/Voodoo3)** |
| DirectDraw 7 Z surface, 24 bits | 16 bpp | 88760091, not created | **511/11** (24-bit Z refused) |
| control: DirectDraw 7 Z surface, 16 bits | 16 bpp | created, in video memory | - |

On a Voodoo3 no D3D8 request ever reaches the driver's 513/12 or 511/11: the
runtime refuses from the caps. The driver's own CanCreateSurface guard is
reachable only the DirectDraw 7 way (a D3D7 game's Z surface), which the new
`ddlab zsurf` does - and it refuses there. `d3dprobe --zfmt` and `ddlab zsurf`
are new in this commit (tests in test_vcr_kmd_d3d.py / test_vcr_kmd_ddraw.py).

## 5. SLI/AA refusal plumbing (step 13) - PASS

`05_sliaa/`. `--i-am-at-the-box` passed where named (an emulator);
`--force-desktop-pll` to get past the tool's own 2x-mode gate and reach the
kernel. **Zero SLI register writes (SLI_STEP 100-899) in every run**; no
SLI_POKE_REFUSED. Desktop 800x600x16@60 and exclusive owner 0 after every run.

| run | Diag\SliAA | request | refused by | kernel result |
|---|---|---|---|---|
| s1 | absent | 1 0 1 0 0 | the tool: no `--i-am-at-the-box` (nothing sent) | - |
| s2 | absent | 1 0 1 0 0 --i-am-at-the-box | the tool: desktop not in 2x mode (nothing sent) | - |
| s3 | absent | 1 0 1 0 0 + --force-desktop-pll | **kernel policy: EDENIED (-4), reason 10 AA_OFF**, "refused before any write" (SLI_STEP 901, persisted phase) | resStatus FAIL; the tool released in the same process |
| s4 | absent | 4 0 1 0 1 (cfg 1's tuple) | the tool: no video-mux branch (nothing sent) | - |
| s5 | absent | 4 1 0 0 0 (4-way SLI) | **kernel: EINVAL (-1)**, "4 chips asked, 1 available, device 0005" | FAIL |
| s6 | **1** | 1 0 1 0 0 + --force-desktop-pll | **kernel Napalm/chip check: EINVAL (-1)**, "device 0005" | FAIL |
| s7 | **1** | 4 0 1 1 1 + --force-desktop-pll | **kernel: EINVAL (-1)** | FAIL |
| s3/s5/s6/s7 `_off` | - | `vcrctl sliaa off`, a SEPARATE process | - | disable OK; exclusive taken, released, mode set back to 800x600x16@60, owner 0 |

SliAA was written, read back (=1), and deleted and read back absent.

**The integration fix, with a real stale owner** (`05_sliaa/owner/`): the
refused enables above release their own exclusive, so they leave no owner to
test `off` against - and on a single chip no enable can succeed. So a
bed-only probe (`harness/stale_owner.c`) took HWCSETEXCLUSIVE and exited
without a release: `vcrctl info` showed `exclusive_pid 1844`. A second
process's HWCRLSEXCLUSIVE was refused (604 a=4, "the chip is owned by 1844 -
no mode set, nothing written"). Then `vcrctl sliaa off` took exclusive
(pid 0xdc), sent the disable, released as the owner, the desktop mode was
set back and `exclusive_pid` read 0. Before 5e4ca36 that `off` was the
refused non-owner release.

`vcrctl info` could not show an owner at all before this commit:
`vcr_info.exclusive_pid` is the miniport's and nothing fills it; the owner
lives in the display driver's PDEV. info now reads it through
VCR_ESC_DD_STATS (`dd_mode`, `exclusive_pid`, `hwc_requests`; null when not
answered) - test_vcrctl_info_reports_the_display_drivers_exclusive_owner.

## Bed state left

Integration build installed (07: guest files = out/, DRIVER_ENTRY 0x1388,
the guest's vcrctl.exe is the rebuilt one with the owner field), smoke gdilab
0 bad / flip 0 mismatch / render 42/0. Desktop 800x600x16@60. Diag value names
identical to before the session - no FlipDeadline, no SliAA (AllowPoke = 1 was
already there and is untouched). Nothing wedged; the bed was never restarted
except by the three installs.
