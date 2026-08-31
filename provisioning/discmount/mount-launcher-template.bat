rem ==========================================================================
rem  THIS FILE IS THE FLEET'S CANONICAL DISC-MOUNT LAUNCHER TEMPLATE.
rem
rem  Do not edit a title's generated copy - edit THIS file and regenerate, or
rem  the next fix lands in one launcher and not the other four.
rem      scripts/fleet/make-mount-launcher.py --spec <title>.json --out "Play X.bat"
rem      scripts/fleet/stage-discmount.py            (regenerates every title)
rem
rem  Placeholders (written here without their doubled-at markers so this
rem  comment is not itself substituted): TITLE, FLEETRES, VAR_GTITLE,
rem  VAR_IMAGE, VAR_VOLID, VAR_MARKER, VAR_GAME, VAR_GAMEARGS, AUTOKILL,
rem  PRELAUNCH.
rem
rem  Derived verbatim from Games-Library/RedFaction/"Play Red Faction.bat",
rem  which was proven on hardware; every comment below is that file's and is
rem  kept because each paragraph records a failure that really happened.
rem ==========================================================================
rem @@TEMPLATE_BANNER_END@@
@echo off
rem ==========================================================================
rem  @@TITLE@@ - resilient disc-image mount + launch   (fleet template v2)
rem
rem  This title wants a CD in a drive. The disc image ships INSIDE this tree,
rem  so the whole thing relocates: every path below is relative to %~dp0 (the
rem  folder this .bat lives in), never to C:\Games\<Title>.
rem
rem  "Resilient" means, in order:
rem    1. If the disc is ALREADY visible in any drive - real or virtual - do
rem       not mount anything, just play. (Tolerates "already mounted", which
rem       is the normal state on the second launch.)
rem    2. Find a mounter. TWO products, because the fleet has both: Daemon
rem       Tools (.145 has C:\Program Files\D-Tools\daemon.exe) and WinCDEmu
rem       (.246 has C:\Program Files\WinCDEmu\batchmnt.exe and NO Daemon
rem       Tools). Each is located by probing the usual install paths, then the
rem       registry, then a bounded search of Program Files. No hard-coded path.
rem    3. Make sure a virtual drive exists, trying each generation's syntax.
rem    4. Mount. Daemon Tools has changed its -mount spelling across versions,
rem       so all four are tried; WinCDEmu just takes the image path. If Daemon
rem       Tools was found but produced no drive, WinCDEmu is STILL tried - a
rem       box can have both and the one that works is the one that matters.
rem    5. WAIT for the drive letter to actually appear. A mount is
rem       asynchronous; starting the game too early is the classic spurious
rem       "please insert the CD".
rem    6. Kill the disc's autorun. Windows starts AUTORUN.INF a moment after
rem       the drive appears and the installer window lands ON TOP of the game.
rem       Killing it before the shell has started it is a no-op, so this waits
rem       first, then kills, then starts the game.
rem    7. On failure say exactly what went wrong - on screen AND in
rem       mount-error.txt beside this file, so an agent can DOWNLOAD it. The
rem       two failures are reported DIFFERENTLY on purpose: "no mounter is
rem       installed" and "a mounter ran but no drive appeared" are different
rem       calls to action, and collapsing them is how an install problem gets
rem       mistaken for a mount problem.
rem
rem  IF THE SHARE ONLY HAS A BARE .bin AND NO .cue, WRITE THE CUE YOURSELF.
rem  Daemon Tools cannot mount a raw .bin, which makes an image that is right
rem  there look as though it does not exist - that is what had Red Faction
rem  marked BLOCKED. Read the geometry instead of guessing:
rem    * size divides exactly by 2352, the bytes 00 FF*10 00 sit at offset 0,
rem      and "CD001" is at 16*2352+16   ->  MODE1/2352
rem    * size divides exactly by 2048 and "CD001" is at 16*2048
rem                                     ->  MODE1/2048  (a plain .iso)
rem  then the cue is three lines:
rem      FILE "<name>.bin" BINARY
rem        TRACK 01 MODE1/2352
rem          INDEX 01 00:00:00
rem  The volume label and a unique marker come from the same place: the PVD at
rem  sector 16 carries the label at offset 40, and its root directory record at
rem  offset 156 points at the root listing.
rem ==========================================================================
setlocal enableextensions
@@FLEETRES@@

