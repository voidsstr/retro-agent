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
echo.
echo  NSC Retro Fleet - Windows 7 Professional network install
echo  =========================================================
echo  Starting the network...
wpeinit
rem NIC drivers Setup's WinPE lacks (it has no RTL8168/8111 driver at all; that one
rem is netrtx32 from install.wim). The card comes up only after the network is
rem re-initialised, so drvload then wpeutil InitializeNetwork.
if exist X:\fleet\drivers\*.inf for %%i in (X:\fleet\drivers\*.inf) do drvload "%%i" >nul
if exist X:\fleet\drivers\*.inf wpeutil InitializeNetwork >nul
set n=0
:waitnet
set /a n+=1
ping -n 1 -w 1000 192.168.1.122 >nul && goto map
if %n% GEQ 45 goto nonet
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
ipconfig
goto fail
:nosrc
echo  ** %SRC%\sources\setup.exe is missing on the NAS.
:fail
echo.
echo  Dropping to a command prompt. Type "exit" to reboot.
cmd /k
