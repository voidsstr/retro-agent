@echo off
rem Quake III: Team Arena - the mission pack, which is fully staged in
rem missionpack\ (pak0-pak3 + cgamex86.dll + qagamex86.dll) but had no
rem desktop shortcut of its own until 2026-08-29.
rem fs_game selects the mod; the CD key is missionpack\q3key.
call "%~dp0FLEETRES.BAT"
> "%~dp0missionpack\fleetres.cfg" echo // written by the launcher at every start - do not edit
>>"%~dp0missionpack\fleetres.cfg" echo seta r_mode "-1"
>>"%~dp0missionpack\fleetres.cfg" echo seta r_customwidth "%FR_W%"
>>"%~dp0missionpack\fleetres.cfg" echo seta r_customheight "%FR_H%"
>>"%~dp0missionpack\fleetres.cfg" echo seta r_customaspect "1"
>>"%~dp0missionpack\fleetres.cfg" echo seta r_customPixelAspect "1"
>>"%~dp0missionpack\fleetres.cfg" echo seta r_fullscreen "1"
>>"%~dp0missionpack\fleetres.cfg" echo seta r_displayRefresh "%FR_HZ%"
>>"%~dp0missionpack\fleetres.cfg" echo seta cg_fov "%FR_FOV%"

call "%~dp0FLEETGL.BAT" "%~dp0missionpack\fleetres.cfg"

cd /d "%~dp0"
start "" %FR_ENGINE% +set fs_game missionpack +set r_mode -1 +set r_customwidth %FR_W% +set r_customheight %FR_H% +set r_customaspect 1 +set r_customPixelAspect 1 +set r_fullscreen 1 +set cg_fov %FR_FOV%

rem Close this console. A .bat that ends at "start" leaves its window open for
rem the life of the game, and after a dozen launches box-145 had SIXTEEN of
rem them stacked on the desktop - which actively corrupted testing, because a
rem screenshot then showed a cluttered desktop and you could not tell "the
rem game is fullscreen and GDI cannot see it" from "the game never started".
exit
