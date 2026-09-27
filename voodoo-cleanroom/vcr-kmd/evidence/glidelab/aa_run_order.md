# AA runs on `.124`, 2026-09-26: run order, boots, and what each result can support

**Source:** session transcript `dcb2b22d-469b-4351-ba8b-e191bd3007a8.jsonl`
(`~/.claude/projects/-home-voidsstr-development-retro-agent/`). Record numbers are
0-based JSONL lines. The records used are 17863, 17870, 17959-17960 (boot #16
before AA), 18021, 18029, 18036, 18053, 18061, 18063-18064 (cfg 3 bands),
18068-18069 (cfg 3 fill), 18073, 18079 (cfg 7, then the refused cfg 1 attempt),
18093, 18095-18096, 18100-18101 (read-back after cfg 7), 18105, 18111 (cfg 1),
18114 (the summary written at the time), 18127 and 18394 (later contact).
Host times are EDT, the dev host's local time; the transcript itself stores UTC
(`Z`, EDT+4). All runs used `tools/glidelab_run.py 192.168.1.124 <mode> --cfg N
--res 1024x768 --refresh 60` with our h5 Glide
(`C:\Games\Quake2Complete\glide3x.dll`) on our kernel driver (vcr-kmd).

## In plain words

1. **cfg 7 ran in the SAME boot (#17) as a cfg 3 AA session.** It started 20 s
   after that session returned (16:24:45 -> 16:25:05), with no reboot between.
   That breaks ground rule 1, "one driver setting per clean boot"
   (`docs/v56k-benchmark-plan.md` §0), so **the cfg 7 wedge is confounded**. It
   may come from cfg 7 itself, or from whatever the cfg 3 session and its
   close-time disable left on the card. The disable (`miniport/vcrmp_sli.c`
   `sli_disable` / `sli_disable_video`) clears sliCtrl, snoop/swap, the
   LFB/AA/video control registers and the slaves' DAC and video processor. It
   does **not** restore pciInit0, re-initialise the slaves, or reprogram the
   6000 clock. Whether that disable even reached `OFF_DONE` is **unrecorded**:
   the rows that would show it were cut by `tail -14` (rec 18095), and boot
   #17's `Prev*` history has since been overwritten.
2. **cfg 3 bands wedged 2.64 h (9507.8 s) into boot #16, and boot #16 was busy.**
   It was the install6 deploy reboot (~13:05). It then ran the DirectDraw/D3D
   battery with 16 and 32 bpp mode switches (13:07:43-13:08:50,
   `evidence/silicon/vcrkmd-v5-6.log`) and a hardware-cursor register read with
   clicks (13:09:44). After that the box sat at the desktop for ~2.5 h with no
   traffic from this session before the AA run. So this run is not a clean-boot
   observation either.
3. **cfg 1 is the only clean-boot observation of the three wedges.** It was the
   first Glide application of boot #18, started 54 s after the agent came back
   from a power cycle. Only read-only commands ran before it (PING,
   REGREAD-based `vcrphases`, DIRLIST/DOWNLOAD). Its kernel phase log and step
   log are in `postmortem_20260927/`, read back on 2026-09-27; they are not
   repeated here. (The cfg 3 *fill* run that PASSED was also the first Glide app
   of its boot, #17. So the pass is a clean observation; the wedge that
   followed it in the same boot is not.)
4. **"625 Mpix/s" does not by itself prove AA was active.** See the section
   below. What is supported is only that **cfg 3 fill ran 200 frames without a
   wedge**.

## Timeline

| # | host time (EDT) | boot | run | what ran earlier in that boot | result | kernel phases | glidelab step log | host log | records |
|---|---|---|---|---|---|---|---|---|---|
| - | 13:04:11-13:09:44 | #16 | install6 reboot, DDraw/D3D battery, cursor read | (boot start) | ok | rows 35-39 only | - | `evidence/silicon/install6.log`, `vcrkmd-v5-6.log` | 17863, 17870, 17959-17960 |
| 1 | 15:43:37 (host EXECW gave up 15:48:18) | #16 | **bands cfg 3** | the battery above, then ~2.5 h idle | **WEDGE**: user saw a blank screen and dead Num Lock (reported 16:19:16) | [`aa_cfg3_prev.txt`](aa_cfg3_prev.txt): HWC_SLIAA a=4 b=0x103 at 9507.781 s, SET_DONE at 9508.437 s | `postmortem_20260927/onbox_bands.log`, ends `grLfbReadRegion back buffer` | `aa_cfg3.log` | 18021, 18029, 18036, 18064 |
| - | ~16:21-16:22:59 | #17 | power cycle ("done" 16:22:55); agent up 16:22:59; `vcrphases --prev` read 16:23:02 | - | - | - | - | - | 18053, 18061, 18063 |
| 2 | 16:24:35-16:24:45 | #17 | **fill cfg 3** | PING + REGREADs only (first Glide app of the boot) | rc=0, 200/200 frames, 198.66 fps, 624.9 Mpix/s, `"chips": 4` | not captured (cut from rec 18096) | overwritten by run 3 before anyone read it | `aa_cfg3_fill.log` | 18068-18069 |
| 3 | 16:25:05 (host gave up; next line 16:29:54) | **#17, same boot** | **fill cfg 7** | **run 2, a cfg 3 AA session, 20 s earlier** | **WEDGE** | [`aa_cfg7_prev.txt`](aa_cfg7_prev.txt): rows 70-82 only, SET_DONE at 228.859 s, no SLICTRL | [`aa_onbox_steplogs.txt`](aa_onbox_steplogs.txt): ends `grSstWinOpen`, no `-> context` | `aa_cfg7_fill.log` | 18073, 18079, 18096, 18101 |
| - | 16:29:54 | (#17, wedged) | fill cfg 1, second iteration of the same loop | - | **rc=2, no session.** glidelab_run's refusal exit, taken before its `plan:` line, because the box was unreachable. No Glide ran. This attempt's log was overwritten by run 4. | - | - | (overwritten) | 18079 |
| - | ~16:37-16:38:35 | #18 | power cycle; agent up 16:38:22; read-only reads 16:38:24-16:38:35 | - | - | - | - | - | 18093, 18095-18101 |
| 4 | 16:39:16 (host gave up; result logged 16:43:57) | #18 | **fill cfg 1** | read-only commands only (first Glide app of the boot) | **WEDGE** | `postmortem_20260927/phases_prev.txt`: HWC_SLIAA a=4 b=0x102 at 115.687 s, SET_DONE at 116.328 s, no SLICTRL | `postmortem_20260927/onbox_fill.log`: ends `grSstWinOpen` | `aa_cfg1_fill.log` | 18105, 18111 |
| - | 16:46:50 | #19 | power cycle; agent answered PING | - | - | - | - | - | 18127 |

After rec 18127 this session sent **no agent command** to `.124` until the
read-only post-mortem of 2026-09-27 00:13 (`postmortem_20260927/`). The only
other contact was one bare TCP connect to `:9898` at 23:27:56 (rec 18394), a
reachability probe that sent no protocol traffic.

## Why 624.9 Mpix/s does not prove AA

- `mpix_s` is `width x height x layers x frames / seconds` of **output pixels**
  (`tools/glidelab.c` `do_fill`). It does not count samples.
- The same fill reached 1124.6 Mpix/s on 4 chips without AA (cfg 2/5). Two
  different states would both land near half of that: 4 chips doing 2-sample
  AA, and 2 SLI units with no AA at all. 624.9 is 0.56 of 1124.6, and nothing
  in the number says which of the two produced it.
- `"chips": 4` is Glide's own `grGet(GR_NUM_FB)` answer (`chips_in_use()`), and
  `"cfg": 3` is the value glidelab was *told*. Neither is a hardware readback.
- The kernel's record of what Glide requested for this run (HWC_SLIAA flags)
  sits in the boot #17 rows that `tail -14` cut. For the *bands* run in boot
  #16, Glide did request `sliEn=1, aaEn=1` (b=0x103). A request is still not
  proof that samples were produced.
- **No frame was captured or compared.** Ground rule 2 of the plan: a setting is
  applied only when the rendering changes; a readback is not evidence.

## Claims written at the time that this record corrects or qualifies

- The summary in rec 18114 says the kernel AA setup "completes" for all three
  configs and that cfg 3 "renders fine, 625 Mpix/s". The AA table in
  `docs/v56k-benchmark-plan.md` (lines 109-114, as of 545c0b1) says "SET_DONE"
  for all three and "fill RENDERS (625 Mpix/s)" for cfg 3.
  - **cfg 1's SET_DONE had not been read back** when those were written. It was
    confirmed only on 2026-09-27 (`postmortem_20260927/`).
  - **cfg 7's result is confounded** (point 1).
  - "Renders" is unsupported (point 4).
- The plan table's evidence pointer (`aa_cfg*.log`) names host-side EXECW
  timeouts only. The kernel phases and the on-box step logs are in the files
  listed here and in `postmortem_20260927/`.

## For the next supervised AA run (only with the user at the box)

- **Save the whole `vcrphases.py --prev` output to a file under `evidence/`
  first, never through `tail`.** The tails used here lost rows 0-34 of boot #16
  and rows 0-69 of boot #17. `Prev*` holds one boot, so there is no second
  chance.
- **One AA config per power-cycled boot, as the first Glide app of that boot.**
  A cfg 7 result counts only if it comes from such a boot.
- **Download glidelab's step log before the next run of the same mode.** `fill.log`
  is overwritten each time, which is how cfg 3 fill's step log was lost.
- The persisted `SET_DONE` phase drops the kernel's warn mask
  (`miniport/vcrmp_multi.c` `k_log`), so W_NOMUX cannot be read after a wedge
  until that is fixed.