cd /d "%~dp0"

set "GTITLE=@@VAR_GTITLE@@"
set "IMAGE=@@VAR_IMAGE@@"
set "VOLID=@@VAR_VOLID@@"
set "MARKER=@@VAR_MARKER@@"
set "GAME=@@VAR_GAME@@"
set "GAMEARGS=@@VAR_GAMEARGS@@"
set "REQUIREDISC=@@VAR_REQUIREDISC@@"
set "ERRFILE=%~dp0mount-error.txt"
if exist "%ERRFILE%" del "%ERRFILE%" >nul 2>&1

if not exist "%GAME%" (
    call :fail "The game executable is missing: %GAME%"
    goto :theend
)

rem ---- 1. is the disc already there? -------------------------------------
call :finddisc
if defined DISCDRV (
    echo [%GTITLE%] disc already present in %DISCDRV%: - not mounting.
    goto :play
)

if not exist "%IMAGE%" (
    call :fail "The disc image is missing from the game folder: %IMAGE% -- the staged tree is incomplete. Re-run GAMESYNC for this title."
    goto :theend
)

rem ---- 2. find a mounter -------------------------------------------------
call :finddt
call :findwcd
rem BOTH failure branches must gate on REQUIREDISC, and for a long time only
rem the other one did. This branch was unconditional, so a shortcut that does
rem NOT need its disc still refused to launch on a box with no mounter at all -
rem and that is not hypothetical: .123 has no mounter AND no optical drive, and
rem it hosted a verified three-box LAN game, because a dedicated-server binary
rem typically carries no CD check. Refusing there would have thrown away a
rem proven capability to enforce a requirement the shortcut does not have.
rem Caught by the serioussam agent while migrating its launchers onto this
rem template; the asymmetry IS the bug, which is why the test covers both.
if not defined DT if not defined WCD (
    if "%REQUIREDISC%"=="1" (
        call :fail "NO DISC MOUNTER IS INSTALLED on this machine. Looked for Daemon Tools (Program Files\D-Tools, DAEMON Tools, DAEMON Tools Lite; registry App Paths daemon.exe and DTLite.exe; HKLM\SOFTWARE\DT Soft) and for WinCDEmu (Program Files\WinCDEmu batchmnt64.exe and batchmnt.exe; registry Uninstall\WinCDEmu InstallLocation), then searched Program Files for both. Install one - this is an INSTALL problem, not a mount problem."
        goto :theend
    )
    echo [%GTITLE%] no disc mounter on this box - starting anyway; this
    echo             shortcut is marked as not needing the disc.
    goto :play
)
if defined DT  echo [%GTITLE%] Daemon Tools: %DT%
if defined WCD echo [%GTITLE%] WinCDEmu:     %WCD%

rem ---- 3/4. mount, with the ONE switch this build understands ------------
rem
rem  *** NEVER SPRAY SWITCHES AT DAEMON TOOLS. ***
rem  An earlier version of this script tried every documented -mount spelling
rem  in turn and ignored the failures. Daemon Tools 3.x answers an UNSUPPORTED
rem  switch with a MODAL DIALOG, which >nul 2>&1 cannot suppress and which then
rem  blocks every later daemon.exe call - System Shock 2 died on exactly that
rem  (box-240, 2026-08-29). So: work out which build this is, issue one call,
rem  and if that does not work, say so.
rem
rem  Which build is which is decided by where it is installed, because that is
rem  the one thing observable from a batch file:
rem     ...\D-Tools\daemon.exe        Daemon Tools 3.47 - the fleet standard,
rem                                   what the install-utility skill installs.
rem                                   Syntax:  -mount 0,"image"
rem     ...\DAEMON Tools\daemon.exe   Daemon Tools 4.x.  -mount dt, 0, "image"
rem     ...DTLite.exe                 Daemon Tools Lite.  -mount dt, 0, "image"
rem  An unrecognised path falls back to the 3.47 form (the fleet standard) and
rem  that assumption is recorded in mount-error.txt if it then fails.
rem Suppress AutoPlay for this user BEFORE the disc appears. Mounting an image
rem throws a modal AutoPlay window over the running game on XP and Win7 alike,
rem and taskkilling the autorun executable afterwards does not stop the shell's
rem own dialog. 255 = every drive type. HKCU only, so it needs no administrator
rem and cannot affect anyone else's session; best effort, ignored if reg.exe is
rem absent (Win9x).
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer" /v NoDriveTypeAutoRun /t REG_DWORD /d 255 /f >nul 2>&1

