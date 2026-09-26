# evidence/86box_v3 - the 86Box Voodoo3 3000 test bed (2026-09-26)

`inbox_*`: XP's own Voodoo3 driver (3dfxvs2k.inf 5.1.2001.0) on the emulated
card - the reference. `vcrkmd_*`: our driver. `install*/`: deploy_box.py
evidence per install (recorder log, register snapshot, screenshot).

| file | what |
|---|---|
| inbox_ddlab_800x600x16.jsonl | ddlab caps/flip/blt, in-box driver |
| inbox_d3d_*.json | d3dprobe caps, render (windowed 26/26, fullscreen 26/26), perf (vsync-bound) |
| vcrkmd_ddlab_800x600x16_software_blt.jsonl | ours BEFORE the 2D engine and the flip fix: blt 16/s (1.0 Mpix/s, CPU over video memory), flip 768/s on a 60 Hz mode (no vsync) |
| vcrkmd_2d_16bpp.jsonl, vcrkmd_2d_32bpp.jsonl | ours AFTER: gdilab 0 bad; blt 0 bad incl. colour key, ~150 Mpix/s; flip vsync-bound |
| vcrkmd_d3d_caps_before.json | ours before the Direct3D HAL: GetDeviceCaps D3DERR_NOTAVAILABLE |
| vcrkmd_d3d_render_windowed.json, vcrkmd_d3d_render_full_800x600x16.json | ours with the Direct3D HAL: d3dprobe render 26/26 windowed and fullscreen |
| vcrkmd_battery_d3d1.jsonl | the whole battery on that build: gdilab 0 bad, ddlab blt/flip 0 bad, d3dprobe 26/26 x2 |
