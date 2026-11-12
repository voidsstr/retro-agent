@echo off
set MESA_FORCE_SSE=1
set SSTH3_SLI_AA_CONFIGURATION=0
set FX_GLIDE_SWAPINTERVAL=0
set FX_SGIS_MULTITEXTURE=0
cd /d "C:\Games\CounterStrike16"
hl.exe -game cstrike -gl -full -w 640 -h 480 -condebug -noipx -nojoy +fps_max 999 +gl_vsync 0 +timedemo cs16_bench
