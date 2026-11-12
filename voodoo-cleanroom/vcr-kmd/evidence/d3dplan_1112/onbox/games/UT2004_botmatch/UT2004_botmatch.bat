@echo off
cd /d "C:\Games\UT2004\System"
start "" "UT2004.exe" DM-Antalus?spectatoronly=true?numbots=8?quickstart=true?attractcam=true -benchmark -seconds=60
exit
