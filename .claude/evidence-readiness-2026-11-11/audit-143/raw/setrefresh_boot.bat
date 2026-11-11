@echo off
rem This monitor's usable max at 1024x768 is 100Hz (120 is out of range).
rem Re-apply at logon so a hard power-cycle can't drop back to 60Hz.
ping -n 16 127.0.0.1 >nul
C:\setmode.exe 1024 768 32 100 >nul 2>&1
ping -n 6 127.0.0.1 >nul
C:\setmode.exe 1024 768 32 100 >nul 2>&1
