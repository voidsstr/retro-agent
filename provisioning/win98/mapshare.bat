@echo off
:retry
ping -n 5 127.0.0.1 > nul
net use \\192.168.1.122\files password /YES
if errorlevel 1 goto retry