echo [%GTITLE%] mounting "%IMAGE%" ...
if defined DT call :domount
if defined DISCDRV goto :mounted

rem Daemon Tools absent, or present and unproductive - WinCDEmu takes just the
rem image path, no device number and no spelling zoo.
if defined WCD (
    echo [%GTITLE%] trying WinCDEmu ...
    rem WinCDEmu's exit code IS meaningful (216 = wrong architecture, the bug
    rem that once started Brood War against the SHOGO disc), so unlike Daemon
    rem Tools this one is called directly and its code is checked.
    "%WCD%" "%IMAGE%" >nul 2>&1
    if errorlevel 1 echo [%GTITLE%] WinCDEmu returned %errorlevel% - wrong architecture?
    call :waitdisc 12
)
rem The OTHER architecture's binary, if we have it. Exit 216 from a
rem wrong-architecture batchmnt is the case this exists for.
if not defined DISCDRV if defined WCD2 (
    echo [%GTITLE%] retrying with %WCD2% ...
    "%WCD2%" "%IMAGE%" >nul 2>&1
    call :waitdisc 12
)
if defined DISCDRV goto :mounted

rem ---- 7. nothing appeared ------------------------------------------------
rem Before giving up: is there ANY disc in ANY optical drive? These checks are
rem looser than they look - on .240 Brood War ran happily with only the
rem STARCRAFT-labelled disc mounted, so the game does not verify that the disc
rem matches. A launcher that refuses where the game would have run is its own
rem bug, so try it and say plainly that that is what happened.
call :anydisc
if defined ANYDRV (
    echo.
    echo   ============================================================
    echo    %GTITLE%: THE MOUNT FAILED.
    echo   ============================================================
    echo    "%IMAGE%" did NOT mount. %ANYDRV%: has SOME disc in it, which may
    echo    be a COMPLETELY DIFFERENT GAME's disc, and the game is being
    echo    started against that. If it behaves oddly, this is why.
    echo    This is a FAILURE that is being tolerated, not a success.
    echo   ============================================================
    echo.
    > "%ERRFILE%" echo %GTITLE%: MOUNT FAILED - started against an unrelated disc
    >>"%ERRFILE%" echo image=%IMAGE%
    >>"%ERRFILE%" echo fell back to whatever disc was in %ANYDRV%:
    >>"%ERRFILE%" echo daemon=%DT%
    >>"%ERRFILE%" echo wincdemu=%WCD%  fallback=%WCD2%
    goto :play
)

if "%REQUIREDISC%"=="1" (
    call :fail "A MOUNTER WAS FOUND BUT NO DRIVE APPEARED within about 30 seconds, and no other disc is in any optical drive either. This is a mount problem, not a missing-software problem. Mount the image by hand (Daemon Tools tray icon - Device 0 - Mount image, or double-click the image with WinCDEmu) and run the game."
    goto :theend
)
echo [%GTITLE%] WARNING: a mounter was found but no drive appeared - starting
echo             anyway. This title usually runs without the disc. If it asks
echo             for the CD, mount the image by hand and try again.
goto :play

:mounted
rem ---- 6. the disc's own autorun would land on top of the game ------------
ping -n 4 127.0.0.1 >nul
for %%K in (@@AUTOKILL@@) do taskkill /f /im %%K >nul 2>&1
goto :play

