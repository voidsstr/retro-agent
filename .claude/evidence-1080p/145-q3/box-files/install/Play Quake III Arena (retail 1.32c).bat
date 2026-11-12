@echo off
rem The retail id binary, 1.32c, kept alongside ioquake3 as an A/B: on a box
rem whose OpenGL driver upsets ioquake3 this one sometimes still runs, and
rem vice versa. Reads baseq3\q3key.
rem
rem *** SINGLE-PLAYER AND A/B ONLY - THIS BINARY CANNOT JOIN THE FLEET Q3
rem SERVER. *** Retail 1.32c speaks network protocol 68; the server on
rem 192.168.1.132:27961 is ioquake3 at com_protocol 71, and the two do not
rem interoperate. Use "Play Quake III Arena.bat" (ioquake3.x86.exe) for
rem anything multiplayer. Measured on the fleet, not assumed.
cd /d "%~dp0"
start "" quake3.exe

rem Close this console. A .bat that ends at "start" leaves its window open for
rem the life of the game, and after a dozen launches box-145 had SIXTEEN of
rem them stacked on the desktop - which actively corrupted testing, because a
rem screenshot then showed a cluttered desktop and you could not tell "the
rem game is fullscreen and GDI cannot see it" from "the game never started".
exit
