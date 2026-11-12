@echo off
rem TEMPORARY test launcher (q3-195 1080p diagnosis) - same args as Play Quake III Arena.bat plus vm_ui 0. Delete after use.
cd /d "C:\Games\Quake3-TeamArena"
start "" ioquake3.x86.exe +set r_mode -1 +set r_customwidth 1920 +set r_customheight 1080 +set r_customaspect 1 +set r_customPixelAspect 1 +set r_fullscreen 1 +set cg_fov 106 +set vm_ui 0
exit
