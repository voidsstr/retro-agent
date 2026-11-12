@echo off
rem ------------------------------------------------------------------
rem Per-box OpenGL selection for the Quake III tree.  Called by every
rem "Play ....bat" in this directory, AFTER FLEETRES.BAT (which sets
rem FR_W / FR_H).  %1 = the mod's fleetres.cfg.  Sets FR_ENGINE.
rem
rem A 3dfx Voodoo 2 is Class=MEDIA.  It does not drive the desktop and
rem it registers NO OpenGL ICD, so the generic opengl32 path can never
rem select it - on .171 Quake III had been rendering on the box's
rem Intel 865G with the Voodoo 2 sitting idle.  The Voodoo 2 driver
rem ships a STANDALONE OpenGL, 3dfxvgl.dll, which the retail id engine
rem loads directly when r_glDriver names it.
rem
rem ioquake3 CANNOT be pointed at that card.  All three routes were
rem measured on .171 on 2026-09-01 and all three failed:
rem   - r_glDriver: ioquake3 dropped the cvar, it is ignored;
rem   - a game-local opengl32.dll: opengl32 is in XP's KnownDLLs, so
rem     the application directory never wins;
rem   - SDL_VIDEO_GL_DRIVER: this SDL.dll hardcodes "OPENGL32.DLL"
rem     and carries no such string.
rem So on a Voodoo box the engine is the retail quake3.exe.  That does
rem NOT cost multiplayer: both fleet Q3 servers run with
rem com_legacyprotocol 68, which retail 1.32c speaks.
rem
rem *** THE 800x600 CEILING IS LOAD-BEARING - DO NOT DROP IT. ***
rem A Voodoo 2 cannot scan out more than 800x600 (1024x768 only with a
rem matched pair in SLI, and this box currently starts one card).  Ask
rem it for more and it does not degrade, it takes the game down:
rem   ...setting mode 8: 1280 1024 FS
rem   ...creating GL context: succeeded
rem   ...making context current: failed
rem   ...failed hard        -> WARNING: could not set the given mode (8)
rem   -> retries with C:\WINDOWS\system32\opengl32.dll and dies there
rem      too: wglMakeCurrent( NULL, NULL ): failed, releasing DC:
rem      failed, Q_strncpyz: NULL src
rem Measured on .171 2026-09-01 with r_mode 8.  Q3's own fallback to
rem the 2D chip is broken on this box, so there is no soft landing.
rem
rem Hence: the Voodoo is selected ONLY when the mode this box is
rem actually going to ask for fits inside the card.  Above that we
rem leave the box on its ordinary GL (the Intel 865G here), which
rem drives 1280x1024 perfectly well - verified, MODE: 8, 1280 x 1024
rem fullscreen, GL_RENDERER: Intel 865G.
rem
rem That makes the box's own per-box ceiling the single switch:
rem   HKLM\Software\RetroAgent  ResCapW/ResCapH = 800x600 -> Voodoo 2
rem   raise or remove them                               -> Intel, big
rem
rem NOTE this cannot police the in-game VIDEO menu.  Picking 1280x1024
rem there still kills that session - the menu is not something a
rem launcher can reach.  It is self-healing: the next launch rewrites
rem fleetres.cfg, which is exec'd last and beats the saved q3config.
rem
rem 3dfxvgl.dll exists only where the Voodoo 2 driver is installed, so
rem every other machine in the fleet takes the ioquake3 path unchanged.
rem ------------------------------------------------------------------
set "FR_ENGINE=ioquake3.x86.exe"
if not exist "%SystemRoot%\system32\3dfxvgl.dll" goto :eof

rem No measured panel size means no informed decision - stay on the
rem ordinary GL rather than guess the card can take it.
if not defined FR_W goto :eof
if not defined FR_H goto :eof
if %FR_W% GTR 800 goto :toobig
if %FR_H% GTR 600 goto :toobig

set "FR_ENGINE=quake3.exe"
>>%1 echo seta r_glDriver "3dfxvgl"
>>%1 echo seta r_colorbits "16"
>>%1 echo seta r_texturebits "16"
>>%1 echo seta r_depthbits "16"
>>%1 echo seta r_stencilbits "0"
>>%1 echo seta r_ext_compressed_textures "0"
goto :eof

:toobig
rem %FR_W%x%FR_H% is past what the Voodoo 2 can scan out - this box
rem runs Quake III on its 2D chip at that size instead of crashing.
echo Quake III: %FR_W%x%FR_H% exceeds the Voodoo 2's 800x600 ceiling -
echo using this machine's ordinary OpenGL instead. Set ResCapW/ResCapH
echo under HKLM\Software\RetroAgent to 800x600 to use the Voodoo 2.
