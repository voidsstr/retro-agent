@echo off
rem ---------------------------------------------------------------
rem DOSSTART.BAT - what Windows 98 runs when you pick
rem "Restart in MS-DOS mode" from the Shut Down dialog.
rem Deploys to C:\WINDOWS\DOSSTART.BAT (staged by the agent, v1.30.0+).
rem
rem NOT AUTOEXEC.BAT. That is the file real-mode BOOT runs; MS-DOS mode
rem runs this one instead, which is why a machine can be perfectly set up
rem at the boot prompt and bare in MS-DOS mode. On the fleet's Win98 box
rem this file did not exist at all, so MS-DOS mode came up with no path to
rem the DOS tools and nothing telling the operator that PLAY exists.
rem
rem THE FIRST LINE IS A LOG MARKER, AND IT IS THE POINT.
rem "Restart in MS-DOS mode" got stuck at a bare cursor on .243 and left
rem NOTHING behind: nothing on the box logs anything between the Shut Down
rem dialog and the moment the operator types PLAY, so there was no way to
rem tell a hang in the Windows-to-DOS transition (video mode not restored,
rem a driver that will not quiesce, the network redirector) from a hang
rem once DOS is already up. Now the answer is in the log either way:
rem
rem   "dosstart: reached MS-DOS mode" present  - DOS came up. The hang is
rem       after this, or the screen simply never came back to text mode.
rem   marker ABSENT                            - the machine never got out
rem       of the Windows shutdown. Nothing in DOS is involved at all.
rem
rem No angle brackets anywhere in a rem comment: COMMAND.COM parses
rem redirection on every line, rem included, which is what created the
rem stray files named 43 all over the disk. Real lines below may use them.
rem ---------------------------------------------------------------
if not exist C:\DOSGAME\NUL goto nolog
echo dosstart: ---- reached MS-DOS mode ---- >> C:\DOSGAME\DOSGAME.LOG

:nolog
PATH=C:\WINDOWS;C:\WINDOWS\COMMAND;C:\DOSGAME;C:\DOSCHAT
rem The DOS environment had NO mouse driver: every mouse game said
rem "mouse not detected", and Return to Zork hung outright - it polls
rem INT 33h and on real hardware nothing answers without a driver.
rem DOSBox installs its own INT 33h handler, which is why no DOSBox
rem test ever caught it. CuteMouse is FreeDOS's GPL driver, 6K.
rem THE MOUSE DRIVER IS OPT-OUT NOW, and this is why. The keyboard stopped
rem working inside games on .243 in the same session CuteMouse was first
rem staged. That box is PS/2 at both ends - BIOS enum shows *PNP0303
rem (PS/2 keyboard) and *PNP0F13 (PS/2 mouse) - so both devices sit behind
rem the SAME 8042 controller, and a mouse driver that probes or programs it
rem can leave the keyboard side out of sorts for a game that installs its
rem own INT 9 handler, while BIOS-based input - this prompt, the DOSGAME
rem menu - keeps working. That matches the report exactly.
rem
rem It is a hypothesis, not a proven cause, and it cannot be tested from
rem off the machine: nobody remote can press a key inside a DOS game. So
rem make it ONE step to settle. Create C:\DOSGAME\NOMOUSE.FLG and reboot
rem to MS-DOS mode. Keyboard back in games means the mouse driver is the
rem cause; unchanged means this is ruled out. Either way the log below
rem records which way the run went, so the next log answers it.
rem Delete the flag to restore the mouse.
rem
rem Default stays LOADED: with nothing answering INT 33h every mouse game
rem reports 'mouse not detected' and Return to Zork hangs outright.
if not exist C:\DOSGAME\CTMOUSE.EXE goto nomouse
if exist C:\DOSGAME\NOMOUSE.FLG goto mouseoff
C:\DOSGAME\CTMOUSE.EXE
if not exist C:\DOSGAME\NUL goto nomouse
echo dosstart: mouse driver loaded (CuteMouse) >> C:\DOSGAME\DOSGAME.LOG
goto nomouse

:mouseoff
if not exist C:\DOSGAME\NUL goto nomouse
echo dosstart: mouse driver SKIPPED - NOMOUSE.FLG present >> C:\DOSGAME\DOSGAME.LOG
:nomouse
SET BLASTER=A220 I5 D1 H5 P330 T6
SET MIDI=SYNTH:1 MAP:E
PROMPT=MS-DOS mode $p$g

echo.
echo   MS-DOS mode.
echo.
echo     PLAY   - the game menu (installs and runs DOS games)
echo     CHAT   - the retro chat client
echo     EXIT   - back to Windows
echo.
if not exist C:\DOSGAME\NUL goto done
echo dosstart: handing over to the prompt >> C:\DOSGAME\DOSGAME.LOG

:done
