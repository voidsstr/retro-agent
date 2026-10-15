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
ping -n 6 127.0.0.1 >nul
rem NIC drivers Setup's WinPE lacks (it has no RTL8168/8111 driver at all; this is
rem Realtek's rt86win7 7.072, REV_02-09).
echo  [2/4] Loading network drivers...
if exist X:\fleet\drivers\*.inf for %%i in (X:\fleet\drivers\*.inf) do drvload "%%i"
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
