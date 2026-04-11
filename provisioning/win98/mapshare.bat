@echo off
rem Map network share at boot. Edit the UNC path, username, and password
rem to match your environment before deploying.
:retry
ping -n 5 127.0.0.1 > nul
net use \\YOUR-SERVER\share YOUR-PASSWORD /user:YOUR-USERNAME /YES
if errorlevel 1 goto retry