:play
if defined DISCDRV echo [%GTITLE%] disc is on %DISCDRV%:
rem ---- the two per-title hooks -------------------------------------------
rem
rem  %DISCDRV% IS IN SCOPE IN BOTH, and it is the honest way to say "only if a
rem  disc actually mounted": it holds the drive letter carrying THIS title's
rem  disc, and is UNDEFINED when no disc was found - including when the mount
rem  failed and we fell through to :anydisc, which is the case that matters.
rem  A hook that cannot express that would push a title back into a bespoke
rem  launcher anyway, so it is stated here rather than left to be discovered.
rem
rem  So a title whose LAN host runs a dedicated server and then a local client
rem  ONLY when a disc is present - which is what lets a box with no mounter at
rem  all still host for the others - writes exactly that:
rem
rem      GAME       = DedicatedServer.exe
rem      postlaunch = ping -n 17 127.0.0.1 >nul
rem                   if defined DISCDRV start "" /D "%~dp0" "%~dp0game.exe"
rem
rem  And a JOIN launcher resolves its host address in the FLEETRES slot, which
rem  runs BEFORE the `set "GAMEARGS=..."` line - cmd expands %HOSTIP% at the
rem  moment that set runs, so resolving it later would silently produce
rem  "+connect" with no address. See specs/SoldierOfFortune-Join.json for a
rem  worked example, including the argv / lanhost.txt / C:\Games\lanhost.txt
rem  order that avoids a `set /p` prompt hanging a headless box forever.
rem ------------------------------------------------------------------------
rem Per-title pre-launch step, if this title needs one.
@@PRELAUNCH@@
start "" /D "%~dp0" "%GAME%" %GAMEARGS%
rem Per-title post-launch step, if this title needs one.
@@POSTLAUNCH@@
goto :theend

rem ======================== subroutines ====================================

rem Issue exactly ONE mount call, in the spelling this build understands.
rem This is a subroutine rather than an if-block on purpose: inside a
rem parenthesised block cmd expands %DTKIND% once, at parse time, so every
rem test would see the value the variable had BEFORE the block ran.
:domount
set "DTKIND="
echo %DT% | find /i "\D-Tools\" >nul && set "DTKIND=347"
if not defined DTKIND echo %DT% | find /i "DTLite.exe" >nul && set "DTKIND=lite"
if not defined DTKIND echo %DT% | find /i "\DAEMON Tools\" >nul && set "DTKIND=4x"
if not defined DTKIND set "DTKIND=347"
echo [%GTITLE%] Daemon Tools flavour: %DTKIND%
rem *** NEVER WAIT ON daemon.exe ITSELF - `start /b`, then poll. ***
rem
rem A DAEMON Tools UNIT CAN BE LOCKED. On .124 and .240 (2026-08-31) device 0
rem answered "Unable to mount image. Unit is locked." - and so did -unmount, and
rem neither d347bus nor d347prt will stop (kernel drivers), so there is no
rem reboot-free way to clear it.
rem
rem WHAT IT IS NOT: "cannot mount over an occupied unit". Refuted - .240 swapped
rem its single virtual drive SHOGO -> SERIOUS_SAM_RC2 -> SamSE with no unmount
rem at all. Occupancy is fine; a lock is a lock, and an unmount-first fix would
rem have been ceremony that fixed nothing.
rem
rem LIKELIEST CAUSE, and the reproducible one: TWO AGENTS MOUNTING AT ONCE.
rem .124 locked at the moment a Serious Sam launcher and a Jedi Academy launcher
rem raced each other. That needs no SafeDisc title and can be reproduced on
rem demand. A second candidate, weaker: a SafeDisc title issues
rem PREVENT_ALLOW_MEDIUM_REMOVAL and the lock outlives the game - both locked
rem boxes were parked on a SafeDisc disc (.124 SYSTEMSHOCK2 1.11.000, .240
rem SHOGO). Neither is proven; the launcher does not depend on which is right.
rem
rem A direct call BLOCKS FOREVER behind that modal, and this is the worst
rem possible failure: the launcher never starts the game, never reaches
rem :anydisc, never writes mount-error.txt, prints no banner - and LEAKS a
rem daemon.exe plus a cmd.exe on every attempt. .124 was found carrying five of
rem each. A silent hang is indistinguishable from "slow", which is exactly the
rem failure this project keeps paying for.
rem
rem `start "" /b` returns immediately, so the modal can no longer wedge the
rem batch; :waitdisc then decides on the POST-CONDITION (did a drive letter
rem carrying our disc actually appear?) rather than on daemon.exe's return, and
rem a locked unit falls through to the loud :anydisc / :fail paths below the way
rem any other mount failure does.
if "%DTKIND%"=="347"  start "" /b "%DT%" -mount 0,"%IMAGE%"
if "%DTKIND%"=="4x"   start "" /b "%DT%" -mount dt, 0, "%IMAGE%"
if "%DTKIND%"=="lite" start "" /b "%DT%" -mount dt, 0, "%IMAGE%"
call :waitdisc 14
rem If the unit was locked, a modal is now up and a daemon.exe is stuck behind
rem it. Clear both so the NEXT launch is not fighting this one's wreckage, and
rem so a second title's launcher is not blocked by a dialog it did not raise.
if not defined DISCDRV taskkill /f /im daemon.exe >nul 2>&1
goto :eof

