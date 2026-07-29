@echo off
rem Delayed max-refresh re-apply: wait for the display driver to settle after
rem logon, then set the monitor max refresh twice (catches a post-init reset).
rem Wired via HKLM..\Run -> setrefresh_boot.vbs (hidden) -> this. See setrefresh.c.
ping -n 16 127.0.0.1 >nul
C:\setrefresh.exe >nul 2>&1
ping -n 6 127.0.0.1 >nul
C:\setrefresh.exe >nul 2>&1
