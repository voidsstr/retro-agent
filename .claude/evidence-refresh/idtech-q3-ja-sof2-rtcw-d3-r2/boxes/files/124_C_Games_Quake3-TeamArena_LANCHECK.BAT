@echo off
cd /d "C:\Games\Quake3-TeamArena"
ioquake3.x86.exe +set r_mode -1 +set r_customwidth 1280 +set r_customheight 960 +set r_customaspect 1 +set r_customPixelAspect 1 +set r_fullscreen 1 +set cg_fov 90 +set logfile 2 +exec lancheck.cfg
