@echo off
rem DOSGAME.BAT - wrapper loop for DOSGAME.EXE (the DOS game manager).
rem The menu exits with errorlevel 42 after writing RUN.BAT; we run it with
rem all conventional memory free (the menu is not resident), then loop back.
:menu
C:\DOSGAME\DOSGAME.EXE
if errorlevel 43 goto menu
if errorlevel 42 goto run
goto done
:run
call C:\DOSGAME\RUN.BAT
goto menu
:done
