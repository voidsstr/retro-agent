# AA wedge post-mortem, read back 2026-09-27 00:15 (read-only)

Read from `.124` after the user power-cycled it following the third AA wedge
(cfg 1, 2026-09-26 ~16:39 host time). `.124` had booted exactly once since, so
the miniport's `Prev*` phase history still held boot #18 - the cfg 1 boot.
Nothing was run on the box: `vcrphases.py` (REGREAD of `Services\vcrmp\Diag`),
`DOWNLOAD` of the glidelab step logs, and REGREADs.

| file | what |
|---|---|
| `phases_prev.txt` | boot #18, the cfg 1 run: `HWC_SLIAA a=4 b=0x102` (4 chips, AA on, SLI off - the malformed tuple), `SET_BEGIN` 115.750 s, `CLOCK_6K`, slave `INIT_BEGIN` x3, mode index writes, `PCIINIT0` x4, **`SET_DONE` 116.328 s, then nothing**. No `SLICTRL` steps (cfg 3's boot #16 had four). |
| `phases_cur.txt` | boot #19 (this one): boot to the desktop mode, no Glide. |
| `onbox_fill.log` | glidelab's flushed step log of the cfg 1 run: last line `step: grSstWinOpen 1024x768 60Hz origin upper`, no `-> context` - the box froze **inside Glide's open, after the kernel reported SET_DONE**. |
| `onbox_bands.log` | the cfg 3 read-back run (boot #16): opened on 4 chips, froze at `grLfbReadRegion back buffer 1024x768`. |

What this settles: cfg 1 reached `SET_DONE` on our kernel and hung in
`grSstWinOpen` (previously asserted but never read back). What it cannot
settle: the kernel's warn mask (W_NOMUX) - the persisted `SET_DONE` phase drops
the value (`miniport/vcrmp_multi.c` k_log); fixed separately.

Armed state at read-back: `Services\3dfxvs\Device0\glide`
`SSTH3_SLI_AA_CONFIGURATION = 5` (SLI, no AA), `FX_GLIDE_REFRESH = 75`,
`FX_GLIDE_ANALOG_SLI = 1`, `FX_GLIDE_BPP = 16`. So an unattended Glide app
opens at cfg 5, not an AA config. `C:\WINDOWS\Minidump` holds only two 2003
dumps - no bugcheck from any wedge (they are hard freezes, not crashes).
