@echo off
set SSTH3_SLI_AA_CONFIGURATION=5
set FX_GLIDE_SWAPINTERVAL=0
cd /d "C:\Games\Quake1"
GLQUAKE.EXE -width 1024 -height 768 -bpp 32 -fullscreen -condebug +exec bench.cfg
