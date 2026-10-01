# In-game AA in several games on the V5 6000 - 2026-09-30 evening (user at the box)

The kernel with the AA fix (vcr-kmd: `cfgAALfbCtrl` READ_EN on the master chip
pair, deployed 16:43, `9fefeec`), the vendor AA recipe as the default, our h5
Glide with the level-2 AA trace (`07c96fd9`, system32), agent 1.96.1.
`scripts/benchmarks/v56k_bench.py` drives each title's own timedemo and its
clean exit (Quake III `demo four` + `nextdemo quit`; CS 1.6 `cs16_bench.dem`;
UT99 F9 timedemo + F10 Exit), and records every DLL by md5 (`versions.json`,
`results.csv`). One AA config per clean boot: 4x on boot #58, 8x on boot #59.
The kernel's `Diag\SliAA` was armed by hand before each round.

| title | stack (ICD over Glide) | mode | AA | fps | user |
|---|---|---|---|---|---|
| Quake III 1.32c | `3dfxOGL.dll` "Mesa Glide v0.63" over our h5 Glide `07c96fd9` | 1024x768x32 | 4x | 33.8 | looked right |
| Quake III 1.32c | our `retrogl.dll` 0.1.76 over a game-local h5 Glide `2df2f969` (`allours`) | 1024x768x32 | 4x | 29.9 | looked right |
| Counter-Strike 1.6 | `3dfxOGL.dll` over our h5 Glide | 1024x768x32 | 4x | 31.0, 35.6 | looked right (re-run) |
| Unreal Tournament 436 | GlideDrv (Glide 2) | 1024x768x16 | 4x | 43.0, 43.0 | looked right (re-run) |
| Quake III 1.32c | `3dfxOGL.dll` over our h5 Glide | 1024x768x32 | **8x** | 15.5 | looked good |
| Unreal Tournament 436 | GlideDrv (Glide 2) | 1024x768x16 | **8x** | 25.0 | looked good |

UT99 at 8x needed two tries for a number, not for the driver: the runner pressed F10 a fixed
105 s after F9, which a 43 fps demo (4x) finishes in time but a 25 fps demo (8x) does not, so
only UE1's 3-frame toggle blip reached the log (`diag/*FAILED*.log`, the game exited cleanly
both times). `v56k_bench.py` UT99Bench now waits for the demo's own summary (cap 600 s).

Every run completed its timedemo and exited by itself - no freeze, no hang.
Unreal Tournament's GlideDrv is Glide 2.x and has no 32-bit framebuffer, so it
is measured at 16 bpp (the runner skips a 32-bit request rather than record a
16-bit render as 32-bit).

What this adds over the Quake II runs (`../aa_vendor_0930/`): the kernel fix
holds under three different OpenGL/Glide combinations - an older MesaFX ICD
over our current Glide, our own older ICD over an older game-local Glide, and
the Glide 2 path of a native Glide game.

Log files had `:` in their names (the runner's `title:api` id); renamed to `-`
so the repo checks out on Windows.
