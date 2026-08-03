@echo off
rem ---------------------------------------------------------------
rem PLAY.BAT - the one command to run in MS-DOS mode.
rem
rem NETUP.BAT brings networking up (so games can be downloaded from
rem the share) and then we enter the game menu. Games already on the
rem hard drive play fine even with no network, so a failed detect is
rem never fatal.
rem ---------------------------------------------------------------
call C:\DOSGAME\NETUP.BAT
call C:\DOSGAME.BAT
