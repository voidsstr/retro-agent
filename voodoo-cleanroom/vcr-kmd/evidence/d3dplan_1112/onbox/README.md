# On-box Direct3D evidence, `.124`, 2026-11-12 (d3dplan_1112)

Box: `.124` (NSC-C543575F526), Voodoo 5 6000 on vcr-kmd, `vcrdd.dll` 95678704 (= `out/vcrdd.dll`),
`Diag\D3D32`=1, `Diag\D3DBigTex`=7, agent 1.100.1, uptime 23 h (boot_count 69). Box time used:
09:59-10:46 host time (47 min). Times below are host local (EST); the box clock reads about 2 min behind.

Narrative and findings: [`../reports/onbox.md`](../reports/onbox.md). Minute-by-minute notes:
[`session_log.md`](session_log.md).

## Tools written for this run (all in this folder)

| file | what |
|---|---|
| `agentcall.py` | one agent call per run (`cmd`, `shot`, `up`, `down`, `ping`); one connection, closed gracefully |
| `d3dprobe_paced.py` | `tools/d3dprobe_run.py` unchanged, plus `--pace 5000` handed to d3dprobe (5 s between its mode switches; `d3dprobe_run.py` has no pace option) |
| `gamerun.py` | uploads a wrapper `.bat` to `C:\vcr\onbox\`, `LAUNCH`es it, `SCREENSHOT 0` + `PROCLIST` + `WINLIST` at fixed times, waits for a self-exit, then console `exit` (only with the game focused), `WM_CLOSE` by pid, and a forced kill by pid only after 20 s; checks `DISPLAYCFG` afterwards; stops on 120 s of agent silence |

Files placed on the box: `C:\vcr\d3dprobe\d3dprobe.exe` + logs (by `d3dprobe_run.py`), `C:\vcr\onbox\*.bat`
(the game wrappers) and `C:\vcr\onbox\ut2004_statfps.txt` (`stat fps`). Nothing else was written by this
run; no registry, driver, Diag or reboot. The games wrote their own logs/benchmark files as they always do.

## Runs, in order

| # | time | command | result | files |
|---|---|---|---|---|
| 1 | 09:59 | `PING`, `SYSINFO`, `PROCLIST`, `DISPLAYCFG get`, `WINLIST` | alive; no game; screensaver `ssstars.scr` in front; 1280x1024x32@85 | `state/*_start.*` |
| 2 | 10:00 | `tools/vcrlog.py fetch 192.168.1.124 --save onbox/ring_start.txt` | 1024 entries, next_seq 24629; 513/16 + 513/18 (D3D32, D3DBigTex armed); a 4-chip SLI OpenGL/Glide session (pid 1892) at 75242-75345 s whose SLI went off by the kernel's display reset, not by Glide's own SLI/AA-off request (Glide's not-idle close path, which also skips the command-FIFO disable) | `ring_start.txt`, `ring_start_decoded.txt` |
| 3 | 10:00 | `UIKEY SHIFT` | screensaver dismissed (ScreenSaverIsSecure=0) | `state/desktop_start.png` |
| 4 | 10:01:28 | `d3dprobe_paced.py 192.168.1.124 render --full --res 640x480 --bpp 32 --zfmt d16 -v` | **CreateDevice 8876086c** (INVALIDCALL); ring 513/12 "ContextCreate refused: 32 bpp target with a 16-bit Z"; runtime switched 640x480x32@85 and back 156 ms later | `d3dprobe_render_640x480x32_d16.txt`, `ring_after_probe_i*.txt` |
| 5 | 10:02:20 | `... render --full --res 1024x768 --bpp 32 -v` (D24X8) | device + ContextCreate ok, clears pass, **every triangle missing: 9 pass / 31 fail** | `d3dprobe_render_1024x768x32.txt`, `ring_after_probe_ii*.txt` |
| 6 | 10:03:23 | `... render -v` (windowed, 32 bpp desktop, D24X8) | **7 / 35**; ring: `2D engine never went idle (status 00000a1f) - acceleration off for this PDEV` right after a texture landed at 0xc0000 | `d3dprobe_render_windowed_desktop32.txt`, `ring_after_probe_windowed*.txt` |
| 7 | 10:07:41 | `EXEC C:\vcr\vcrctl.exe reg <off>` x32 (chip 0, read-only) | status 0xa1f (SST_BUSY + SST_CMD0_BUSY); **cmdFifo0 ENABLED** (baseSize 0x1fe, base 0x18000, 255 pages), readPtr stuck at 0x2f624, aMin 0x2f630, holeCount 0x22177 | `regs/regs_after_windowed_1009.txt` |
| 8 | 10:11:03 | `... render --full --res 1024x768 --bpp 32 -v` | **GetDeviceCaps(HAL) 8876086a, CreateDevice 8876086a**: no D3D HAL at all on the desktop PDEV whose 2D was given up; no mode switch made | `d3dprobe_render_1024x768x32_after_reset.txt`, `ring_after_probe_ii_b*.txt` |
| 9 | 10:12:07 | `EXECW 120 C:\vcr\vcrctl.exe modeseq 6000 1024 768 32 85` | one paced switch + GDI test (0 mismatches) + restore; ring: `chip 0 not idle (0xa5f)` -> `engine reset (busy at mode set): idle again (0x5f)`; cmdFifo0 now off | `vcrctl_modeseq_recover_1012.txt`, `ring_after_modeseq*.txt`, `regs/regs_after_modeseq_1012.txt` |
| 10 | 10:12:32 | `... render --full --res 1024x768 --bpp 32 -v` (same as #5) | **40 / 0** | `d3dprobe_render_1024x768x32_clean.txt` |
| 11 | 10:12:57 | `... --full 1024x768x32 --timeout 240 --tests mip,mip2048,tex512,tex1024,tex2048,tex8888,tex8888_256,tex8888_512,tex8888_512x128,dxt1,dxt3,dxt5,dxt1_512,dxt5_512,dxt5_512x256,dxt5_512x128,dxt5_256x128` | **83 / 0**, 0 skipped | `d3dprobe_textures_a_1024x768x32.txt` |
| 12 | 10:13:17 | `... --full 1024x768x32 --timeout 300 --tests dxt1_decode,dxt3_decode,dxt5_decode,dxt1_decode1,dxt5_decode1,rgb565_decode,rgb565_decode1,dxt5_big_a,dxt5_big_b,dxt5_big_c,dxt5_512x128_def,dxt5_128_def,tex8888_512x128_def,dxt5_512x128_upd,tex8888_512x128_copy,texhigh,texhigh_dxt5` | **37 / 3**: every decode 0 wrong texels (RGB and alpha); the 3 fails are the `*_def` tests: `LockRect 8876086c` on a D3DPOOL_DEFAULT texture (runtime refusal) | `d3dprobe_textures_b_1024x768x32.txt` |
| 13 | 10:13:58 | `... perf --full --res 640x480 --bpp 16 --frames 300 -v` | 85.1 fps (vsync), 17029 tris/s | `d3dprobe_perf_640x480x16_vsync.txt` |
| 14 | 10:14:11 | `... perf --full --res 1024x768 --bpp 32 --frames 300 -v` | 84.4 fps (vsync), 16879 tris/s; ring: flips 300, retrace 284, deadline 15 | `d3dprobe_perf_1024x768x32_vsync.txt`, `ring_after_probes*.txt` (last 1024 only) |
| 15 | 10:16:33 | `gamerun.py --name UT2004_botmatch --dir C:\Games\UT2004\System --exe UT2004.exe --args "DM-Antalus?spectatoronly=true?numbots=8?quickstart=true?attractcam=true -benchmark -seconds=60" --shots 25,45,65,90 --wait-exit 150` | in-game at 65/90 s: sky right, terrain + rock meshes block noise, world very dark; did not end by itself; WM_CLOSE ignored; forced by pid at 242 s; log 0 bytes | `games/UT2004_botmatch/` |
| 16 | 10:23:26 | `gamerun.py --name UT2003_flyby --dir C:\Games\UT2003\Benchmark --exe ..\System\ut2003.exe --args "dm-antalus?game=engine.gameinfo exec=..\Benchmark\Stuff\flybyexec.txt -benchmark -seconds=77 -nosound" --shots 30,60,90,120 --wait-exit 150 --console-exit` | 640x480x16@120 (log); **avg 9.41 fps, min 2.07**, score 9.42; 123 ms/frame, 116.7 ms in DrawPrimitive; block noise, blue grass, white polygons, checkerboard, dark; console exit clean | `games/UT2003_flyby/` (avgfps log, stats CSV (gzip), UT2003.log, ring) |
| 17 | 10:29:53 | `gamerun.py --name Halo_timedemo --dir C:\Games\Halo --exe halo.exe --args "-novideo -vidmode 1280,960,85 -timedemo" --shots 30,60,90,120,180 --wait-exit 300` | cutscenes render largely right; a glow drawn as a translucent square; block noise on some rock faces; timedemo unfinished at 485 s (no timedemo.txt on C:); WM_CLOSE clean | `games/Halo_timedemo/`, `games/halo/fleetres_cmd.txt` |
| 18 | 10:39:37 | `... render --full --res 640x480 --bpp 16 -v` (R5G6B5 + D16) | **40 / 0** | `d3dprobe_render_640x480x16.txt`, `regs/regs_after_games_1039.txt` |
| 19 | 10:40:21 | `gamerun.py --name UT2004_statfps ... --args "DM-Antalus?spectatoronly=true?numbots=8?quickstart=true?attractcam=true -exec=C:\vcr\onbox\ut2004_statfps.txt" --shots 40,60,80,100 --console-exit` | 1280x960x16 (log, although Use16bit=False); **stat fps: 4 cur / 6 avg**; weapon/grass/HUD/text right; floor block noise; sky/horizon solid white; console exit clean | `games/UT2004_statfps/` |
| 20 | 10:43:24 | `... render -v` (windowed, 32 bpp desktop) | **42 / 0** (vs 7/35 in #6) | `d3dprobe_render_windowed_desktop32_clean.txt` |
| 21 | 10:44:08 | `tools/vcrlog.py fetch 192.168.1.124 --save onbox/ring_end.txt`; `vcrctl reg 0 / 80024`; `PROCLIST`; `DISPLAYCFG get`; `SCREENSHOT 0` | 1280x1024x32@85; nothing of this run left running; status 0x1f idle; cmdFifo0 off; no refusal in the ring's last 1024 entries | `ring_end*.txt`, `regs/regs_end.txt`, `state/*_end.*` |

`ring_*_decoded.txt` is `vcrlog.py`'s decode of the raw TSV beside it. The ring holds 1024 entries at
`LogLevel` 3: one game session overruns it many times (UT2004 alone ~67,000 events), so the per-run
`ring.txt` under `games/` keeps only that session's last 1024 entries (its `context N gone` summary).

Not reached / not done (and why) is in the report's "What was not done".
