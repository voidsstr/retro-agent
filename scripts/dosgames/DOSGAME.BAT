@echo off
rem DOSGAME.BAT - wrapper loop for DOSGAME.EXE (the DOS game manager).
rem The menu exits with errorlevel 42 after writing RUN.BAT; we run it with
rem all conventional memory free (the menu is not resident), then loop back.
rem
rem RUN.BAT is deleted before each menu pass. Without that, a menu run that
rem ends any other way than "write RUN.BAT and exit 42" could still find the
rem PREVIOUS script sitting there - and a stray errorlevel 42 would then
rem re-run whatever the user did last time.
rem
rem "if errorlevel N" means ">= N", so the tests must run high to low.
:menu
if exist C:\DOSGAME\RUN.BAT del C:\DOSGAME\RUN.BAT
if not exist C:\DOSGAME\DOSGAME.EXE goto nomenu
C:\DOSGAME\DOSGAME.EXE
rem >=43 means the menu died in a way it does not define (a DOS abort sets a
rem code of its own). Leaving that as "goto menu" turned any such crash into
rem an unattended relaunch loop nobody could break into.
if errorlevel 43 goto crashed
if errorlevel 42 goto run
goto done
:crashed
echo DOSGAME.EXE exited unexpectedly. Type PLAY to try again.
goto done
:run
if not exist C:\DOSGAME\RUN.BAT goto menu
call C:\DOSGAME\RUN.BAT
goto menu
:nomenu
echo DOSGAME.EXE is missing from C:\DOSGAME - re-stage the DOS tools.
pause
:done
