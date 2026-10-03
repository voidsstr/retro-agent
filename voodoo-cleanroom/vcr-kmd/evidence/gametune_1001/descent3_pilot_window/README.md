# Descent 3: the pilot file sizes the 3D view (`.123`, 2026-10-02)

Summary and conclusions: `../README.md`, section "Descent 3".

| run | what | non-black area at 1600x1200 |
|---|---|---|
| `20261002_220232_123_timetest_1600x1200_user_pilot/` | the game's own `-timetest Secret2.dem`, user's unpatched pilot (window 1280x960) | full frame: the demo path does NOT use the pilot's window, so it cannot show the defect |
| `20261002_220637_123_join_A_user_pilot_unpatched/` | joined the dev host's `descent3-server` (`-pilot SDF -directip +connect 192.168.1.132`), the user's pilot as saved on `.124` (md5 6b8115ec) | **160,120-1440,1080** - a centred 1280x960 box |
| `20261002_220752_123_join_B_user_pilot_4096/` | the same join, that pilot with the window raised to 4096x4096 (md5 dd51fb83 = `sdf.plt.user_4096x4096` = what `provisioning/patches/descent3/apply.py` builds and published) | **0,0-1600,1200** - full screen, cockpit and HUD |

Each `run.json` has the box, the pilot md5, the mode and the bounding box of
all three captures (25/45/65 s); one frame per run is kept. Every run ended with
the agent's `PROCKILL` (Direct3D on `.123`, not a Glide session) and the
desktop read back at 1280x1024 @ 100 Hz.
