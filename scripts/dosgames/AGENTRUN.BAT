@echo off
rem ---------------------------------------------------------------
rem AGENTRUN.BAT - the Win9x agent launcher, run from the Run key.
rem
rem WHY THIS EXISTS
rem Windows 9x cannot rename or overwrite a RUNNING exe (NT can), so
rem the agent can never swap its own binary from inside itself. Every
rem in-session attempt fails with "Duplicate file name or file in
rem use" and silently leaves the old build running - which is why
rem auto-update quietly did nothing on this box for four versions,
rem and why RESTART did not help either.
rem
rem At logon nothing holds the file yet. So the update happens HERE,
rem before the agent starts. Two sources, in order:
rem
rem   1. the share - the same latest-pointer + .ver sidecar every
rem      other box auto-updates from, so publishing a build reaches
rem      this machine too, on its next boot
rem   2. a hand-staged retro_agent_new.exe dropped next to the agent
rem
rem Fail-safe by construction: every step is skipped if its input is
rem missing, and whatever happens we start whatever retro_agent.exe
rem is present. The box can never be left with no agent - on this
rem machine that means a walk to the keyboard.
rem
rem The previous build is kept as retro_agent_old.exe for rollback.
rem ---------------------------------------------------------------
SET AGDIR=C:\RETRO_AGENT
SET AGSHARE=\\192.168.1.122\files\Utility\Retro Automation
SET AGLOG=C:\RETRO_AGENT\agent.log

echo boot:   AGENTRUN.BAT starting >> %AGLOG%

rem --- 0. get an authenticated session to the share.
rem
rem     A bare UNC path is NOT readable on Windows 9x without one: FindFirstFile
rem     and copy against \\server\share\... simply find nothing, silently. That
rem     is hardware-confirmed (the Deskpro staged 0 files while reporting
rem     success) and is why dosstage.c resolves a drive letter instead.
rem
rem     This script used to rely on MAPSHARE.BAT, a SIBLING Run-key entry, to
rem     have created that session already. Run-key order is undefined and
rem     MAPSHARE itself spends up to ~53s retrying because the network is not
rem     up at logon - so on a cold boot the check below was simply false and
rem     the entire share half of the updater was skipped, with nothing logged
rem     to say so. Map it here rather than depend on winning a race.
net use E: \\192.168.1.122\files password /YES > nul
if exist E:\Utility\RETRO_~1\retro_agent.exe.ver SET AGSHARE=E:\Utility\RETRO_~1
if exist E:\Utility\RETRO_~1\retro_agent.exe.ver echo boot:   share via E: >> %AGLOG%

rem --- 1. the share. The network may not be up yet at logon (the drive
rem     mapping races it too), so this is best-effort: if the sidecar is
rem     not readable we simply run what we have and try again next boot.
if exist "%AGSHARE%\retro_agent.exe.ver" goto haveshare
echo boot:   share not readable - skipping the update this boot >> %AGLOG%
goto trystaged
:haveshare
copy "%AGSHARE%\retro_agent.exe.ver" %AGDIR%\share.ver > nul
if not exist %AGDIR%\share.ver goto trystaged
if not exist %AGDIR%\agent.ver goto pullit
fc %AGDIR%\agent.ver %AGDIR%\share.ver > nul
if not errorlevel 1 goto uptodate

:pullit
echo boot:   share has a different version - pulling it >> %AGLOG%
copy "%AGSHARE%\retro_agent.exe" %AGDIR%\retro_agent_new.exe > nul
if not exist %AGDIR%\retro_agent_new.exe goto trystaged
copy %AGDIR%\share.ver %AGDIR%\agent.ver > nul
goto trystaged

:uptodate
echo boot:   already on the share's version >> %AGLOG%

rem --- 2. install a staged build (from the share above, or dropped by hand)
:trystaged
if not exist %AGDIR%\retro_agent_new.exe goto runit
echo boot:   installing staged build >> %AGLOG%
if exist %AGDIR%\retro_agent_old.exe del %AGDIR%\retro_agent_old.exe
ren %AGDIR%\retro_agent.exe retro_agent_old.exe
if exist %AGDIR%\retro_agent.exe goto locked
ren %AGDIR%\retro_agent_new.exe retro_agent.exe
echo boot:   staged build installed >> %AGLOG%
goto runit

:locked
echo boot:   retro_agent.exe still locked - keeping the old build >> %AGLOG%

:runit
rem No -l: the agent defaults to C:\RETRO_AGENT\agent.log on its own.
echo boot:   starting the agent >> %AGLOG%
start %AGDIR%\retro_agent.exe
