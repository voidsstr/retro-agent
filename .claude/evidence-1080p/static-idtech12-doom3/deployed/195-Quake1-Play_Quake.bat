@echo off
rem Quake - fleet launcher (GLQuake).
rem
rem This .bat exists because launch.txt used to point straight at GLQUAKE.EXE,
rem and a desktop shortcut cannot carry arguments - so there was nowhere to set
rem a resolution and every box got GLQuake's own 640x480 default.
rem
rem CAPPED AT 1280x960, AND THAT CEILING IS MEASURED, NOT ASSUMED. On .145
rem (GeForce 8400GS, 1920x1080 panel) GLQUAKE.EXE answers
rem     Quake Error: "Specified video mode not available"
rem at BOTH 1920x1080 and 1600x1200, and comes up fullscreen at 1280x960. Its
rem mode list is a fixed 4:3 table compiled into the binary, so there is no
rem 16:9 entry to ask for and no custom mode. 1280x960 pillarboxed on a 1080p
rem panel is the honest best this engine can do.
cd /d "%~dp0"

call "%~dp0FLEETRES.BAT" -cap 1280 960

start "" GLQUAKE.EXE -width %FR_W43% -height %FR_H43% -bpp 32

exit