rem Set DISCDRV to the first drive letter carrying THIS disc.
rem
rem The VOLUME LABEL is tested FIRST and the marker file only as a fallback -
rem the hard way round. Descent II originally shipped MARKER=AUTORUN.INF, which
rem is on essentially every game CD ever pressed, so on .240 it "found" a
rem mounted StarCraft disc, skipped its own mount and started the game against
rem the wrong disc (box-240, 2026-08-29). A marker must be a file that exists
rem ONLY on this disc: do not reach for AUTORUN.INF or INSTALL.EXE, and use a
rem path inside a subdirectory when the disc root has nothing distinctive.
:finddisc
set "DISCDRV="
for %%D in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if not defined DISCDRV (
        vol %%D: 2>nul | find /i "%VOLID%" >nul && set "DISCDRV=%%D"
    )
)
if defined DISCDRV goto :eof
for %%D in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if not defined DISCDRV if exist "%%D:\%MARKER%" set "DISCDRV=%%D"
)
goto :eof

rem Set ANYDRV to any optical drive that currently has a disc in it, ours or
rem not. fsutil is how the drive TYPE is established; it ships with XP Pro and
rem Win7. If it is missing this simply finds nothing, which is the safe way to
rem be wrong - we then report the real failure instead of launching blind.
:anydisc
set "ANYDRV="
for %%D in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if not defined ANYDRV (
        fsutil fsinfo drivetype %%D: 2>nul | find /i "CD-ROM" >nul && (
            vol %%D: >nul 2>&1 && set "ANYDRV=%%D"
        )
    )
)
goto :eof

rem Poll for the disc %1 times, ~2s apart.
:waitdisc
set /a _tries=%1
:waitloop
call :finddisc
if defined DISCDRV goto :eof
set /a _tries=%_tries% - 1
if %_tries% LEQ 0 goto :eof
ping -n 3 127.0.0.1 >nul
goto :waitloop

rem Locate daemon.exe / DTLite.exe without hard-coding a path.
:finddt
set "DT="
call :try "%ProgramFiles%\DAEMON Tools Lite\DTLite.exe"
call :try "%ProgramFiles%\DAEMON Tools Lite\daemon.exe"
call :try "%ProgramFiles%\DAEMON Tools\daemon.exe"
call :try "%ProgramFiles%\D-Tools\daemon.exe"
call :try "%ProgramFiles(x86)%\DAEMON Tools Lite\DTLite.exe"
call :try "%ProgramFiles(x86)%\DAEMON Tools\daemon.exe"
call :try "%ProgramFiles(x86)%\D-Tools\daemon.exe"
call :try "%SystemDrive%\Program Files\D-Tools\daemon.exe"
call :try "%SystemDrive%\Program Files\DAEMON Tools\daemon.exe"
call :try "%SystemDrive%\Program Files\DAEMON Tools Lite\DTLite.exe"
call :try "C:\Program Files\D-Tools\daemon.exe"
call :try "C:\Program Files\DAEMON Tools\daemon.exe"
call :try "C:\Program Files\DAEMON Tools Lite\DTLite.exe"
if defined DT goto :eof
for /f "tokens=2*" %%A in ('reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\daemon.exe" /ve 2^>nul ^| find "REG_"') do call :try "%%B"
if defined DT goto :eof
for /f "tokens=2*" %%A in ('reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\DTLite.exe" /ve 2^>nul ^| find "REG_"') do call :try "%%B"
if defined DT goto :eof
for /f "tokens=2*" %%A in ('reg query "HKLM\SOFTWARE\DT Soft\DAEMON Tools Lite" /v InstallPath 2^>nul ^| find "REG_"') do call :try "%%B\DTLite.exe"
if defined DT goto :eof
for /f "delims=" %%P in ('dir /b /s "%ProgramFiles%\daemon.exe" 2^>nul') do call :try "%%P"
if defined DT goto :eof
for /f "delims=" %%P in ('dir /b /s "%ProgramFiles%\DTLite.exe" 2^>nul') do call :try "%%P"
goto :eof

