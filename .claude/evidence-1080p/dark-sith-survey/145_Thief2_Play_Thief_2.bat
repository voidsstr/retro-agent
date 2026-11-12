@echo off
rem install.cfg / darkinst.cfg use relative paths, so the working directory
rem must be the game directory.
call "%~dp0FLEETRES.BAT"
if exist "%~dp0FLEETRES.EXE" "%~dp0FLEETRES.EXE" -setline "%~dp0cam.cfg" game_screen_size game_screen_size %FR_W% %FR_H%

cd /d "%~dp0"
rem ---- per-box DISPLAY SYSTEM - see stage-fleetres.py newdark_display_swap ----
rem NewDark's Direct3D 9 display needs a screen-sized 32-bit texture;
rem vcr-kmd's HAL has 16-bit textures of at most 256x256. Where vcr-kmd
rem drives the screen cam_ext.cfg is moved aside and NewDark runs its
rem legacy DirectX 6 display at 16 bpp, which that HAL can host.
set ND_DX6=0
sc query vcrmp 2>nul | find /i "RUNNING" >nul && set ND_DX6=1
if "%ND_DX6%"=="1" (
  if exist "%~dp0cam_ext.cfg" move /y "%~dp0cam_ext.cfg" "%~dp0cam_ext.cfg.d3d9" >nul
) else (
  if not exist "%~dp0cam_ext.cfg" if exist "%~dp0cam_ext.cfg.d3d9" move /y "%~dp0cam_ext.cfg.d3d9" "%~dp0cam_ext.cfg" >nul
)
if exist "%~dp0nd_dx6-error.txt" del /q "%~dp0nd_dx6-error.txt" >nul 2>&1
if "%ND_DX6%"=="1" if exist "%~dp0cam_ext.cfg" echo cam_ext.cfg could not be moved aside - Thief II will try its Direct3D 9 display on vcr-kmd and refuse to start> "%~dp0nd_dx6-error.txt"

start "" Thief2.exe

rem Close this console. A .bat that ends at "start" leaves its window open for
rem the life of the game, and after a dozen launches box-145 had SIXTEEN of
rem them stacked on the desktop - which actively corrupted testing, because a
rem screenshot then showed a cluttered desktop and you could not tell "the
rem game is fullscreen and GDI cannot see it" from "the game never started".
exit
