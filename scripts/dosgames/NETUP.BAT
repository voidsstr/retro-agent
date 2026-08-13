@echo off
rem ---------------------------------------------------------------
rem NETUP.BAT - bring DOS networking up (packet driver + DHCP).
rem Called automatically by PLAY.BAT and CHAT.BAT, so the user never
rem has to run network setup by hand. Safe to call repeatedly:
rem C:\DOSGAME\NET\PKT.OK marks a live session, and because that
rem marker survives a reboot it is verified with a quick DHCP probe
rem (mTCP fails fast when no packet driver answers) before trusting it.
rem
rem EVERY step logs to C:\DOSGAME\DOSGAME.LOG, the same file DOSGAME.EXE
rem and RUN.BAT write to. Network bring-up used to be completely silent,
rem so when PLAY died here there was no evidence at all of how far it
rem got - the log was empty because the menu had never been reached.
rem
rem Each candidate driver gets its OWN packet interrupt. A Crynwr
rem driver that fails to find its card still disturbs the vector it
rem was given, and a later driver loading on that same vector comes up
rem half-broken - DHCP then just times out with no clue why (seen in
rem DOSBox, 2026-07-29). Whichever driver loads, we write its
rem interrupt into MTCP.CFG so DHCP/HTGET talk to the right one.
rem
rem Only drivers actually present on disk are tried: COMMAND.COM does
rem NOT set errorlevel for a missing file, so running an absent .COM
rem both printed "Bad command or file name" and made the errorlevel
rem check think the driver LOADED (writing a dead PACKETINT).
rem
rem Order matters too: 3C509 and NE2000 detect cleanly, while the ancient
rem 3C50x drivers probe hard enough to claim a card that isn't theirs
rem (3C503 grabbed an NE2000 in DOSBox), so they are tried last.
rem ---------------------------------------------------------------
SET MTCPCFG=C:\DOSGAME\NET\MTCP.CFG
SET DGLOG=C:\DOSGAME\DOSGAME.LOG
echo net:    NETUP.BAT starting >> %DGLOG%

if not exist C:\DOSGAME\NET\PKT.OK goto detect
rem Marker present - could be stale from an earlier boot. A DHCP probe
rem settles it: instant failure if the packet driver is gone, and a
rem harmless lease refresh if it is still resident.
if not exist C:\DOSGAME\NET\DHCP.EXE goto detect
echo net:    PKT.OK present, probing with DHCP >> %DGLOG%
rem mTCP's DHCP advertises "Press [ESC] to abort" and takes ANY pending
rem keystroke as that ESC. A key left in the BIOS buffer by the menu or by a
rem "press a key" prompt therefore kills the lease request the instant it
rem starts - the log showed "attempt 1: Aborting" immediately, which reads
rem like a dead network but is nothing of the sort. Drain the buffer first.
if exist C:\DOSGAME\DOSGAME.EXE C:\DOSGAME\DOSGAME.EXE /kflush
C:\DOSGAME\NET\DHCP.EXE >> %DGLOG%
if errorlevel 1 goto stale
echo net:    existing packet driver still good >> %DGLOG%
goto netdone

:stale
echo net:    PKT.OK was stale, re-detecting >> %DGLOG%
del C:\DOSGAME\NET\PKT.OK

:detect
echo.
echo Looking for a network card...
echo net:    detecting a packet driver >> %DGLOG%

if not exist C:\DOSGAME\NET\3C509.COM goto try2
echo net:    trying 3C509 on int 0x60 >> %DGLOG%
C:\DOSGAME\NET\3C509.COM 0x60 > nul
if errorlevel 1 goto try2
echo PACKETINT 0x60> C:\DOSGAME\NET\MTCP.CFG
echo net:    3C509 loaded on int 0x60 >> %DGLOG%
goto gotpkt

:try2
if not exist C:\DOSGAME\NET\NE2000.COM goto try5
echo net:    trying NE2000 int 0x61 irq 10 io 0x300 >> %DGLOG%
C:\DOSGAME\NET\NE2000.COM 0x61 10 0x300 > nul
if errorlevel 1 goto try3
echo PACKETINT 0x61> C:\DOSGAME\NET\MTCP.CFG
echo net:    NE2000 loaded on int 0x61 >> %DGLOG%
goto gotpkt

