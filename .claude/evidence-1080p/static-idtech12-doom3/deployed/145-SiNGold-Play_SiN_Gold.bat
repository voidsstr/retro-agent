@echo off
rem SiN Gold - fleet launcher.
rem
rem This .bat exists because launch.txt used to point straight at the exe, and a
rem desktop shortcut cannot carry arguments - so there was nowhere to run
rem FLEETRES and the game was pinned to gl_mode 6 (1024x768) on every monitor.
rem
rem id Tech 2 has a FIXED mode table and no custom mode: 0=320x240 ... 6=1024x768
rem ... 9=1600x1200, with no 16:9 entry at all. A correctly proportioned 4:3 mode
rem is the honest best on a widescreen panel; stretching it is worse.
cd /d "%~dp0"

call "%~dp0FLEETRES.BAT"
> "%~dp0base\fleetres.cfg" echo // written by the launcher at every start - do not edit
>>"%~dp0base\fleetres.cfg" echo set gl_mode "%FR_Q2MODE%"
>>"%~dp0base\fleetres.cfg" echo set vid_fullscreen "1"

start "" sin.exe +set gl_mode %FR_Q2MODE%

exit
