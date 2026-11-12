@echo off
rem Fleet launcher. cd first so quake2.exe finds baseq2 wherever the tree lands.
call "%~dp0FLEETRES.BAT"
> "%~dp0baseq2\fleetres.cfg" echo // written by the launcher at every start - do not edit
>>"%~dp0baseq2\fleetres.cfg" echo set gl_mode "%FR_Q2MODE%"
>>"%~dp0baseq2\fleetres.cfg" echo set vid_fullscreen "1"

cd /d "%~dp0"
start "" quake2.exe +set gl_mode %FR_Q2MODE%

rem Close this console. A .bat that ends at "start" leaves its window open for
rem the life of the game, and after a dozen launches box-145 had SIXTEEN of
rem them stacked on the desktop - which actively corrupted testing, because a
rem screenshot then showed a cluttered desktop and you could not tell "the
rem game is fullscreen and GDI cannot see it" from "the game never started".
exit
