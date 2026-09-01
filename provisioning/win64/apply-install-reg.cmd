@echo off
setlocal
rem ==========================================================================
rem apply-install-reg.cmd  -  merge a staged title's install.reg CORRECTLY,
rem                           on 32-bit Windows and on 64-bit Windows alike.
rem
rem     apply-install-reg.cmd "D:\Games\Halo"
rem     apply-install-reg.cmd            (uses the directory it sits in)
rem
rem WHY THIS EXISTS
rem ---------------
rem 25 of the library's 30 install.reg files seed keys under
rem HKEY_LOCAL_MACHINE\SOFTWARE, and NONE of them mention Wow6432Node.
rem
rem On the fleet that is exactly right: every fleet box is 32-bit, there is no
rem redirection, and GAMESYNC's `regedit /s` puts the keys where the game looks.
rem
rem On 64-bit Windows it is exactly wrong. regedit.exe is a 64-bit process, so
rem it writes the 64-bit view - while the GAME is 32-bit and reads through
rem HKLM\SOFTWARE\Wow6432Node. The keys are present and the game cannot see
rem one of them. Measured on Windows 11: Halo imported the default way sits on
rem its EULA forever; imported with /reg:32 it reaches the main menu.
rem
rem THE TRAP THIS AVOIDS, WHICH IS THE SAME FLAG IN REVERSE
rem -------------------------------------------------------
rem /reg:32 DOES NOT EXIST ON WINDOWS XP - it arrived with Vista. GOG's own
rem regs.cmd for Rainbow Six ends all 40 of its lines with /reg:32, and on XP
rem every one of them fails silently, which is why the fleet ships a generated
rem .reg applied with `regedit /s` instead. So: same switch, opposite sign,
rem depending on the OS. This script picks per machine instead of guessing.
rem ==========================================================================

set "DIR=%~1"
if not defined DIR set "DIR=%~dp0"
if "%DIR:~-1%"=="\" set "DIR=%DIR:~0,-1%"
set "REGFILE=%DIR%\install.reg"

if not exist "%REGFILE%" (
    echo   No install.reg in "%DIR%" - nothing to do.
    exit /b 0
)

rem PROCESSOR_ARCHITEW6432 is set only inside a 32-bit process on 64-bit
rem Windows, so testing BOTH catches every combination of cmd and OS.
set "IS64="
if /i "%PROCESSOR_ARCHITECTURE%"=="AMD64" set "IS64=1"
if /i "%PROCESSOR_ARCHITECTURE%"=="ARM64" set "IS64=1"
if defined PROCESSOR_ARCHITEW6432 set "IS64=1"

if not defined IS64 (
    echo   32-bit Windows - plain regedit, no redirection to worry about.
    regedit /s "%REGFILE%"
    if errorlevel 1 echo   WARNING: regedit reported an error.
    exit /b 0
)

echo   64-bit Windows - importing into BOTH registry views.
rem 32-bit view FIRST: that is the one a 32-bit game actually reads.
reg import "%REGFILE%" /reg:32
if errorlevel 1 (
    echo   ERROR: `reg import /reg:32` failed. Without it a 32-bit game
    echo          cannot see these keys on this machine.
    exit /b 1
)
rem And the 64-bit view too, harmlessly, so anything 64-bit that looks for the
rem title (a launcher, a shell extension) still finds it.
reg import "%REGFILE%" >nul 2>&1
echo   Done - keys are visible to both 32-bit and 64-bit readers.
exit /b 0
