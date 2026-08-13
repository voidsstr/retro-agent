@echo off
rem ---------------------------------------------------------------
rem MAPSHARE.BAT - map the fleet share with explicit credentials.
rem
rem Run from HKLM\...\Run at logon. It RETRIES, because a bare
rem "net use" in the Run key races the network coming up: on a cold
rem boot the mapping silently failed and the box came up with no
rem share at all, while running exactly the same command by hand a
rem minute later worked fine.
rem
rem Explicit credentials on the command line are the point - the
rem account's Windows password is blank (that is what makes Win9x
rem auto-logon skip the dialog), so the cached credential in
rem ADMIN.PWL is no longer what the NAS accepts.
rem
rem Win9x dialect only: no "call :label", no "goto :eof", no
rem setlocal, and labels must be unique within 8 characters.
rem COMMAND.COM has no sleep either; "ping -n COUNT 127.0.0.1" is
rem (no angle brackets anywhere in this file, not even in a comment:
rem  parses redirection on every line, rem included, and this one used to
rem  redirect stdin from a nonexistent file at every single logon.)
rem the standard stand-in (roughly one second per count).
rem ---------------------------------------------------------------
SET DGLOG=C:\DOSGAME\DOSGAME.LOG
echo map:    MAPSHARE.BAT starting >> %DGLOG%

net use E: \\192.168.1.122\files password /SAVEPW:NO /YES > nul
if not errorlevel 1 goto mapok
echo map:    attempt 1 failed, waiting for the network >> %DGLOG%
ping -n 6 127.0.0.1 > nul

net use E: \\192.168.1.122\files password /SAVEPW:NO /YES > nul
if not errorlevel 1 goto mapok
echo map:    attempt 2 failed, waiting longer >> %DGLOG%
ping -n 16 127.0.0.1 > nul

net use E: \\192.168.1.122\files password /SAVEPW:NO /YES > nul
if not errorlevel 1 goto mapok
echo map:    attempt 3 failed, waiting longer still >> %DGLOG%
ping -n 31 127.0.0.1 > nul

net use E: \\192.168.1.122\files password /SAVEPW:NO /YES > nul
if not errorlevel 1 goto mapok
echo map:    FAILED - share not mapped; downloads and staging unavailable >> %DGLOG%
goto mapend

:mapok
echo map:    share mapped on E: >> %DGLOG%

:mapend
