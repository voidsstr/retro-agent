@echo off
set RETRO_GLIDE_MAPLOG=C:\RETRO_AGENT\cr\maplog.txt
set SSTH3_SLI_AA_CONFIGURATION=5
set FX_GLIDE_SWAPINTERVAL=0
cd /d "C:\Games\Quake2Complete"
quake2.exe +set basedir "C:\Games\Quake2Complete" +set vid_ref gl +set gl_driver retrogl +set gl_mode 9 +set gl_bitdepth 16 +set vid_fullscreen 1 +set logfile 2 +exec bench.cfg
