@echo off
rem ---------------------------------------------------------------
rem PLAY.BAT - the one command to run in MS-DOS mode.
rem
rem NETUP.BAT brings networking up (so games can be downloaded from
rem the share) and then we enter the game menu. Games already on the
rem hard drive play fine even with no network, so a failed detect is
rem never fatal.
rem
rem Every step is logged to C:\DOSGAME\DOSGAME.LOG. This file used to
rem be silent, so when PLAY failed on the Win98 box there was no
rem evidence whatsoever: the menu's own log was empty because the
rem menu had never been reached, and nothing recorded how far the
rem batch got. If it dies now, the log says exactly where.
rem ---------------------------------------------------------------
SET DGLOG=C:\DOSGAME\DOSGAME.LOG
echo play:   ---- PLAY.BAT starting ---- >> %DGLOG%

if exist C:\DOSGAME\DOSGAME.EXE goto haveexe
echo DOSGAME.EXE is missing from C:\DOSGAME - re-stage the DOS tools.
echo play:   FAILED - C:\DOSGAME\DOSGAME.EXE is missing >> %DGLOG%
goto done

:haveexe
if exist C:\DOSGAME\NETUP.BAT goto havenet
echo play:   NETUP.BAT missing - skipping network bring-up >> %DGLOG%
goto menu

:havenet
echo play:   calling NETUP.BAT >> %DGLOG%
call C:\DOSGAME\NETUP.BAT
echo play:   back from NETUP.BAT >> %DGLOG%

:menu
if exist C:\DOSGAME.BAT goto havemenu
echo C:\DOSGAME.BAT is missing - re-stage the DOS tools.
echo play:   FAILED - C:\DOSGAME.BAT is missing >> %DGLOG%
goto done

:havemenu
echo play:   entering the game menu >> %DGLOG%
call C:\DOSGAME.BAT
echo play:   left the game menu >> %DGLOG%

:done
echo play:   ---- PLAY.BAT done ---- >> %DGLOG%
