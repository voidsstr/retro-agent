@echo off
rem ============================================================
rem  Retro Agent Installer for Windows 98SE / XP
rem
rem  Installs:
rem    - retro_agent.exe  (background TCP agent on ports 9897/9898)
rem    - retro_chat.exe   (Claude Code-style chat client)
rem    - retro-infer.exe  (Fleet AI engine, supervised by the agent)
rem
rem  All three autostart/self-heal on boot. The agent self-updates from
rem  the share on every startup; it also updates retro_chat.exe in place
rem  when a newer version is available, and will pull retro-infer.exe on
rem  its own the first time an AI command is used if it's missing --
rem  this installer just gets a fresh install current on day one instead
rem  of waiting for that lazy first-use pull.
rem
rem  EDIT THE SRCDIR BELOW to point to your SMB share before running.
rem
rem  WIN98 NOTE: this runs under COMMAND.COM on Win98, which is much stricter
rem  than XP's cmd.exe. Keep it to the common subset: no "goto :eof", no
rem  "setlocal", no "%~1"/"set /a", no "2>&1", and only quote a path when it
rem  actually contains a space (COMMAND.COM can leave the quotes IN the value).
rem  The earlier "looping" was an 8-char label-name collision (COMMAND.COM
rem  only compares the first 8 characters of a label) -- all labels below are
rem  now distinct within 8 chars.
rem ============================================================

echo.
echo  Retro Agent + Chat Installer
echo  ============================
echo.

rem --- EDIT THIS: point to your SMB share containing the binaries ---
set SRCDIR=\\YOUR-SERVER\share\Retro Automation
set INSTALLDIR=C:\RETRO_AGENT

echo  Source:  %SRCDIR%
echo  Target:  %INSTALLDIR%
echo.

rem -- Was the agent already installed? (decides start vs. reboot below) --
set WASINST=
if exist "%INSTALLDIR%\retro_agent.exe" set WASINST=1

