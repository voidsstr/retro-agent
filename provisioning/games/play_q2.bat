@echo off
REM play_q2.bat - Quake II on the Voodoo3 via OUR stable retro3dfx ICD (retrogl).
REM retrogl owns the Glide fullscreen mode itself, so NO 16-bit desktop switch is
REM needed (unlike the old stock 3dfxgl path, which was unstable and could crash
REM the display). retrogl is also faster: ~96 fps @640 vs 75.7 for stock 3dfxgl.
cd /d "C:\Games\Quake2"
start "" quake2.exe +set vid_ref gl +set gl_driver retrogl +set gl_bitdepth 16 +set gl_mode 3 +set vid_fullscreen 1
