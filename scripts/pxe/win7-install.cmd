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
rem NOTHING here may block, and wpeinit does not run first. On .186 (ASUS P5QL,
rem ICH10) wpeinit's device install never finishes ("Starting the network..."
rem forever, 2026-10-15), and drvload queues behind that same device install:
rem with wpeinit started first the screen sat at "Loading network drivers" and
rem the card never sent a packet. So: drvload first, in the background, with
rem nothing ahead of it; wpeinit only as a fallback if the NAS never answers.
rem Realtek's own driver (rt86win7 7.072 / rt64win7 7.100) covers chips WinPE's
rem inbox netrtx32/netrtx64 (2008, REV_01-03) does not - .145 is REV_06. Both are
rem service RTL8167 with the SAME file name, so drvload must never run over an
rem inbox driver that already bound (it fails, leaving the card dead). WinPE has
rem no sc.exe or tasklist: a bound card is an Enum\PCI instance whose Service
rem value is RTL8167.
echo  [1/4] Loading the network driver...
set WPEINIT=
reg query HKLM\SYSTEM\CurrentControlSet\Enum\PCI /s /v Service 2>nul | find /i "RTL8167" >nul && goto inboxnic
if not exist X:\fleet\drivers\*.inf goto netup
start "drvload" /min cmd /c for %%i in (X:\fleet\drivers\*.inf) do drvload "%%i"
set w=0
:waitdrv
set /a w+=1
reg query HKLM\SYSTEM\CurrentControlSet\Enum\PCI /s /v Service 2>nul | find /i "RTL8167" >nul && goto drvok
if %w% GEQ 20 goto drvslow
ping -n 3 127.0.0.1 >nul
goto waitdrv
:inboxnic
echo        A Realtek driver is already running - nothing to load.
goto netup
:drvok
echo        Realtek driver installed.
goto netup
:drvslow
echo        No Realtek card took the driver within 40 s - carrying on.
:netup
echo  [2/4] Starting the network (in the background)...
start "network" /min wpeutil InitializeNetwork
echo  [3/4] Waiting for the NAS at 192.168.1.122 (up to 3 minutes)...
set n=0
:waitnet
set /a n+=1
ping -n 1 -w 1000 192.168.1.122 >nul && goto map
if %n% GEQ 60 goto nonet
set /a m=n %% 10
if %m%==0 echo        still waiting - %n% of 60
if %m%==0 start "network" /min wpeutil InitializeNetwork
if %n%==15 if not defined WPEINIT goto fallback
ping -n 3 127.0.0.1 >nul
goto waitnet
:fallback
set WPEINIT=1
echo        No network yet - starting Windows PE's own device setup, wpeinit, in the background.
start "wpeinit" /min wpeinit
goto waitnet
:map
echo  [4/4] Connecting to the NAS (\\192.168.1.122\files)...
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
rem /unattend only adds X:\fleet\drivers to the installed Windows (offlineServicing);
rem every Setup page is still interactive. See win7-unattend.xml.
set UA=
if exist X:\fleet\unattend.xml set UA=/unattend:X:\fleet\unattend.xml
%SRC%\sources\setup.exe %UA%
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
