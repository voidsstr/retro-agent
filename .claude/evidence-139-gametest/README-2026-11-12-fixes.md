# .139 (Windows 10) game fixes - 2026-11-12

Installed: DirectPlay (dism), DirectX June 2010 redist, VC++ 2005 x86 + GFWL 3.5.92
(Halo 2), OpenAL 1.0 (SoF2 tree's own installer). Halo 2 then reached its title screen.

Fixed:
- **HalfLife1** (library, stage-fleetres `WON_HL_CAP`, commit 1e94f5d): WON hl.exe 1.1.0.8
  falls back to a 400x300 window above 1280x960 (r8sheet.png); all four shortcuts now
  fullscreen 1280x960 (hlfix_sheet.png), Esc reaches the menu (hl_esc.png).
- **SiN / Wages of SiN** (driver profile on the box): crash c0000409 + c0000005 at an ASCII
  address = GL_EXTENSIONS overflow. NVIDIA "Extension Limit (OGL)" 0x1B58 (the value NVIDIA's
  Quake2 profile uses, npi4c.png) set on the driver's "SiN: Episodes" profile, which owns
  sin.exe (npi12c.png). Fleetbook recipe #117.
- **Redneck Rampage - Route 66** (library `dosboxRR_r66.conf`): ROUTE66M.EXE runs SETUP.EXE
  first; ROUTE66.EXE starts the add-on (Redneck_Rampage_Route_66_fix.png).

Not fixed:
- Quake II family: main thread spins at 100% after the demo map loads (q2_qconsole*.log ends
  "CDAudio: drive not ready" / "S_Update_ : overflow"; cd_nocd, s_initsound 0, soft, other
  gl_modes and GL extension cvars changed nothing).
- Soldier of Fortune: launcher needs a disc mounter (.cue with audio); profile for sof.exe imported.
- Serious Sam host shortcuts: first-run info box, then the client stays black after
  "Command line connection: 127.0.0.1" while the dedicated server reports ALL OK (ss.log).
- RTCW SP, Tribes 2, AvP, Tiberian Sun, Jedi Knight x2; UE titles run windowed.
