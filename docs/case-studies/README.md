# Case studies

Real-world diagnostic walkthroughs. Each one gives the symptom, the evidence,
the root cause (or an honest "not proven"), and what to do next time.

| case study | box | subject |
|---|---|---|
| [001 — Voodoo 3 ghost devices](001-voodoo3-ghost-device-diagnosis.md) | Win98 | Ghost PCI devices, remote driver install, display configuration |
| [002 — GeForce2 GTS vcache](002-geforce2-gts-vcache-driver-install.md) | Win98 | vcache "Windows Protection Error" with >512 MB RAM; NVIDIA driver install |
| [003 — EPoX nForce2 STOP 0x7B](003-epox-nforce2-xp-pxe-0x7b.md) | XP (PXE) | XP installs cleanly and will not boot; the IDE driver binding in TXTSETUP.SIF |
| [003 — Red Alert 2 on a Pentium III](003-red-alert-2-pentium3-sse2-cnc-ddraw.md) | XP | Black screen from an SSE2-only DirectDraw wrapper; the minimize-to-black bug |
| [2026-09 — ADMIN-PC Win7 "crashes"](2026-09-win7-admin-pc-freezes.md) | Win7 | Two display TDRs (0x117 live dumps) ended by the power button; a wedged agent; what is proven and what is not |

The two files numbered 003 are separate studies; the duplicate number is historical.
New studies use a `YYYY-MM-<subject>.md` name so that numbers do not collide
across worktrees.
