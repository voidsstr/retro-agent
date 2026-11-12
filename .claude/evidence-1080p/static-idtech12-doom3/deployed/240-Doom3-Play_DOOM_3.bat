@echo off
rem DOOM 3 - fleet launcher.
rem
rem This .bat exists because a desktop shortcut cannot carry arguments
rem (gamesync.c never calls SetArguments), so there is nowhere else to run
rem FLEETRES and the game would come up at whatever DoomConfig.cfg last held on
rem whichever box wrote it.
rem
rem DOOM 3 takes its resolution from the COMMAND LINE, not from a staged cfg:
rem id Tech 4 re-applies +set startup variables AFTER exec'ing DoomConfig.cfg.
rem That is the opposite of every id Tech 3 title in this library.
cd /d "%~dp0"

call "%~dp0FLEETRES.BAT"

rem r_aspectRatio (0=4:3, 1=16:9, 2=16:10) from the panel FLEETRES measured.
rem Integer cross-multiply so there is no rounding: 1920*9 == 1080*16 is 16:9.
rem Anything that is neither stays 0, which is the safe answer for a 4:3 tube.
set FR_D3AR=0
set /a _FRA=%FR_W%*9
set /a _FRB=%FR_H%*16
if "%_FRA%"=="%_FRB%" set FR_D3AR=1
set /a _FRA=%FR_W%*10
set /a _FRB=%FR_H%*16
if "%_FRA%"=="%_FRB%" set FR_D3AR=2

start "" DOOM3.exe +set r_mode -1 +set r_customWidth %FR_W% +set r_customHeight %FR_H% +set r_aspectRatio %FR_D3AR% +set r_fullscreen 1

rem Close this console - a launcher that lingers stacks a window per launch and
rem makes every later screenshot ambiguous.
exit
