@echo off
set RETRO_GLIDE_MAPLOG=C:\RETRO_AGENT\cr\maplog.txt
set SSTH3_SLI_AA_CONFIGURATION=5
set FX_GLIDE_SWAPINTERVAL=0
cd /d "C:\Games\ReturnToCastleWolfenstein"
WolfMP.exe +set fs_basepath "C:\Games\ReturnToCastleWolfenstein" +set fs_homepath "C:\Games\ReturnToCastleWolfenstein" +set logfile 2 +set r_glIgnoreWicked3D 1 +set r_glDriver retrogl +set r_mode 3 +set r_fullscreen 1 +set r_colorbits 32 +set r_texturebits 32 +set r_depthbits 24 +set r_picmip 0 +set r_swapInterval 0 +set com_maxfps 0 +set sv_pure 0 +set s_initsound 0 +set timedemo 1 +demo wolfbench
