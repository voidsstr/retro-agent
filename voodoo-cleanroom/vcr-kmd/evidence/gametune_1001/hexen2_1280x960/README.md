# Hexen II at 1280x960 on `.124` (2026-10-02)

The share's Hexen II launchers call FLEETRES with `-cap 1024 768`, because on
`.240` (ATI X800) `glh2.exe` refused 1920x1080, 1280x1024 and 1280x960 with
"Specified video mode not available". The mode list is the driver's. On `.124`
(our vcr-kmd + ICD), started without the cap, `glh2.exe -width 1280 -height
960` came up through our ICD: `fxBestResolution: request 1280x960 -> res enum
17`, `SUCCESS ... screen=1280x960 colDepth=32`. `vcrctl info` read
1280x960x32@85 in 4-chip SLI. It showed no dialog, rendered its menu
(`menu_fbshot.png`, the scanned-out frame, captured after the probe keys had
opened the Single Player menu), and quit cleanly through its own menu (ESC, UP
to Quit, RETURN, Y).

`stage-fleetres.py` `h2_uncap` (marker `H2_UNCAP`) re-runs FLEETRES without the
cap where the 3dfx card drives the screen. After a sync `.124`'s Play, Host and
Join launchers build `-width 1280 -height 960`. The same library launcher run on
`.123` still builds `-width 1024 -height 768`.
