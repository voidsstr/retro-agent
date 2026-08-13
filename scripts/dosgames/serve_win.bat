@echo off
rem serve_win.bat - run the DOS games HTTP bridge on the WINDOWS side.
rem
rem WSL2 is NAT'd, so a server bound inside WSL is invisible to the fleet: the
rem DOS boxes reach this machine on its LAN address (Wi-Fi), not the WSL one.
rem Running it here also means the SMB share is used through the drive letter
rem Windows already has mapped, with no Linux-side mount at all.
rem
rem   DOSGAMES_SHARE  the DOS archive tree      (default Z:\Files\Games\DOS)
rem   DOSGAMES_DATA   GAMES.CAT + tiles\        (default REPO\scripts\dosgames\data)
rem
rem No angle brackets anywhere in this file, not even in a comment: both
rem cmd.exe and COMMAND.COM parse redirection on a rem line, so the default
rem above used to open a file for input and create one named "repo".
rem   port            first argument            (default 8181)

if "%DOSGAMES_SHARE%"=="" set DOSGAMES_SHARE=Z:\Files\Games\DOS
if "%DOSGAMES_DATA%"=="" set DOSGAMES_DATA=%~dp0data
set PORT=%1
if "%PORT%"=="" set PORT=8181

echo share = %DOSGAMES_SHARE%
echo data  = %DOSGAMES_DATA%
python "%~dp0serve_dosgames.py" %PORT%
