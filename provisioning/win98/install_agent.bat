@echo off
rem ============================================================
rem  Retro Agent Installer for Windows 98SE / XP
rem
rem  Installs:
rem    - retro_agent.exe  (background TCP agent on ports 9897/9898)
rem    - retro_chat.exe   (Claude Code-style chat client)
rem
rem  Both autostart on boot. The agent self-updates from the share
rem  on every startup; it also updates retro_chat.exe in place when
rem  a newer version is available.
rem
rem  Run from: \\192.168.1.122\files\Utility\Retro Automation
rem ============================================================

echo.
echo  Retro Agent + Chat Installer
echo  ============================
echo.

set SRCDIR=\\192.168.1.122\files\Utility\Retro Automation
set INSTALLDIR=C:\RETRO_AGENT

echo  Source:  %SRCDIR%
echo  Target:  %INSTALLDIR%
echo.

rem -- Kill any old processes (XP has taskkill, Win9x doesn't) --
if "%OS%"=="Windows_NT" taskkill /F /IM retro_agent.exe > nul 2>&1
if "%OS%"=="Windows_NT" taskkill /F /IM retro_chat.exe > nul 2>&1
if "%OS%"=="Windows_NT" ping -n 2 127.0.0.1 > nul

rem -- Create install directory --
if not exist "%INSTALLDIR%\nul" mkdir "%INSTALLDIR%"
if not exist "%INSTALLDIR%" mkdir "%INSTALLDIR%"
echo  [OK] Install directory ready

rem -- Copy agent binary --
copy /Y "%SRCDIR%\retro_agent.exe" "%INSTALLDIR%\retro_agent.exe"
if errorlevel 1 goto err_copy_agent
echo  [OK] Agent binary copied

rem -- Copy chat client (optional: don't fail install if missing) --
if not exist "%SRCDIR%\retro_chat.exe" goto chat_skip
copy /Y "%SRCDIR%\retro_chat.exe" "%INSTALLDIR%\retro_chat.exe"
if errorlevel 1 goto chat_skip
echo  [OK] Chat client copied
goto detectos

:chat_skip
echo  [..] Chat client not found on share, skipping
goto detectos

:err_copy_agent
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
if not exist "%SRCDIR%\autologon_9x.reg" goto chat_autostart_9x
regedit /s "%SRCDIR%\autologon_9x.reg"
echo  [OK] Registry keys installed (Win9x)
goto chat_autostart_9x

:isnt
echo  Detected: Windows NT/XP
if not exist "%SRCDIR%\autologon_nt.reg" goto chat_autostart_nt
regedit /s "%SRCDIR%\autologon_nt.reg"
echo  [OK] Registry keys installed (WinNT/XP)
goto chat_autostart_nt

rem -- Register retro_chat.exe to autostart on boot --
rem    Win9x: HKLM\Software\Microsoft\Windows\CurrentVersion\Run
rem    NT/XP: same key, REG.EXE handles it directly
:chat_autostart_9x
if not exist "%INSTALLDIR%\retro_chat.exe" goto chat_autostart_done
echo REGEDIT4 > "%INSTALLDIR%\chat_run.reg"
echo. >> "%INSTALLDIR%\chat_run.reg"
echo [HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Run] >> "%INSTALLDIR%\chat_run.reg"
echo "RetroChat"="C:\\RETRO_AGENT\\retro_chat.exe" >> "%INSTALLDIR%\chat_run.reg"
regedit /s "%INSTALLDIR%\chat_run.reg"
del "%INSTALLDIR%\chat_run.reg"
echo  [OK] Chat client autostart registered (Win9x)
goto chat_autostart_done

:chat_autostart_nt
if not exist "%INSTALLDIR%\retro_chat.exe" goto chat_autostart_done
reg add "HKLM\Software\Microsoft\Windows\CurrentVersion\Run" /v RetroChat /t REG_SZ /d "%INSTALLDIR%\retro_chat.exe" /f > nul 2>&1
if errorlevel 1 goto chat_autostart_done
echo  [OK] Chat client autostart registered (NT/XP)
goto chat_autostart_done

:chat_autostart_done
rem -- Configure auto-update paths in registry (idempotent) --
if "%OS%"=="Windows_NT" reg add "HKLM\Software\RetroAgent" /v UpdatePath /t REG_SZ /d "%SRCDIR%\retro_agent.exe" /f > nul 2>&1
if "%OS%"=="Windows_NT" reg add "HKLM\Software\RetroAgent" /v ChatUpdatePath /t REG_SZ /d "%SRCDIR%\retro_chat.exe" /f > nul 2>&1
goto startagent

:startagent
rem -- Start the agent (which will also launch retro_chat after the
rem    auto-update check on the next reboot, or you can start chat now). --
echo.
echo  Starting agent...
start %INSTALLDIR%\retro_agent.exe
echo  [OK] Agent started from %INSTALLDIR%

if not exist "%INSTALLDIR%\retro_chat.exe" goto install_done
echo  Starting chat client...
start %INSTALLDIR%\retro_chat.exe
echo  [OK] Chat client started

:install_done
echo.
echo  Installation complete!
echo  - Agent will auto-start on boot
echo  - Chat client will auto-start on boot
echo  - Both will auto-update from the share on every startup
echo.

:done
pause
