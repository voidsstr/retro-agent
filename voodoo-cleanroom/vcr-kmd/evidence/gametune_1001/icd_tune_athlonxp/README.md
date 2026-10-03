# ICD `-mtune=athlon-xp` A/B on `.124` (Athlon XP 2 GHz), 2026-10-03

`build-mesafx-retail.sh` builds with `-march=pentium3` (the floor; `.124` has
no SSE2) and `-mtune=pentium4`. The question was whether scheduling for `.124`'s
own CPU buys anything. The two builds differ only in `-mtune` (`TUNE=` is now
overridable):
- **0.1.84**, `-mtune=pentium4`: `be518e43`, the deployed build.
- **0.1.85**, `-mtune=athlon-xp`: `a6c9608e`, an experiment build.

All runs are 4-chip, cfg 5, vsync off. Quake III is the `quake3:retrogl` lane
(game-local `retrogl.dll`); TSE uses the system ICD and its first auto-demo.

| run | 0.1.84 (P4 tune) | 0.1.85 (Athlon XP tune) |
|---|---|---|
| Quake III 1280x960x32 | 76.7, 76.8 | 76.8, 76.8 |
| Quake III 640x480x32 (CPU-bound) | 121.5, 121.3 | 122.2, 122.2 |
| Serious Sam TSE 640x480x32 (CPU-bound) | 52.2 | 52.3 |

**Not adopted.** The gain is +0.6% at most, only where the CPU is the limit, and
nothing at the 1280x960 the games are played at. A box-specific build is not
worth that. `.124` stays on 0.1.84; build number 0.1.85 was consumed by this
experiment (CHANGELOG).
