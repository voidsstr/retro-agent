# 2x AA (cfg 6), default recipe - froze at the 3dfx splash (2026-09-30 11:59)

A clean boot, ICD 0.1.79 (RETROGL_SYNCTRACE=1) + FX_GLIDE_TRACE=2 in one
disk-flushed file (`q2aa2.trace`). The box froze ~20 s after launch; the last
line is `splash: grSplash` - the splash plugin draws with LFB writes, and the
first one never finished. Monitor black / no signal (user). The LAN stayed up:
.124's NIC had been set to FlowControl = 2 (Respond) that morning.
`SliAAAutoOff` = 52 after the power cycle. Root cause and fix:
`../aa_vendor_0930/README.md`.
