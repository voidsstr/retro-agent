@echo off
set MESA_FORCE_SSE=1
cd /d "C:\Games\CounterStrike16"
hl.exe -game cstrike -full -gl -w 1280 -h 960 -condebug +exec lancheck.cfg
