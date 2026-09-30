# 2x AA (cfg 6), default recipe - froze at quit (2026-09-30 09:42)

Quake II at 1280x960x32 through our ICD 0.1.78 + our h5 Glide + vcr-kmd, the
kernel's DEFAULT (dos_mode.c) AA recipe. Three minutes of play were fine; the
box froze ~4 s after `quit` was typed with the console open. The trace's last
line is an ordinary per-frame LFB unlock - no close step ran. The frozen NIC
then flooded 802.3x PAUSE frames and took the whole wired LAN down
(docs/host-issues-log.md signature 7).

- `q2aa.trace.gz` - FX_GLIDE_TRACE=2, disk-flushed per line. `pig:` lines:
  `cfgAALfbCtrl` = 0x4C000000 on all chips (base 0 - the FIFO overwrite).
- `retrogl.log` - the ICD log (OS-cache only, so its last lines may be lost).
- `box_agent.log` - .124's agent log (batched: the last ~15 s before a freeze are lost).
- `diag_after.json`, `glide_after.json`, `phases_prev.txt`, `sysinfo.json` - after
  the power cycle: the AA auto-disarm had fired (`SliAAAutoOff` = 49).

Root cause and fix: `../aa_vendor_0930/README.md`.
