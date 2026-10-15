@echo off
rem Started by cmd.exe AutoRun (HKLM\Software\Microsoft\Command Processor):
rem WinPE 3.0 winpeshl drops LaunchApps arguments, so "cmd /k script" never ran.
rem The guard stops every nested cmd from starting the install again.
if defined FLEETRAN goto :eof
set FLEETRAN=1
title NSC Retro Fleet - Windows 7 network install
rem Served by retro-pxe on 192.168.1.132 (scripts/pxe/make-win7-source.sh).
rem Setup runs from the NAS copy of the DVD; install.wim is 2 GB and stays there.
set SRC=Z:\Files\OS\Win7PRO-x86-PXE
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" set SRC=Z:\Files\OS\Win7PRO-x64-PXE
echo.
echo  NSC Retro Fleet - Windows 7 Professional network install (%PROCESSOR_ARCHITECTURE%)
echo  =====================================================================
rem NOTHING here may block. wpeinit never returned on .186 (ASUS P5QL, ICH10,
rem 2026-10-15: "Starting the network..." forever), so wpeinit and the network
rem bring-up run in the background and the ping loop below decides.
echo  [1/4] Starting Windows PE (wpeinit, in the background)...
start "wpeinit" /min wpeinit
rem Realtek's own driver (rt86win7 7.072 / rt64win7 7.100) for chips WinPE's
rem inbox netrtx32/netrtx64 (2008, REV_01-03) does not cover - .145 is REV_06.
rem Both are service RTL8167 with the SAME file name, so when the inbox one has
rem already bound (.186, REV_02, 2026-10-15) drvload has to overwrite a loaded
rem Rt64win7.sys and fails, leaving the card dead. Wait for PnP to bind the
rem inbox driver; only load ours if nothing did. WinPE has no sc.exe: a bound
rem card is an Enum\PCI\VEN_10EC... instance whose Service value is RTL8167.
echo  [2/4] Loading network drivers...
set w=0
:waitpnp
set /a w+=1
reg query HKLM\SYSTEM\CurrentControlSet\Enum\PCI /s /v Service 2>nul | find /i "RTL8167" >nul && goto inboxnic
if %w% GEQ 15 goto loaddrv
ping -n 3 127.0.0.1 >nul
goto waitpnp
:inboxnic
echo        Windows PE's own Realtek driver is running - nothing to load.
goto netup
:loaddrv
if not exist X:\fleet\drivers\*.inf goto netup
for %%i in (X:\fleet\drivers\*.inf) do (
  drvload "%%i"
  if errorlevel 1 echo        drvload %%~nxi failed - continuing, the NAS check decides.
)
:netup
echo  [3/4] Starting the network (in the background)...
start "network" /min wpeutil InitializeNetwork
echo  [4/4] Waiting for the NAS at 192.168.1.122 (up to 3 minutes)...
set n=0
:waitnet
set /a n+=1
ping -n 1 -w 1000 192.168.1.122 >nul && goto map
if %n% GEQ 60 goto nonet
set /a m=n %% 10
if %m%==0 start "network" /min wpeutil InitializeNetwork
ping -n 3 127.0.0.1 >nul
goto waitnet
:map
echo  Connecting to the NAS (\\192.168.1.122\files)...
net use Z: \\192.168.1.122\files "" /user:guest >nul 2>&1 && goto run
net use Z: \\192.168.1.122\files "" /user:"" >nul 2>&1 && goto run
net use Z: \\192.168.1.122\files /user:MEDIASERVER\guest "" >nul 2>&1 && goto run
echo.
echo  ** Could not map \\192.168.1.122\files. Trying with a password prompt:
net use Z: \\192.168.1.122\files && goto run
goto fail
:run
if not exist %SRC%\sources\setup.exe goto nosrc
echo  Starting Windows Setup from %SRC% ...
%SRC%\sources\setup.exe
goto :eof
:nonet
echo.
echo  ** No network: 192.168.1.122 never answered. Windows PE probably has no
echo  ** driver for this network card. Adapter state:
ipconfig /all
echo  ** Network-class devices drvload could not match (send this line to the operator):
reg query HKLM\SYSTEM\CurrentControlSet\Enum\PCI /s /f VEN_ /k 2>nul | find /i "VEN_"
goto fail
:nosrc
echo  ** %SRC%\sources\setup.exe is missing on the NAS.
:fail
echo.
echo  Dropping to a command prompt. Type "exit" to reboot.
cmd /k
