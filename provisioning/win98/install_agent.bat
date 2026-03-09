@echo off
rem ============================================================
rem  Retro Agent Installer for Windows 98SE / XP
rem  Run from: \\192.168.1.122\files\Utility\Retro Automation
rem ============================================================

echo.
echo  Retro Agent Installer
echo  =====================
echo.

set SRCDIR=\\192.168.1.122\files\Utility\Retro Automation
set INSTALLDIR=C:\RETRO_AGENT

echo  Source:  %SRCDIR%
echo  Target:  %INSTALLDIR%
echo.

rem -- Kill any old agent (XP has taskkill, Win9x doesn't) --
if "%OS%"=="Windows_NT" taskkill /F /IM retro_agent.exe > nul 2>&1
if "%OS%"=="Windows_NT" ping -n 2 127.0.0.1 > nul

rem -- Create install directory --
if not exist "%INSTALLDIR%\nul" mkdir "%INSTALLDIR%"
if not exist "%INSTALLDIR%" mkdir "%INSTALLDIR%"
echo  [OK] Install directory ready

rem -- Copy agent binary --
copy /Y "%SRCDIR%\retro_agent.exe" "%INSTALLDIR%\retro_agent.exe"
if errorlevel 1 goto err_copy
echo  [OK] Agent binary copied
goto detectos

:err_copy
echo.
echo  ERROR: Could not copy agent binary!
echo  The agent may still be running. Close the agent window
echo  and run this installer again.
echo.
goto done

:detectos
rem -- Detect OS: Windows NT/XP sets %OS%=Windows_NT --
rem -- Windows 98 does not set %OS% at all --
if "%OS%"=="Windows_NT" goto isnt
goto is9x

:is9x
echo  Detected: Windows 9x
if not exist "%SRCDIR%\autologon_9x.reg" goto startagent
regedit /s "%SRCDIR%\autologon_9x.reg"
echo  [OK] Registry keys installed (Win9x)
goto startagent

:isnt
echo  Detected: Windows NT/XP
if not exist "%SRCDIR%\autologon_nt.reg" goto startagent
regedit /s "%SRCDIR%\autologon_nt.reg"
echo  [OK] Registry keys installed (WinNT/XP)
goto startagent

:startagent
rem -- Start the agent --
echo.
echo  Starting agent...
start %INSTALLDIR%\retro_agent.exe
echo  [OK] Agent started from %INSTALLDIR%
echo.
echo  Installation complete!
echo  The agent will start automatically on boot.
echo.

:done
pause
