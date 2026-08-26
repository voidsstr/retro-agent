@ECHO OFF
rem ---------------------------------------------------------------
rem AUTOEXEC.TPL - template for C:\AUTOEXEC.BAT on a fleet DOS box.
rem
rem A TEMPLATE, NOT A STAGED FILE. The agent deliberately does NOT copy this
rem over a box's AUTOEXEC.BAT: that file belongs to the machine and carries
rem its own drivers (CD-ROM, mouse, sound). Merge these lines in by hand.
rem
rem WHY IT MATTERS: there are TWO ways into real-mode DOS on a Win98 box and
rem they run DIFFERENT files.
rem
rem   1. Shut Down - "Restart in MS-DOS mode"   runs C:\WINDOWS\DOSSTART.BAT
rem   2. Hold CTRL or F8 at boot - "Command prompt only"  runs THIS file
rem
rem Route 2 never starts Windows at all, so it cannot be broken by anything
rem in the Windows-to-DOS shutdown transition - no video mode to restore, no
rem redirector to tear down, no driver to quiesce. It is the same code path
rem the box already survives on every single boot, which makes it the one to
rem fall back on when route 1 sticks at a bare cursor.
rem
rem It was useless here, though: AUTOEXEC.BAT set only BLASTER and MIDI, so
rem route 2 landed on a bare C prompt with no PATH to C:\DOSGAME and nothing
rem saying PLAY exists - which looks like the same failure. These lines make
rem route 2 land exactly where route 1 does.
rem
rem Safe on a normal Windows boot: PATH/SET/echo cannot stop the GUI
rem starting, and the hint scrolls past in a fraction of a second.
rem
rem No angle brackets in a rem comment - COMMAND.COM parses redirection on
rem every line, rem included.
rem ---------------------------------------------------------------
PATH=C:\WINDOWS;C:\WINDOWS\COMMAND;C:\DOSGAME;C:\DOSCHAT
SET BLASTER=A220 I5 D1 H5 P330 T6
SET MIDI=SYNTH:1 MAP:E

rem Which route got us here, and how far it got. Guarded, so a box with no
rem C:\DOSGAME does not print a redirect error on every boot.
if not exist C:\DOSGAME\NUL goto nolog
echo autoexec: ---- real-mode boot ---- >> C:\DOSGAME\DOSGAME.LOG

:nolog
echo.
echo     PLAY   - the game menu (installs and runs DOS games)
echo     CHAT   - the retro chat client
echo.
