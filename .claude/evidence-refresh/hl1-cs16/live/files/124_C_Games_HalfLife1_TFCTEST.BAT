@echo off
cd /d C:\Games\HalfLife1
call FLEETRES.BAT
start "" hl.exe -nosierra -full -gl -w %FR_W43% -h %FR_H43% -toconsole -game tfc +maxplayers 4 +map 2fort
