@echo off
rem SWAP.BAT - install a staged agent build on a Win9x box.
rem
rem Win98 cannot rename a RUNNING exe (NT can), so the swap has to happen
rem after the agent exits - and the thing doing it therefore cannot be the
rem agent. This batch is LAUNCHed detached first; the agent is then told to
rem quit, and this orphan outlives it, does the rename, and starts the new
rem build.
rem
rem Fail-safe by construction: if the rename does not work for any reason we
rem still start whatever retro_agent.exe is there, so the box can never be
rem left with no agent - which on this machine means a walk to the keyboard.
echo swap:   SWAP.BAT starting >> C:\RETRO_AGENT\agent.log
ping -n 12 127.0.0.1 > nul

if not exist C:\RETRO_AGENT\retro_agent_new.exe goto runit
del C:\RETRO_AGENT\retro_agent_122.exe
ren C:\RETRO_AGENT\retro_agent.exe retro_agent_122.exe
if exist C:\RETRO_AGENT\retro_agent.exe goto stillrun
ren C:\RETRO_AGENT\retro_agent_new.exe retro_agent.exe
echo swap:   installed the staged build >> C:\RETRO_AGENT\agent.log
goto runit

:stillrun
echo swap:   old exe still locked - starting it unchanged >> C:\RETRO_AGENT\agent.log

:runit
echo swap:   starting the agent >> C:\RETRO_AGENT\agent.log
start C:\RETRO_AGENT\retro_agent.exe