:try
if defined DT goto :eof
if exist %1 set "DT=%~1"
goto :eof

rem Locate WinCDEmu's batch mounter.
rem
rem *** PICK BY ARCHITECTURE. batchmnt64.exe ON 32-BIT WINDOWS IS FATAL. ***
rem This block used to try batchmnt64.exe FIRST unconditionally. On 32-bit
rem Windows 7 (.246) that binary dies with "not compatible with the version of
rem Windows you're running", exit code 216 - and because the failure was not
rem checked, the script fell through to the "any disc will do" branch and
rem started Brood War against the SHOGO disc. A wrong-architecture binary
rem therefore did not merely fail, it produced a CONFIDENT WRONG RESULT.
rem batchmnt.exe (32-bit) mounted the identical image first try, RC=0.
rem
rem WCD is the preferred binary for this machine and WCD2 is the other one,
rem tried only if the first returns non-zero - so a bad guess costs a retry
rem rather than the whole launch.
:findwcd
set "WCD="
set "WCD2="
set "_W64="
if defined PROCESSOR_ARCHITEW6432 set "_W64=1"
if /i not "%PROCESSOR_ARCHITECTURE%"=="x86" set "_W64=1"
if defined _W64 (
    call :tryw  "%ProgramFiles%\WinCDEmu\batchmnt64.exe"
    call :tryw  "%ProgramFiles(x86)%\WinCDEmu\batchmnt64.exe"
    call :tryw2 "%ProgramFiles%\WinCDEmu\batchmnt.exe"
    call :tryw2 "%ProgramFiles(x86)%\WinCDEmu\batchmnt.exe"
) else (
    call :tryw  "%ProgramFiles%\WinCDEmu\batchmnt.exe"
    call :tryw  "%SystemDrive%\Program Files\WinCDEmu\batchmnt.exe"
    call :tryw  "C:\Program Files\WinCDEmu\batchmnt.exe"
    call :tryw2 "%ProgramFiles%\WinCDEmu\batchmnt64.exe"
)
if defined WCD goto :eof
for /f "tokens=2*" %%A in ('reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\WinCDEmu" /v InstallLocation 2^>nul ^| find "REG_"') do call :tryw "%%B\batchmnt.exe"
if defined WCD goto :eof
for /f "delims=" %%P in ('dir /b /s "%ProgramFiles%\batchmnt.exe" 2^>nul') do call :tryw "%%P"
goto :eof

:tryw2
if defined WCD2 goto :eof
if exist %1 set "WCD2=%~1"
goto :eof

:tryw
if defined WCD goto :eof
if exist %1 set "WCD=%~1"
goto :eof

rem Loud, attributable failure - on screen and in a file an agent can fetch.
:fail
echo.
echo   ============================================================
echo    %GTITLE% could not start.
echo   ============================================================
echo    %~1
echo   ============================================================
echo.
> "%ERRFILE%" echo %GTITLE% mount failure
>>"%ERRFILE%" echo %~1
>>"%ERRFILE%" echo image=%IMAGE%
>>"%ERRFILE%" echo game=%GAME%
>>"%ERRFILE%" echo daemon=%DT%
>>"%ERRFILE%" echo wincdemu=%WCD%
>>"%ERRFILE%" echo dt_flavour=%DTKIND%
rem ~20s so a person at the keyboard can read it, without hanging a headless box.
ping -n 21 127.0.0.1 >nul
goto :eof

:theend
endlocal
rem Close this console - a launcher that leaves its window open for the life of
rem the game stacks up and makes every later screenshot ambiguous (box-145 had
rem sixteen of them).
exit
