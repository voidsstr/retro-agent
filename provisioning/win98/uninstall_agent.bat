@echo off
rem ============================================================
rem  Retro Agent Uninstaller for Windows 98SE / XP
rem  Run from: \\192.168.1.122\files\Utility\Retro Automation
rem  or locally from any directory
rem ============================================================

echo.
echo  Retro Agent Uninstaller
echo  =======================
echo.

set INSTALLDIR=C:\RETRO_AGENT

rem -- Stop the agent if running --
echo  Stopping agent...
if "%OS%"=="Windows_NT" goto killnt
goto kill9x

:kill9x
rem -- Win98: no taskkill, use undocumented command --
echo  Please close retro_agent.exe manually if running.
echo  Press any key to continue after closing it...
pause
goto removefiles

:killnt
taskkill /F /IM retro_agent.exe >nul 2>&1
echo  [OK] Agent process stopped
goto removefiles

:removefiles
rem -- Remove agent files --
echo  Removing agent files...
if not exist "%INSTALLDIR%\nul" goto nodir
del /Q "%INSTALLDIR%\retro_agent.exe" >nul 2>&1
del /Q "%INSTALLDIR%\agent.log" >nul 2>&1
rmdir "%INSTALLDIR%" >nul 2>&1
echo  [OK] Agent files removed
goto removereg

:nodir
echo  [--] Agent directory not found (already removed)
goto removereg

:removereg
rem -- Remove registry keys --
echo  Removing registry keys...
if "%OS%"=="Windows_NT" goto regremovent
goto regremove9x

:regremove9x
rem -- Win98: create a .reg file that deletes the keys --
echo REGEDIT4> C:\WINDOWS\TEMP\uninstall.reg
echo.>> C:\WINDOWS\TEMP\uninstall.reg
echo [HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Run]>> C:\WINDOWS\TEMP\uninstall.reg
echo "RetroAgent"=->> C:\WINDOWS\TEMP\uninstall.reg
echo "MapShare"=->> C:\WINDOWS\TEMP\uninstall.reg
echo.>> C:\WINDOWS\TEMP\uninstall.reg
echo [HKEY_LOCAL_MACHINE\Network\Logon]>> C:\WINDOWS\TEMP\uninstall.reg
echo "PrimaryProvider"=->> C:\WINDOWS\TEMP\uninstall.reg
echo "username"=->> C:\WINDOWS\TEMP\uninstall.reg
echo "MustBeValidated"=->> C:\WINDOWS\TEMP\uninstall.reg
regedit /s C:\WINDOWS\TEMP\uninstall.reg
del C:\WINDOWS\TEMP\uninstall.reg >nul 2>&1
echo  [OK] Registry keys removed (Win9x)
goto disconnect

:regremovent
echo REGEDIT4> %TEMP%\uninstall.reg
echo.>> %TEMP%\uninstall.reg
echo [HKEY_LOCAL_MACHINE\Software\Microsoft\Windows\CurrentVersion\Run]>> %TEMP%\uninstall.reg
echo "RetroAgent"=->> %TEMP%\uninstall.reg
echo "MapShare"=->> %TEMP%\uninstall.reg
echo.>> %TEMP%\uninstall.reg
echo [HKEY_LOCAL_MACHINE\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon]>> %TEMP%\uninstall.reg
echo "AutoAdminLogon"=->> %TEMP%\uninstall.reg
echo "DefaultUserName"=->> %TEMP%\uninstall.reg
echo "DefaultPassword"=->> %TEMP%\uninstall.reg
regedit /s %TEMP%\uninstall.reg
del %TEMP%\uninstall.reg >nul 2>&1
echo  [OK] Registry keys removed (WinNT/XP)
goto disconnect

:disconnect
rem -- Disconnect network share --
echo  Disconnecting network share...
net use \\192.168.1.122\files /delete >nul 2>&1
echo  [OK] Network share disconnected
echo.
echo  Uninstall complete!
echo  The agent has been removed from this machine.
echo.

pause