rem -- Stop / move aside the old agent so its .exe can be replaced. --
rem    NT/XP: taskkill it (a stopped exe is overwritable).
rem    Win9x: NO taskkill exists, and Windows won't overwrite a RUNNING exe,
rem    which is why re-running this installer on Win98 failed with "Could not
rem    copy agent binary". A running exe CAN be renamed, though, so we move it
rem    aside to free the name for the new copy; the old process keeps running
rem    until reboot (handled at :startagent -- we don't launch a 2nd copy).
if "%OS%"=="Windows_NT" taskkill /F /IM retro_agent.exe > nul 2>&1
if "%OS%"=="Windows_NT" taskkill /F /IM retro_chat.exe > nul 2>&1
if "%OS%"=="Windows_NT" ping -n 2 127.0.0.1 > nul
if "%OS%"=="Windows_NT" goto moved
if exist "%INSTALLDIR%\retro_agent.exe.old" del "%INSTALLDIR%\retro_agent.exe.old" >nul
if exist "%INSTALLDIR%\retro_agent.exe" ren "%INSTALLDIR%\retro_agent.exe" retro_agent.exe.old
:moved

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
goto copy_infer

:chat_skip
echo  [..] Chat client not found on share, skipping
goto copy_infer

rem -- Copy the Fleet AI engine (optional: agent self-heals this on first
rem    AI command anyway, but a fresh install shouldn't have to wait for
rem    that -- get it current now like everything else). --
:copy_infer
if not exist "%SRCDIR%\retro-infer.exe" goto infer_skip
copy /Y "%SRCDIR%\retro-infer.exe" "%INSTALLDIR%\retro-infer.exe"
if errorlevel 1 goto infer_skip
echo  [OK] AI engine (retro-infer.exe) copied
goto detectos

:infer_skip
echo  [..] AI engine not found on share, skipping (agent will self-heal it later)
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
if not exist "%SRCDIR%\autologon_9x.reg" goto chat9x_reg
regedit /s "%SRCDIR%\autologon_9x.reg"
echo  [OK] Registry keys installed (Win9x)
goto chat9x_reg

:isnt
echo  Detected: Windows NT/XP
if not exist "%SRCDIR%\autologon_nt.reg" goto chatnt_reg
regedit /s "%SRCDIR%\autologon_nt.reg"
echo  [OK] Registry keys installed (WinNT/XP)
goto chatnt_reg

rem -- Register retro_chat.exe to autostart on boot --
rem    Win9x: HKLM\Software\Microsoft\Windows\CurrentVersion\Run
rem    NT/XP: same key, REG.EXE handles it directly
:chat9x_reg
if not exist "%INSTALLDIR%\retro_chat.exe" goto chat_done
rem INSTALLDIR has no space in it, so these targets are deliberately
rem UNQUOTED: some Win9x COMMAND.COM builds mishandle a quoted string
rem after a redirection operator (the quote characters can end up
rem literally part of the resulting filename instead of being stripped),
rem which is exactly what broke this on a real Windows 98 box -- regedit
rem then fails with "cannot import" because the file it's told to open
rem was never actually created at that path.
echo REGEDIT4 > %INSTALLDIR%\chat_run.reg
echo. >> %INSTALLDIR%\chat_run.reg
echo [HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Run] >> %INSTALLDIR%\chat_run.reg
echo "RetroChat"="C:\\RETRO_AGENT\\retro_chat.exe" >> %INSTALLDIR%\chat_run.reg
if not exist %INSTALLDIR%\chat_run.reg goto chat9x_writefail
regedit /s %INSTALLDIR%\chat_run.reg
del %INSTALLDIR%\chat_run.reg >nul
echo  [OK] Chat client autostart registered (Win9x)
goto chat_done

:chat9x_writefail
echo  [..] Could not write chat_run.reg, skipping autostart registration
echo       (chat client will still work, just won't auto-start on boot)
goto chat_done

:chatnt_reg
if not exist "%INSTALLDIR%\retro_chat.exe" goto chat_done
reg add "HKLM\Software\Microsoft\Windows\CurrentVersion\Run" /v RetroChat /t REG_SZ /d "%INSTALLDIR%\retro_chat.exe" /f > nul 2>&1
if errorlevel 1 goto chat_done
echo  [OK] Chat client autostart registered (NT/XP)
goto chat_done

:chat_done
rem -- Configure auto-update paths in registry (idempotent) --
if "%OS%"=="Windows_NT" reg add "HKLM\Software\RetroAgent" /v UpdatePath /t REG_SZ /d "%SRCDIR%\retro_agent.exe" /f > nul 2>&1
if "%OS%"=="Windows_NT" reg add "HKLM\Software\RetroAgent" /v ChatUpdatePath /t REG_SZ /d "%SRCDIR%\retro_chat.exe" /f > nul 2>&1
if "%OS%"=="Windows_NT" reg add "HKLM\Software\RetroAgent" /v EnginePath /t REG_SZ /d "%SRCDIR%\retro-infer.exe" /f > nul 2>&1
goto startagent

:startagent
rem -- On a Win9x RE-install the previous agent is still running (we could
rem    only move its .exe aside, not kill it), so launching a 2nd copy would
rem    just fight it for port 9898. Tell the user to reboot instead; the Run
rem    key launches the new build on boot. NT already taskkilled the old one,
rem    and a fresh install has nothing running, so those start normally. --
set REBOOTMSG=
if not "%WASINST%"=="1" goto do_start
if "%OS%"=="Windows_NT" goto do_start
set REBOOTMSG=1
:do_start
if "%REBOOTMSG%"=="1" goto reinstall_9x
echo.
echo  Starting agent...
start %INSTALLDIR%\retro_agent.exe
echo  [OK] Agent started from %INSTALLDIR%

if not exist "%INSTALLDIR%\retro_chat.exe" goto install_done
echo  Starting chat client...
start %INSTALLDIR%\retro_chat.exe
echo  [OK] Chat client started
goto install_done

:reinstall_9x
echo.
echo  [OK] Agent binary UPDATED to the new build.
echo  ==> The OLD agent is still running. REBOOT this machine to activate the
echo      new version (or close the old agent window, then run
echo      C:\RETRO_AGENT\retro_agent.exe).
echo.

:install_done
echo.
echo  Installation complete!
echo  - Agent will auto-start on boot
echo  - Chat client will auto-start on boot
echo  - Agent + chat client auto-update from the share on every startup
echo  - AI engine (if present on the share) is staged and self-heals if
echo    ever missing or replaced by a newer build
echo.

:done
pause
