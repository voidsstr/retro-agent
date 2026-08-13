@echo off
rem serve_win.bat - run the DOS games HTTP bridge on the WINDOWS side.
rem
rem Double-click this, or run it from a terminal. Leave the window open: the
rem DOS boxes fetch from it while it runs.
rem
rem WSL2 is NAT'd, so a server bound inside WSL is invisible to the fleet: the
rem DOS boxes reach this machine on its LAN address, not the WSL one. That is
rem why this has to run here and cannot be started from the Linux side - and
rem with Windows interop disabled, the Linux side cannot even launch it.
rem
rem   DOSGAMES_SHARE  the DOS archive tree   (default: the UNC path below, so
rem                                           no drive mapping is required)
rem   DOSGAMES_DATA   GAMES.CAT + tiles      (default: the data dir beside this)
rem   port            first argument         (default 8181)
rem
rem No angle brackets anywhere in this file, not even in a comment: both
rem cmd.exe and COMMAND.COM parse redirection on a rem line, so the default
rem above used to open a file for input and create one named "repo".

setlocal

rem --- the archives. A UNC path works without mapping a drive letter, and the
rem     collection has been seen at two different depths on this share, so try
rem     the known layouts rather than hardcoding one and 404ing every download.
rem     Set DOSGAMES_SHARE yourself to skip the search.
if not "%DOSGAMES_SHARE%"=="" goto haveshare
for %%D in (
    "\\192.168.1.122\files\Files\Games\DOS"
    "\\192.168.1.122\files\Games\DOS"
    "Z:\Files\Games\DOS"
    "Z:\Games\DOS"
) do if exist "%%~D\nul" if "%DOSGAMES_SHARE%"=="" set DOSGAMES_SHARE=%%~D
if "%DOSGAMES_SHARE%"=="" set DOSGAMES_SHARE=\\192.168.1.122\files\Files\Games\DOS
:haveshare
if "%DOSGAMES_DATA%"=="" set DOSGAMES_DATA=%~dp0data
set PORT=%1
if "%PORT%"=="" set PORT=8181

rem --- find Python. "python" alone is not enough: a machine can have the py
rem     launcher, or a versioned install, without python.exe on PATH - and the
rem     failure is a bare "not recognized" that looks like the script is broken.
set PY=
python -c "import sys" >nul 2>&1 && set PY=python
if "%PY%"=="" py -3 -c "import sys" >nul 2>&1 && set PY=py -3
if "%PY%"=="" if exist C:\Python314\python.exe set PY=C:\Python314\python.exe
if "%PY%"=="" (
    echo.
    echo   No Python found. Install it from python.org, or set PY in this file.
    echo.
    pause
    exit /b 1
)

rem --- the archives have to be readable or every download 404s
if not exist "%DOSGAMES_SHARE%" (
    echo.
    echo   Cannot read %DOSGAMES_SHARE%
    echo   Check the file server is up, or set DOSGAMES_SHARE to the DOS
    echo   archive directory and run this again.
    echo.
    pause
    exit /b 1
)

echo.
echo   share  = %DOSGAMES_SHARE%
echo   data   = %DOSGAMES_DATA%
echo   python = %PY%
echo.
echo   Serving on port %PORT%. The DOS boxes' DOSGAME.CFG must have
echo   url=http://THIS-PC-LAN-IP:%PORT%   (currently 192.168.1.82)
echo.
echo   If a DOS box says "HTGET failed", the usual cause is the Windows
echo   firewall: run open-firewall.ps1 once, as Administrator.
echo.
echo   Leave this window open. Ctrl-C to stop.
echo.

%PY% "%~dp0serve_dosgames.py" %PORT%

echo.
echo   The server stopped.
pause
