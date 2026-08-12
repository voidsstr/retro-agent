@echo off
rem ---------------------------------------------------------------
rem AGENTRUN.BAT - the Win9x agent launcher, run from the Run key.
rem
rem Why this exists: Windows 9x cannot rename a RUNNING exe (NT can),
rem so a self-update can never swap the agent binary from inside the
rem agent. Every in-session attempt fails with "Duplicate file name
rem or file in use" and quietly leaves the old build in place.
rem
rem At logon, though, nothing holds the file yet. So the update is
rem applied HERE, before the agent starts: drop a new build next to
rem it as retro_agent_new.exe and the next boot installs it.
rem
rem Fail-safe by construction - if the rename does not work we still
rem start whatever retro_agent.exe is present, so the box can never
rem end up with no agent. On this machine that means a walk to the
rem keyboard.
rem
rem The previous build is kept as retro_agent_old.exe for rollback.
rem ---------------------------------------------------------------
if not exist C:\RETRO_AGENT\retro_agent_new.exe goto runit

echo swap:   installing staged build >> C:\RETRO_AGENT\agent.log
if exist C:\RETRO_AGENT\retro_agent_old.exe del C:\RETRO_AGENT\retro_agent_old.exe
ren C:\RETRO_AGENT\retro_agent.exe retro_agent_old.exe
if exist C:\RETRO_AGENT\retro_agent.exe goto locked
ren C:\RETRO_AGENT\retro_agent_new.exe retro_agent.exe
echo swap:   staged build installed >> C:\RETRO_AGENT\agent.log
goto runit

:locked
echo swap:   retro_agent.exe was still locked - keeping the old build >> C:\RETRO_AGENT\agent.log

:runit
rem No -l: the agent defaults to C:\RETRO_AGENT\agent.log on its own.
start C:\RETRO_AGENT\retro_agent.exe
