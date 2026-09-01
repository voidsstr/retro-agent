@echo off
rem Tom Clancy's Rainbow Six (1998) - fleet launcher.
rem
rem NO FLEETRES BLOCK, AND THAT IS DELIBERATE. RainbowSix.exe has exactly two
rem switch-shaped tokens in the entire image - "-server" and "-client" (VA
rem 0x898568/70). There is no resolution switch, no windowed switch and no
rem dedicated-server switch. Resolution and fullscreen are REGISTRY values, so
rem they live in install.reg; handing this exe -w/-h would do nothing at all.
rem
rem THE GAME NEEDS ITS REGISTRY VALUES OR IT FINDS NOTHING. 33 of its 34
rem asset-path defaults are compiled as \data2\* - the CD - and only
rem \data\journals defaults to disk. install.reg is what redirects them, and
rem GAMESYNC merges it at deploy time. If this box was deployed before that
rem existed, run:  regedit /s C:\Games\RainbowSix\install.reg
rem
rem DO NOT RUN THE SHIPPED regs.cmd. Every one of its 40 REG ADD lines ends
rem with /reg:32, a switch XP's reg.exe does not have, so on XP it writes
rem nothing while printing errors that scroll past. It is kept in the tree only
rem as provenance - it is part of the GOG build.
cd /d "%~dp0"
start "" RainbowSix.exe
rem Close this window - a .bat that ends at "start" leaves its console open for
rem the life of the game, and a stack of them corrupts screenshot testing.
exit
