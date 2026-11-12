@echo off
set SSTH3_SLI_AA_CONFIGURATION=5
set FX_GLIDE_SWAPINTERVAL=0
cd /d "C:\Games\Quake3-TeamArena"
quake3.exe +set fs_basepath "C:\Games\Quake3-TeamArena" +set fs_homepath "C:\Games\Quake3-TeamArena" +set logfile 2 +set r_glDriver 3dfxogl +set r_mode -1 +set r_customwidth 1024 +set r_customheight 768 +set r_customaspect 1 +set r_colorbits 16 +set r_depthbits 16 +set r_texturebits 16 +set r_stencilbits 0 +set r_fullscreen 1 +set r_picmip 0 +exec bench.cfg
