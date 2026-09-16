# FSAA evidence — Voodoo 5 6000 + AmigaMerlin 3.1-R11 on .124 (2026-09-15)

Requesting FSAA changes nothing, on **either** rendering path. These are the
frames behind that claim. Every capture is the ENGINE'S OWN screenshot: the
agent's GDI `SCREENSHOT` of a Glide exclusive-fullscreen surface returns dark
noise (verified here), so a GDI capture could not be used to judge edges.

## OpenGL path — Quake III, retail 1.32c, `demo four`, fixed frame

| file | requested |
|---|---|
| `quake3_1024x768_cfg5_4chip-noaa.png` | no AA |
| `quake3_1024x768_cfg7_4chip-4xaa.png` | 4x AA |
| `quake3_1024x768_cfg7_4xaa_plus-aa-sample.png` | 4x AA + `FX_GLIDE_AA_SAMPLE=4` |

All three are **byte-identical** — md5 `6361acdebeb7dec0e2396aa436a9a18a`, zero
pixel difference. Three settings, one image. The frame is deterministic, which
is exactly why identical images prove identical rendering.

## Native Glide path — UT99 436, `GlideDrv.GlideRenderDevice`, 1024x768

| file | requested | FRAME | RENDER |
|---|---|--:|--:|
| `ut99-glide_1024x768_cfg5_noaa.png` | no AA | 6.9 ms | 5.4 ms |
| `ut99-glide_1024x768_cfg8_8xaa.png` | **8x AA** | 6.6 ms | **5.1 ms** |

Read off UT's own `stat fps` overlay in the frames themselves
(`*_statline.png`). **8x AA renders faster than no AA**, and
`ut99-glide_cfg8_8xaa_edge-zoom.png` shows why: the polygon edges are hard
staircases with no intermediate shading.

The quantified edge statistic comes from a DIFFERENT capture with a cleaner
two-tone boundary: `ut99-glide_1024x768_cfg4_4xaa.png` (the driver's "Dual
Chip, 4-Sample AA" setting requested), terrain horizon against the starfield,
crop (300,430)-(700,510). `edge_stats.py` on that crop: **78% of edge columns
carry no blended pixel** (257 of 328). The same metric on a textured indoor
crop of the cfg 8 frame is not meaningful (texture noise reads as blending),
which is why the cfg 8 evidence is the frame time and the zoom, not a
percentage. Method: `python3 edge_stats.py <png> 300 430 700 510`.

The two UT frames are different spawn points — UT's `?quickstart=true` picks a
random PlayerStart — so they are NOT a pixel-diff pair. Edge quality is
scene-independent, which is what makes them comparable; a 74% pixel difference
between two such frames is scene, not AA, and was nearly misreported as one.
