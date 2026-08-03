@echo off
rem CHAT.BAT - run the DOS retro agent + chat.
rem NETUP.BAT handles every dependency itself (packet driver, MTCPCFG,
rem DHCP) and is safe to re-run, so chat works on the first try with no
rem manual network setup - no need to run PLAY first.
call C:\DOSGAME\NETUP.BAT
if exist C:\DOSGAME\NET\PKT.OK goto run
echo.
echo Network did not come up - chat needs it. Fix the card/cable and
echo run CHAT again (see C:\DOSGAME\NET\README.TXT).
goto end
:run
C:\DOSCHAT\DOSCHAT.EXE
:end
