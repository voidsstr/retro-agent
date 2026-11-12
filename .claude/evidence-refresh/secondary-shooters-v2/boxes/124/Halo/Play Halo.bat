@echo off
call "%~dp0FLEETRES.BAT"

rem ---- per-box HALO_NOVIDEO - see stage-fleetres.py halo_novideo ----
set HALO_NOVIDEO=
sc query vcrmp 2>nul | find /i "RUNNING" >nul && set HALO_NOVIDEO=-novideo

cd /d "%~dp0"
rem ---- per-box HALO_BOXKEY - see stage-fleetres.py ----------------
if exist "%ALLUSERSPROFILE%\RetroFleet\halo-key.reg" reg import "%ALLUSERSPROFILE%\RetroFleet\halo-key.reg" >nul 2>&1
if not exist "%ALLUSERSPROFILE%\RetroFleet\halo-key.reg" echo NO PER-BOX HALO KEY - this box plays on the library key, and a second box on it is refused. Assign one with scripts/halo/assign_keys.py

start "" halo.exe %HALO_NOVIDEO% -vidmode %FR_W%,%FR_H%,%FR_HZ%
