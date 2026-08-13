@echo off
rem ---------------------------------------------------------------
rem DOSSTART.BAT - what Windows 98 runs when you pick
rem "Restart in MS-DOS mode" from the Shut Down dialog.
rem Deploys to C:\WINDOWS\DOSSTART.BAT.
rem
rem NOT AUTOEXEC.BAT. That is the file real-mode BOOT runs; MS-DOS mode
rem runs this one instead, which is why a machine can be perfectly set up
rem at the boot prompt and bare in MS-DOS mode. On the fleet's Win98 box
rem this file did not exist at all, so MS-DOS mode came up with no path to
rem the DOS tools and nothing telling the operator that PLAY exists.
rem
rem No angle brackets anywhere in this file, not even in a comment:
rem COMMAND.COM parses redirection on every line, rem included, which is
rem what created the stray files named 43 all over the disk.
rem ---------------------------------------------------------------
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
