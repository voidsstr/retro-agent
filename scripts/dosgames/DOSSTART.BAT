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
