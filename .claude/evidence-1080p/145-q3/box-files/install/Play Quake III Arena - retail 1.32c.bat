@echo off
rem The retail id binary, 1.32c, kept alongside ioquake3 as an A/B: on a box
rem whose OpenGL driver upsets ioquake3 this one sometimes still runs, and
rem vice versa. Reads baseq3\q3key.
rem
rem *** THIS BINARY DOES JOIN THE FLEET Q3 SERVER. *** An earlier version of
rem this comment said it could not: retail 1.32c speaks network protocol 68
rem and the server is ioquake3 at com_protocol 71. But both fleet Q3 units
rem (quake3-server :27961 and q3ta-server :27962) run +set com_legacyprotocol
rem 68, which serves BOTH - re-measured 2026-09-01 by connecting from .171
rem and standing in q3dm6 with the bots, screenshot kept.
rem
rem That matters beyond an A/B: on a 3dfx Voodoo 2 box this is the ONLY
rem engine that can reach the card at all (see FLEETGL.BAT), so if retail
rem really could not join, .171 would have to choose between its Voodoo and
rem multiplayer. It does not.
call "%~dp0FLEETRES.BAT"
> "%~dp0baseq3\fleetres.cfg" echo // written by the launcher at every start - do not edit
>>"%~dp0baseq3\fleetres.cfg" echo seta r_mode "-1"
>>"%~dp0baseq3\fleetres.cfg" echo seta r_customwidth "%FR_W%"
>>"%~dp0baseq3\fleetres.cfg" echo seta r_customheight "%FR_H%"
>>"%~dp0baseq3\fleetres.cfg" echo seta r_customaspect "1"
>>"%~dp0baseq3\fleetres.cfg" echo seta r_customPixelAspect "1"
>>"%~dp0baseq3\fleetres.cfg" echo seta r_fullscreen "1"
>>"%~dp0baseq3\fleetres.cfg" echo seta r_displayRefresh "%FR_HZ%"
>>"%~dp0baseq3\fleetres.cfg" echo seta cg_fov "%FR_FOV%"

call "%~dp0FLEETGL.BAT" "%~dp0baseq3\fleetres.cfg"

cd /d "%~dp0"
start "" quake3.exe +set r_mode -1 +set r_customwidth %FR_W% +set r_customheight %FR_H% +set r_customaspect 1 +set r_customPixelAspect 1 +set r_fullscreen 1 +set cg_fov %FR_FOV%

rem Close this console. A .bat that ends at "start" leaves its window open for
rem the life of the game, and after a dozen launches box-145 had SIXTEEN of
rem them stacked on the desktop - which actively corrupted testing, because a
rem screenshot then showed a cluttered desktop and you could not tell "the
rem game is fullscreen and GDI cannot see it" from "the game never started".
exit
