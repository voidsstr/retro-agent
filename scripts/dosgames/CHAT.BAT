@echo off
rem CHAT.BAT - run the DOS retro agent + chat (needs networking).
rem PLAY.BAT already brings the network up; run that first, or this
rem will bring it up the same way.
SET MTCPCFG=C:\DOSGAME\NET\MTCP.CFG
if exist C:\DOSGAME\NET\PKT.OK goto run
echo Network not up yet - run PLAY first (it loads the packet driver),
echo or load a packet driver and DHCP by hand.
goto end
:run
C:\DOSCHAT\DOSCHAT.EXE
:end
