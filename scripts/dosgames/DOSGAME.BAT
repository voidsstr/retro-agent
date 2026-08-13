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
rem "if errorlevel N" means N or higher, so the tests must run high to low.
rem
rem NOTHING in this file may contain an angle bracket - not even a comment.
rem COMMAND.COM parses redirection on EVERY line, rem included: this line and
rem the "code 43 or higher" one below used to spell those words with angle
rem brackets, and each pass through :menu created a stray 0-byte file named 43 in
rem whatever directory the loop was standing in (C:\43, C:\KEEN\43, C:\DUKE\43
rem and C:\GAMES\HERETIC\43 were all found on the Win98 box). RUN.BAT cd's
rem into the game's directory and a batch cd persists into its caller, which
rem is why they landed all over the disk. tests/run_dos_tests.sh enforces this.
rem
rem This file logs like the rest of the chain. It was the ONE step with no
rem logging, and that is exactly where PLAY died on the Win98 box: the log
rem showed "entering the game menu" then "left the game menu" with nothing
rem in between, so there was no way to tell whether this batch had even run.
rem DOSGAME.EXE's output is appended to the log too - "Bad command or file
rem name" comes from COMMAND.COM on stdout, so redirecting it captures the
rem failure instead of leaving it on a screen nobody is reading. (The menu
rem itself draws straight to video memory, so redirecting costs it nothing.)
SET DGLOG=C:\DOSGAME\DOSGAME.LOG
echo menubat: DOSGAME.BAT starting >> %DGLOG%

:menu
if exist C:\DOSGAME\RUN.BAT del C:\DOSGAME\RUN.BAT
if not exist C:\DOSGAME\DOSGAME.EXE goto nomenu
echo menubat: launching DOSGAME.EXE >> %DGLOG%
C:\DOSGAME\DOSGAME.EXE >> %DGLOG%
rem code 43 or higher means the menu died in a way it does not define (a DOS abort sets a
rem code of its own). Leaving that as "goto menu" turned any such crash into
rem an unattended relaunch loop nobody could break into.
if errorlevel 43 goto crashed
if errorlevel 42 goto run
echo menubat: menu exited normally >> %DGLOG%
goto done

:crashed
echo menubat: DOSGAME.EXE exited with an undefined code >> %DGLOG%
echo DOSGAME.EXE exited unexpectedly. Type PLAY to try again.
goto done

:run
if not exist C:\DOSGAME\RUN.BAT goto norun
echo menubat: running RUN.BAT >> %DGLOG%
call C:\DOSGAME\RUN.BAT
echo menubat: back from RUN.BAT >> %DGLOG%
goto menu

:norun
echo menubat: exit 42 but no RUN.BAT was written >> %DGLOG%
goto menu

:nomenu
echo menubat: DOSGAME.EXE is MISSING from C:\DOSGAME >> %DGLOG%
echo DOSGAME.EXE is missing from C:\DOSGAME - re-stage the DOS tools.
pause

:done
echo menubat: DOSGAME.BAT done >> %DGLOG%
