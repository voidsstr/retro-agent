@echo off
rem Hexen II - HOST a LAN deathmatch on this machine.
rem
rem Hexen II is Quake-engine and speaks TCP/IP NATIVELY - there is no IPX
rem here and no IPXWrapper is needed. -listen starts a listen server that
rem answers the LAN broadcast search, so the other boxes find it with
rem "Join Hexen II - LAN.bat" -> Search.
rem
rem This mirrors the disc's own H2ListenDM.bat, on the GL build.
rem
rem  VERIFIED ON HARDWARE 2026-08-31 (.123 hosting, .240 joining): the join
rem  browser lists this box and both players end up on demo1 together. The
rem  host answers the Hexen II LAN control query on UDP 26900 -- probe it with
rem  scripts/game-servers/nqquery.py <ip> 26900 HEXENII, NOT with Quake III's
rem  getstatus and NOT with Quake II's status, both of which are ignored and
rem  make a live host look dead.
rem
rem  -hostname WAS A NO-OP. Hexen II has no such command-line switch, so the
rem  server advertised the Windows machine name instead of a fleet name. It is
rem  a CVAR, so it is set with + not -.
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
start "" glh2.exe -width %FR_W% -height %FR_H% -heapsize 32768 -nocdaudio -listen 8 +hostname "NSC Retro Fleet Hexen II" +deathmatch 1 +map demo1
exit
