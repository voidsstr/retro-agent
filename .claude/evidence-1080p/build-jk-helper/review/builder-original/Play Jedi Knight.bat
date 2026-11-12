@echo off
rem Jedi Knight - Dark Forces II - fleet launcher.
rem
rem This .bat exists because launch.txt pointed straight at JK.EXE, and a
rem desktop shortcut cannot carry arguments - so nothing could give the game
rem this box's resolution and every box started at 640x480 8-bit software.
rem
rem The Sith engine stores its mode as an INDEX into a DirectDraw mode list it
rem builds itself - 64 entries at most - not as a width and height, so no
rem staged value can be right: the index differs per box, per driver and per
rem game. JKMODE.EXE repeats the game's own enumeration and writes the index
rem of FR_W x FR_H at 16 bpp, b3DAccel=1 and the two device GUIDs the game
rem would store, then reads them back. Where the game cannot select that mode
rem - an X800's list reaches 1920x1080 only at entry 76 - it REFUSES and the
rem game keeps what it has stored; the one thing it then removes is a stored
rem index past the END of the list, which the game would read beyond. Its
rem report is JKMODE.LOG.
cd /d "%~dp0"

call "%~dp0FLEETRES.BAT"

if exist "%~dp0JKMODE.EXE" "%~dp0JKMODE.EXE" df2 %FR_W% %FR_H% > "%~dp0JKMODE.LOG" 2>&1

start "" JK.EXE

rem Close this console - a launcher that lingers stacks a window per launch and
rem makes every later screenshot ambiguous.
exit
