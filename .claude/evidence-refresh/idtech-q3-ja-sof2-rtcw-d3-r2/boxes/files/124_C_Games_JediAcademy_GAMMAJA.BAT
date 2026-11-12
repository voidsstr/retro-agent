@echo off
cd /d C:\Games\JediAcademy
call FLEETRES.BAT
start "" jamp.exe +set logfile 2 +set r_mode -1 +set r_customwidth %FR_W% +set r_customheight %FR_H% +set r_customaspect 1 +set r_fullscreen 1
