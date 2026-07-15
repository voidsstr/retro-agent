@echo off
REM ddk_setenv.bat - fxD3D DDK build-environment wrapper.
REM
REM Sets up the Windows Server 2003 DDK (build 3790) environment for building
REM Windows XP display drivers, then leaves the caller's cmd session configured
REM so `build` is on PATH. Called (not run) by build_fxd3d.bat.
REM
REM   call ddk_setenv.bat [chk|fre] [WXP|WNET]
REM     chk = checked (debug) build   fre = free (release) build   (default chk)
REM     WXP = Windows XP target        WNET = Server 2003 target    (default WXP)
REM
REM BASEDIR is where provision_ddk.py extracts the DDK.

if "%BASEDIR%"=="" set BASEDIR=C:\WINDDK\3790
set _BT=%1
if "%_BT%"=="" set _BT=chk
set _OS=%2
if "%_OS%"=="" set _OS=WXP

if not exist "%BASEDIR%\bin\setenv.bat" (
    echo ERROR: DDK not found at %BASEDIR% - run provision_ddk.py first
    exit /b 2
)
call "%BASEDIR%\bin\setenv.bat" "%BASEDIR%" %_BT% %_OS%
