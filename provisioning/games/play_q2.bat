@echo off
REM play_q2.bat - launch Quake II on the Voodoo3 the way that WORKS.
REM
REM Q2's ref_gl can only create an OpenGL context on the Voodoo3 when the DESKTOP
REM is 16-bit; a 32bpp desktop makes qwglCreateContext fail (Q3 is unaffected
REM because it forces r_colorbits 16 in its own GL init). And gl_driver=opengl32
REM loads a Direct3D-wrapper GL that GREEN-SCREENS the display driver at 1024x768.
REM So: switch the desktop to 16-bit, run Q2 via the native 3dfx path at 640x480,
REM and restore 32-bit on exit.
setlocal
set Q2=C:\Games\Quake2
"%Q2%\setmode.exe" 1024 768 16 75
cd /d "%Q2%"
start /wait quake2.exe +set vid_ref gl +set gl_driver 3dfxgl +set gl_bitdepth 16 +set gl_mode 3
"%Q2%\setmode.exe" 1024 768 32 75
endlocal
