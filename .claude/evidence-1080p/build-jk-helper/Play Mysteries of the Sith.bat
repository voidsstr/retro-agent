@echo off
rem Jedi Knight - Mysteries of the Sith - fleet launcher.
rem
rem This .bat exists because launch.txt pointed straight at JKM.EXE, and a
rem desktop shortcut cannot carry arguments - so nothing could give the game
rem this box's resolution and every box started at 640x480 8-bit software.
rem
rem The Sith engine stores its mode as an INDEX into a DirectDraw mode list it
rem builds itself - 64 entries at most - not as a width and height, so no
rem staged value can be right: the index differs per box, per driver and per
rem game. JKMODE.EXE repeats the game's own enumeration, cap and sort and
rem writes the index of FR_W x FR_H at 16 bpp, b3DAccel=1 and the two device
rem GUIDs the game would store, then reads them back. Where the game cannot
rem select that mode - an X800 enumerates 1920x1080 only 77th, past the 64
rem modes the game keeps - it REFUSES, and the game keeps what it has stored;
rem the one thing it then removes is a stored index past the END of the list,
rem which the game would read beyond. Its report is JKMODE.LOG.
cd /d "%~dp0"

call "%~dp0FLEETRES.BAT"

if exist "%~dp0JKMODE.EXE" "%~dp0JKMODE.EXE" mots %FR_W% %FR_H% > "%~dp0JKMODE.LOG" 2>&1

start "" JKM.EXE

rem Close this console - a launcher that lingers stacks a window per launch and
rem makes every later screenshot ambiguous.
exit
