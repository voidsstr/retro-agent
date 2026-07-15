@echo off
REM build_fxd3d.bat - build the fxD3D host display driver with the staged DDK.
REM
REM Runs on a fleet box that has been provisioned by provision_ddk.py. Invoked
REM remotely by build_driver.py via the agent (EXEC).
REM
REM   build_fxd3d.bat [srcroot] [chk|fre]
REM     srcroot = tree containing driver\ d3dhal\ glide-sdk\  (default C:\build\fxd3d)
REM
REM Output: copies the built fxd3ddd.dll (and any .sys) to C:\build\out\ and
REM prints BUILD_EXIT=<code> so the caller can tell success from failure.

setlocal
set SRCROOT=%~1
if "%SRCROOT%"=="" set SRCROOT=C:\build\fxd3d
set BLD=%2
if "%BLD%"=="" set BLD=chk

REM DDK env (wrapper is dropped next to this file by provision_ddk.py -> C:\DDK)
call C:\DDK\ddk_setenv.bat %BLD% WXP
if errorlevel 1 ( echo BUILD_EXIT=2 & goto :eof )

if not exist "%SRCROOT%\driver\nt\SOURCES" (
    echo ERROR: sources not found under %SRCROOT% & echo BUILD_EXIT=3 & goto :eof
)

cd /d "%SRCROOT%\driver\nt"
echo === building fxD3D (%BLD% WXP) in %CD% ===
build -cZ -w
set BE=%errorlevel%

if not exist C:\build\out md C:\build\out
REM the DDK drops output under obj*\i386\ ; harvest by name
for /r "%SRCROOT%\driver\nt" %%f in (fxd3ddd.dll fxd3ddd.sys fxd3dmp.sys) do (
    if exist "%%f" copy /Y "%%f" C:\build\out\ >nul 2>&1
)
echo === outputs ===
dir /b C:\build\out 2>nul
echo BUILD_EXIT=%BE%
