# Shogo CShell.dll mode-list patch - BLOCKED (draft only)

`cave.S` is an UNBUILT, UNREVIEWED draft (2026-09-29, lcd1080 lane). CShell.dll
v3 inside SHOGOP3.REZ keeps 12 resolution slots per render device and appends
modes ascending, so 1920x1080 is truncated off the default device and appears
only under a second device whose name is corrupted ("(d3d.ren) Primary X").

The review blocked it: no apply.py, no tests, and two static checks not done
(no relocation inside the detour/cave range; no branch into the middle of the
patched bytes). Nothing from this directory may be published. See
.claude/evidence-1080p/_results/build/review-shogo.json for the full findings.