:try3
echo net:    trying NE2000 int 0x62 irq 3 io 0x300 >> %DGLOG%
C:\DOSGAME\NET\NE2000.COM 0x62 3 0x300 > nul
if errorlevel 1 goto try4
echo PACKETINT 0x62> C:\DOSGAME\NET\MTCP.CFG
echo net:    NE2000 loaded on int 0x62 >> %DGLOG%
goto gotpkt

:try4
echo net:    trying NE2000 int 0x63 irq 5 io 0x280 >> %DGLOG%
C:\DOSGAME\NET\NE2000.COM 0x63 5 0x280 > nul
if errorlevel 1 goto try5
echo PACKETINT 0x63> C:\DOSGAME\NET\MTCP.CFG
echo net:    NE2000 loaded on int 0x63 >> %DGLOG%
goto gotpkt

:try5
if not exist C:\DOSGAME\NET\3C503.COM goto try6
echo net:    trying 3C503 on int 0x64 >> %DGLOG%
C:\DOSGAME\NET\3C503.COM 0x64 > nul
if errorlevel 1 goto try6
echo PACKETINT 0x64> C:\DOSGAME\NET\MTCP.CFG
echo net:    3C503 loaded on int 0x64 >> %DGLOG%
goto gotpkt

:try6
if not exist C:\DOSGAME\NET\3C505.COM goto try7
echo net:    trying 3C505 on int 0x65 >> %DGLOG%
C:\DOSGAME\NET\3C505.COM 0x65 > nul
if errorlevel 1 goto try7
echo PACKETINT 0x65> C:\DOSGAME\NET\MTCP.CFG
echo net:    3C505 loaded on int 0x65 >> %DGLOG%
goto gotpkt

:try7
if not exist C:\DOSGAME\NET\3C507.COM goto try8
echo net:    trying 3C507 on int 0x66 >> %DGLOG%
C:\DOSGAME\NET\3C507.COM 0x66 > nul
if errorlevel 1 goto try8
echo PACKETINT 0x66> C:\DOSGAME\NET\MTCP.CFG
echo net:    3C507 loaded on int 0x66 >> %DGLOG%
goto gotpkt

:try8
if not exist C:\DOSGAME\NET\3C501.COM goto nopkt
echo net:    trying 3C501 on int 0x67 >> %DGLOG%
C:\DOSGAME\NET\3C501.COM 0x67 > nul
if errorlevel 1 goto nopkt
echo PACKETINT 0x67> C:\DOSGAME\NET\MTCP.CFG
echo net:    3C501 loaded on int 0x67 >> %DGLOG%
goto gotpkt

:nopkt
echo No network card detected - installed games still play fine.
echo (Downloads and chat need a packet driver: see C:\DOSGAME\NET\README.TXT)
echo net:    NO packet driver loaded - downloads unavailable >> %DGLOG%
goto netdone

:gotpkt
echo HOSTNAME DOSBOX>> C:\DOSGAME\NET\MTCP.CFG
echo Network card found. Getting an IP address...
echo net:    requesting a DHCP lease >> %DGLOG%
if not exist C:\DOSGAME\NET\DHCP.EXE goto nodhcpe
rem mTCP's DHCP advertises "Press [ESC] to abort" and takes ANY pending
rem keystroke as that ESC. A key left in the BIOS buffer by the menu or by a
rem "press a key" prompt therefore kills the lease request the instant it
rem starts - the log showed "attempt 1: Aborting" immediately, which reads
rem like a dead network but is nothing of the sort. Drain the buffer first.
if exist C:\DOSGAME\DOSGAME.EXE C:\DOSGAME\DOSGAME.EXE /kflush
C:\DOSGAME\NET\DHCP.EXE >> %DGLOG%
if errorlevel 1 goto nodhcp
echo ok > C:\DOSGAME\NET\PKT.OK
echo net:    DHCP lease obtained - downloads should work >> %DGLOG%
goto netdone

:nodhcpe
echo DHCP.EXE missing from C:\DOSGAME\NET - re-stage the DOS tools.
echo net:    DHCP.EXE is missing >> %DGLOG%
goto netdone

:nodhcp
echo No DHCP reply - downloads and chat will not work this session.
echo net:    NO DHCP reply - downloads unavailable >> %DGLOG%

:netdone
echo net:    NETUP.BAT done >> %DGLOG%
