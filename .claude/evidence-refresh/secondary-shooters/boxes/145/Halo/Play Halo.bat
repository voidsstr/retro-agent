@echo off
call "%~dp0FLEETRES.BAT"

cd /d "%~dp0"
start "" halo.exe -vidmode %FR_W%,%FR_H%,%FR_HZ%
