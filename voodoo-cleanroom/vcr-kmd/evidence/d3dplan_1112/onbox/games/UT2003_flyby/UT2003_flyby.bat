@echo off
cd /d "C:\Games\UT2003\Benchmark"
start "" "..\System\ut2003.exe" dm-antalus?game=engine.gameinfo exec=..\Benchmark\Stuff\flybyexec.txt -benchmark -seconds=77 -nosound
exit
