@echo off
rem ---------------------------------------------------------------
rem PLAY.BAT - the one command to run in MS-DOS mode.
rem
rem Brings networking up (so games can be downloaded from the share)
rem and then enters the game menu. Games already on the hard drive
rem play fine even with no network, so a failed detect is never fatal.
rem
rem Each candidate driver gets its OWN packet interrupt. A Crynwr
rem driver that fails to find its card still disturbs the vector it
rem was given, and a later driver loading on that same vector comes up
rem half-broken - DHCP then just times out with no clue why (seen in
rem DOSBox, 2026-07-29). Whichever driver loads, we write its
rem interrupt into MTCP.CFG so DHCP/HTGET talk to the right one.
rem
rem Order matters too: 3C509 and NE2000 detect cleanly, while the ancient
rem 3C50x drivers probe hard enough to claim a card that isn't theirs
rem (3C503 grabbed an NE2000 in DOSBox), so they are tried last.
rem ---------------------------------------------------------------
SET MTCPCFG=C:\DOSGAME\NET\MTCP.CFG

echo.
echo Looking for a network card...

C:\DOSGAME\NET\3C509.COM 0x60 > nul
if errorlevel 1 goto try2
echo PACKETINT 0x60 > C:\DOSGAME\NET\MTCP.CFG
goto gotpkt

:try2
C:\DOSGAME\NET\NE2000.COM 0x61 10 0x300 > nul
if errorlevel 1 goto try3
echo PACKETINT 0x61 > C:\DOSGAME\NET\MTCP.CFG
goto gotpkt

:try3
C:\DOSGAME\NET\NE2000.COM 0x62 3 0x300 > nul
if errorlevel 1 goto try4
echo PACKETINT 0x62 > C:\DOSGAME\NET\MTCP.CFG
goto gotpkt

:try4
C:\DOSGAME\NET\NE2000.COM 0x63 5 0x280 > nul
if errorlevel 1 goto try5
echo PACKETINT 0x63 > C:\DOSGAME\NET\MTCP.CFG
goto gotpkt

:try5
C:\DOSGAME\NET\3C503.COM 0x64 > nul
if errorlevel 1 goto try6
echo PACKETINT 0x64 > C:\DOSGAME\NET\MTCP.CFG
goto gotpkt

:try6
C:\DOSGAME\NET\3C505.COM 0x65 > nul
if errorlevel 1 goto try7
echo PACKETINT 0x65 > C:\DOSGAME\NET\MTCP.CFG
goto gotpkt

:try7
C:\DOSGAME\NET\3C507.COM 0x66 > nul
if errorlevel 1 goto try8
echo PACKETINT 0x66 > C:\DOSGAME\NET\MTCP.CFG
goto gotpkt

:try8
C:\DOSGAME\NET\3C501.COM 0x67 > nul
if errorlevel 1 goto nopkt
echo PACKETINT 0x67 > C:\DOSGAME\NET\MTCP.CFG
goto gotpkt

:nopkt
echo No network card detected - installed games still play fine.
echo (Downloads need a packet driver: see C:\DOSGAME\NET\README.TXT)
goto menu

:gotpkt
echo HOSTNAME DOSBOX >> C:\DOSGAME\NET\MTCP.CFG
echo Network card found. Getting an IP address...
C:\DOSGAME\NET\DHCP.EXE
if errorlevel 1 goto nodhcp
goto menu

:nodhcp
echo No DHCP reply - downloads will not work this session.

:menu
call C:\DOSGAME.BAT
