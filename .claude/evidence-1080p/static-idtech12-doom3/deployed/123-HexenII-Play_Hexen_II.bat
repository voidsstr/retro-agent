@echo off
rem Hexen II - fleet launcher. OpenGL renderer, FULLSCREEN.
rem
rem glh2.exe is the disc's own GL build (Glroot\glh2.exe on the CD). It is
rem staged in the ROOT rather than in Glroot\ ON PURPOSE: it links OPENGL32.dll
rem by name, so a copy of the disc's Glroot\opengl32.dll sitting beside it
rem would shadow the system ICD. That file is the 1997 3Dfx MiniGL - it is
rem kept for reference in Glroot-3dfx-minigl\ and DELIBERATELY NOT in the root:
rem there it would take the GL renderer away from every box WITHOUT a 3dfx
rem card. A box WITH a Voodoo gets its own launcher, Play Hexen II - Voodoo.bat
rem (gated on the agent's glide capability), running glh2 beside a MiniGL.
rem
rem Fullscreen is glh2's default; -window is what makes it windowed. To get a
rem GDI-capturable frame for debugging, run:
rem   glh2.exe -window -width 800 -height 600
rem In-game F11 writes a real screenshot (data1\shots\hexen00.pcx).
rem
rem -nocdaudio: the music is REDBOOK AUDIO on tracks 2-17 of the retail disc,
rem not files in this tree, so there is no music without the disc mounted.
rem Left explicit so the engine does not stall probing for a CD player on the
rem boxes that have no optical drive at all.
rem
rem  CAPPED AT 1024x768, AND THAT CEILING IS MEASURED, NOT ASSUMED.
rem  Measured on .240 (1920x1080 panel), 2026-08-31: glh2.exe answers
rem      Hexen II Error: Specified video mode not available
rem  at 1920x1080, at 1280x1024 AND at 1280x960, and starts at 1024x768.
rem  Like GLQuake, this engine has a small fixed mode table compiled in -
rem  there is no 16:9 entry to ask for and no custom mode - so the launchers
rem  must ASK FOR A MODE IT HAS rather than the desktop's.
rem
rem  THIS IS WHY IT MATTERS HERE MORE THAN IN A SINGLE-PLAYER LAUNCHER: the
rem  engine dies at the video-mode check BEFORE it ever loads the map, so the
rem  listen server never opens UDP 26900 and the other boxes' Search comes up
rem  empty. glh2.exe is still in the process list the whole time, so from a
rem  PROCLIST the host looks perfectly healthy - exactly the shape of failure
rem  this project keeps paying for. Probe the port, not the process:
rem      python3 scripts/game-servers/nqquery.py <ip> 26900 HEXENII
rem
call "%~dp0FLEETRES.BAT" -cap 1024 768

cd /d "%~dp0"
start "" glh2.exe -width %FR_W% -height %FR_H% -heapsize 32768 -nocdaudio
rem Close this console - a .bat that ends at "start" leaves its window open
rem for the life of the game and they stack up across launches.
exit
