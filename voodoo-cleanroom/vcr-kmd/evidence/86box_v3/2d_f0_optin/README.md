# Text on the 2D engine - opt-in, verified on the 86Box Voodoo3 bed (2026-09-27)

The text track (DrvTextOut by monochrome expansion: one 1 bpp mask per clip
rectangle sent as a host-to-screen blit; `include/vcr_text.h`) was built and
tested on the bed by the track agent (evidence dirs `2d_a0_before` ..
`2d_e0_final`, harness `2d_harness/`). Its reviews never ran (the account's
usage limit), so it lands **default OFF**: `Diag\Accel2DText = 1` (a positive
`VCR_INFO_F_TEXT2D`, read at every IOCTL_VCR_INFO, so the next mode change
picks it up - no reboot).

Why off: correctness is clean, speed on the bed is not better.

| glyphs/s on the bed | engine text (ours) | software (ours, text off) | in-box XP driver |
|---|---|---|---|
| mssans8 opaque, 16 bpp | 113-116k (`2d_e0_final`) | 139k (`2d_a0_before`) | 275k at 32 bpp (`2d_b0_inbox`) |
| courier13 opaque, 16 bpp | 65-82k | 71k | 224k at 32 bpp |
| arial33b transparent, 16 bpp | 18-21k | 40k | 52k at 32 bpp |

Every engine call waits on PCI FIFO room (`text_fifo_waits` 13-20k per 2 s
bench) - the emulator's FIFO drains slowly; on silicon the balance may differ
(the software path's framebuffer writes cross the bus too). Measure on `.124`
with the user present before arming it there.

This run (build = worktree-vk-2d with the opt-in change):

| check | result |
|---|---|
| install | BootAttempts 0, LastDecline 0 |
| gdilab base,text 16 bpp, default (text off) | 0 bad; text_engine_calls 0, text_software 90 - today's path |
| arm `Accel2DText=1`, mode change to 800x600x32 | read back 1 |
| gdilab base,text 32 bpp, armed | 0 bad; text_engine_calls 90, glyphs 2301, clipped 276, blits 192, software 0 |
| disarm (`reg delete ... /v Accel2DText` - a value; never REGDELETE), back to 800x600x16 | value absent |
| d3dprobe render windowed | 42 pass / 0 fail |

Open: gdilab's BASE tests (fill/rop/copy/scroll) report bad pixels at 8 bpp
(`2d_a6_diag_off/gdilab_8bpp_off.txt`, with the 2D engine on and text off) -
not investigated; the track's 8 bpp text runs used `--tests text,bench`.
Whether that is gdilab's 8 bpp reference (palette mapping, tolerance 0) or the
driver's 8 bpp fill/copy is unknown.
