#!/usr/bin/env python3
r"""Stage Serious Sam - The First and Second Encounter - as disc-mount titles.

    python3 scripts/fleet/stage-serioussam.py <library>          # apply
    python3 scripts/fleet/stage-serioussam.py <library> --check  # verify only

THE FINDING THIS TITLE EXISTS TO ENCODE
---------------------------------------
Both Encounters were staged once before, hit a modal "Please insert the game
CD", and were WITHDRAWN as disc-locked and unfixable. The withdrawal was right
about the symptom and wrong about the cause, and the difference is the whole
title.

``SeriousSam.exe`` carries **no copy protection at all** - no ``stxt774``, no
``stxt371``, no ``BoG_`` marker, no ``secdrv``. It is not SafeDisc. The check is
forty bytes of ordinary code at ``0x420950`` in the retail TFE binary:

    for ch in 'C'..'Z':
        if GetDriveTypeA("<ch>:\\") == DRIVE_CDROM:        # 5
            _fnmCDPath[0] = ch                             # patch the letter
            f = fopen(_fnmCDPath + "Bin\\SeriousSam.exe", "rb")
            if f: fclose(f); return TRUE
    return FALSE

``_fnmCDPath`` starts out as the literal ``"C:\Install\"``, so the predicate is
exactly: **some drive reporting DRIVE_CDROM holds ``Install\Bin\SeriousSam.exe``.**
The identical loop is in the retail TSE binary (two ``GetDriveTypeA`` xrefs at
``0x423358`` / ``0x427f2e``).

Two consequences, and BOTH were needed to get this title back:

1. **A CD-ROM-typed drive is necessary and NOT sufficient.** Six of the seven
   live boxes already had DRIVE_CDROM volumes - and every one of them held some
   other game's disc (SYSTEMSHOCK2, SHOGO, RF_2, STARCRAFT) or nothing at all.
   That is why the first attempt failed on hardware while the static evidence
   said a mounter would be enough. Measured on .240: a mounted SHOGO disc,
   ``F:\Install\Bin\SeriousSam.exe`` absent, modal raised.

2. **It needs no SafeDisc emulation whatsoever**, which is what separates it
   from Generals and BF1942. DAEMON Tools 3.47 - the fleet's only mounter, and
   provably unable to satisfy SafeDisc 2.80 - satisfies this completely.

DO NOT APPLY THE OFFICIAL TSE 1.07 PATCH
----------------------------------------
``serious-sam-tse-1.07.exe`` on the share replaces the 442,434-byte retail
``SeriousSam.exe`` with a 1,777,634-byte one carrying ``stxt774``, ``stxt371``
and the ``BoG_ *90.0&!!  Yy>`` marker, and ships ``secdrv.sys`` + ``drvmgt.dll``
beside it. **The patch ADDS SafeDisc 2 where retail had none**, and it drops
``GetDriveTypeA`` entirely - so it would convert a title this fleet can run into
one it demonstrably cannot. This is the exact inverse of the Doom 3 case, where
the official 1.3 patch REMOVED the wrapper: "apply the latest official patch" is
not a rule, it is a thing to measure each time.

The TFE 1.05 patch is clean (442,368 bytes, ``GetDriveTypeA`` and "Please insert
the game CD" both still present, no SafeDisc markers) and is safe to apply.
Neither is applied here: both boxes in a LAN pair only have to agree with each
other, and retail-vs-retail agrees.

WHY THE MOUNT MACHINERY IS A SEPARATE ``MOUNTDISC.BAT``
------------------------------------------------------
It is System Shock 2's "fleet template v2" mount logic, unchanged in behaviour -
same probe order, same one-call-per-flavour Daemon Tools rule, same loud
``mount-error.txt``. It lives in one file per title instead of being pasted into
three launchers, for the reason ``FLEETRES.BAT`` gives for itself: six copies of
a 200-line block is six chances to fix a bug five times. It is CALLed, so unlike
the System Shock 2 launcher it must never ``exit`` (that would kill the caller's
console) and it must not ``setlocal`` away the variables the caller needs - its
own variables are all ``MD_``-prefixed instead.

WHY ``Host`` DOES NOT REQUIRE THE DISC
--------------------------------------
``DedicatedServer.exe`` has no ``GetDriveTypeA`` import, no "+cdpath" and no
"Please insert the game CD" string: **the dedicated server has no CD check.**
So the disc requirement is declared PER SHORTCUT and the Host shortcut is
exempt, which is what lets .123 - the one box on the fleet with no optical
drive and no mounter at all - still serve a game to the others. A title-level
``requires_capabilities`` would have suppressed all three shortcuts and left
.123 with no icon, which is precisely how Descent 2 lost both of its.
"""
import argparse
import os
import sys

# --------------------------------------------------------------------------
# Per-title facts. Everything that differs between the two Encounters is here;
# the templates below are shared.
# --------------------------------------------------------------------------
TITLES = {
    'SeriousSamFirstEncounter': dict(
        pretty='Serious Sam - The First Encounter',
        short='Serious Sam TFE',
        iso='SeriousSamTFE.iso',
        # From the ISO's own Primary Volume Descriptor, offset 40 of sector 16.
        volid='SERIOUS_SAM_RC2',
        # A marker must exist ONLY on this disc. Install\Bin\SeriousSam.exe -
        # the file the CD CHECK opens - is on BOTH discs and is therefore
        # exactly the wrong choice here: it would let the Second Encounter's
        # disc satisfy the First's launcher and be reported as a success.
        marker=r'Install\1_00.gro',
        gro='1_00.gro',
        play='Play Serious Sam - The First Encounter.bat',
        host='Host Serious Sam TFE - LAN.bat',
        join='Join Serious Sam TFE - LAN.bat',
        year=2001,
        disk_mb=900,
    ),
    'SeriousSamSecondEncounter': dict(
        pretty='Serious Sam - The Second Encounter',
        short='Serious Sam TSE',
        iso='SeriousSamTSE.iso',
        volid='SamSE',
        marker=r'Install\SE1_00.gro',
        gro='SE1_00.gro',
        play='Play Serious Sam - The Second Encounter.bat',
        host='Host Serious Sam TSE - LAN.bat',
        join='Join Serious Sam TSE - LAN.bat',
        year=2001,
        disk_mb=1050,
        extra_note='''THE TITLE SCREEN SAYS "THE FIRST ENCOUNTER". THE TREE IS CORRECT.
    This release's SE1_00.gro ships The FIRST Encounter's menu-logo textures
    BYTE FOR BYTE - Textures/Logo/sam_menulogo256a.tex and 256b.tex are md5
    a703371889b7... and 1571785bea78... in both games' archives - so the main
    menu reads "SERIOUS SAM - THE FIRST ENCOUNTER - v1.05" while running the
    Second Encounter. It is a Croteam packaging quirk, not a staging error.

    What settles it, and what to check if you ever doubt this again:
      * the campaign. SINGLE PLAYER loads a SNOWY ALPINE VILLAGE
        (Levels/LevelsMP/1_0_InTheLastEpisode) - The First Encounter is
        entirely Egyptian, and its levels are 01_Hatshepsut .. 10_Metropolis.
      * SE1_00_Levels.gro carries Palenque, Teotihuacan, Ziggurrat,
        Persepolis, TowerOfBabylon, GothicCastle, LandOfDamned - all TSE.
      * the tree's only .gro files are SE1_00*.gro, and the running exe is
        this tree's own Bin\\SeriousSam.exe (checked with wmic
        ExecutablePath, because a leftover process from the other Encounter
        would look exactly like this).
    Do NOT "fix" the menu art. It is what the disc ships.'''),
}

ICON = 'SeriousSam.ico'

#: Files the CD's Install\ directory contains that MUST NOT be staged, because
#: they are PER-BOX STATE the engine rewrites on exit.
#:
#: `Scripts\PersistentSymbols.ini` is where Serious Engine saves its persistent
#: cvars when it quits - the resolution, the renderer it detected, and
#: `sam_bFirstStarted`. Staged, it is copied back over every box's own copy on
#: the next GAMESYNC (the engine's write changes size AND mtime, so the resume
#: test always fires), which:
#:   * resets sam_bFirstStarted, so the modal "SeriousSam is starting for the
#:     first time" returns after EVERY sync - on a headless box that is a
#:     dialog with nobody to click it; and
#:   * would carry one machine's detected renderer onto all eight.
#:
#: This is the same rule Doom 3 already carries for DoomConfig.cfg and
#: config.spec, and the engine recreates the file by itself - its own shipped
#: contents are the single line "// initially, this file is empty".
#:
#: GAMESYNC never DELETES, so a box that already took a copy keeps it. That is
#: correct: the copy on the box is the live one.
PER_BOX_STATE = (
    os.path.join('Scripts', 'PersistentSymbols.ini'),
)

# The share path a box falls back to when GAMESYNC could not fit _disc\ on it.
# Z: is the fleet's mapping convention and NOT a guarantee, so the launcher
# reports the fallback rather than relying on it silently.
SHARE_ISO = r'Z:\Files\Games-Library\%s\_disc\%s'


def mountdisc_bat(t):
    return r'''@echo off
rem ==========================================================================
rem  %(pretty)s - mount the game disc.   [System Shock 2 fleet template v2]
rem
rem  CALLed by every launcher in this tree, so:
rem    * NO `exit` - that would close the caller's console with the game in it.
rem    * NO bare `setlocal` - the caller needs DISCDRV afterwards. Everything
rem      private here is MD_-prefixed instead.
rem
rem  WHAT THIS TITLE ACTUALLY WANTS, because it is unusual and cheap:
rem  SeriousSam.exe walks C: to Z:, and for each drive whose GetDriveTypeA is
rem  DRIVE_CDROM it fopen()s "<drive>:\Install\Bin\SeriousSam.exe". That is the
rem  entire check - no SafeDisc, no secdrv, no weak sectors. A CD-ROM-typed
rem  drive is NECESSARY BUT NOT SUFFICIENT: a drive holding some other game's
rem  disc fails it, which is what made this look unfixable the first time.
rem ==========================================================================
set "MD_TITLE=%(pretty)s"
set "MD_VOLID=%(volid)s"
set "MD_MARKER=%(marker)s"
set "MD_ERR=%%~dp0mount-error.txt"
if exist "%%MD_ERR%%" del "%%MD_ERR%%" >nul 2>&1

rem ---- 1. is our disc already in a drive? ---------------------------------
call :md_finddisc
if defined DISCDRV (
    echo [%%MD_TITLE%%] disc already present in %%DISCDRV%%: - not mounting.
    goto :eof
)

rem ---- 2. where is the image? --------------------------------------------
rem  Local _disc\ first: a retro PC must never need the network to start a
rem  game. The share is the FALLBACK, for a box where GAMESYNC could not fit
rem  the image - and it is announced, because a silent network dependency is
rem  how a box that works today stops working when the NAS is off.
set "MD_IMAGE=%%~dp0_disc\%(iso)s"
if not exist "%%MD_IMAGE%%" (
    set "MD_IMAGE=%(share_iso)s"
    echo [%%MD_TITLE%%] no local _disc image - falling back to the SHARE.
)
if not exist "%%MD_IMAGE%%" (
    call :md_fail "The disc image is missing BOTH locally and on the share: %%~dp0_disc\%(iso)s and %(share_iso)s . Re-run GAMESYNC for this title, and check the box has Z: mapped."
    goto :eof
)

rem ---- 3. find a mounter --------------------------------------------------
call :md_finddt
call :md_findwcd
if not defined DT if not defined WCD (
    call :md_fail "NO DISC MOUNTER IS INSTALLED on this machine. Looked for Daemon Tools (Program Files\D-Tools, DAEMON Tools, DAEMON Tools Lite; App Paths daemon.exe and DTLite.exe) and WinCDEmu (batchmnt.exe / batchmnt64.exe), then searched Program Files. This is an INSTALL problem, not a mount problem - and it is why this title declares the disc_mount capability."
    goto :eof
)
if defined DT  echo [%%MD_TITLE%%] Daemon Tools: %%DT%%
if defined WCD echo [%%MD_TITLE%%] WinCDEmu:     %%WCD%%

rem AutoPlay throws a modal over the running game, and taskkilling the disc's
rem autorun afterwards does not stop the shell's own dialog. 255 = every drive
rem type. HKCU only - no administrator needed, nobody else's session touched.
reg add "HKCU\Software\Microsoft\Windows\CurrentVersion\Policies\Explorer" /v NoDriveTypeAutoRun /t REG_DWORD /d 255 /f >nul 2>&1

if defined DT call :md_domount
if defined DISCDRV goto :md_mounted
if defined WCD (
    "%%WCD%%" /mount "%%MD_IMAGE%%" >nul 2>&1
    if errorlevel 1 if defined WCD2 "%%WCD2%%" /mount "%%MD_IMAGE%%" >nul 2>&1
    call :md_waitdisc 14
)
if defined DISCDRV goto :md_mounted

rem ---- 4. it did not mount. SAY SO. --------------------------------------
call :md_anydisc
if defined ANYDRV (
    echo.
    echo   ============================================================
    echo    %%MD_TITLE%%: THE MOUNT FAILED.
    echo   ============================================================
    echo    "%%MD_IMAGE%%" did NOT mount. %%ANYDRV%%: has SOME disc in it, which
    echo    may be a COMPLETELY DIFFERENT game's disc. This game checks for
    echo    Install\Bin\SeriousSam.exe on a CD-ROM drive, so an unrelated disc
    echo    will NOT satisfy it and the game will ask for its CD.
    echo    This is a FAILURE that is being tolerated, not a success.
    echo   ============================================================
    echo.
    > "%%MD_ERR%%" echo %%MD_TITLE%%: MOUNT FAILED - an unrelated disc is in %%ANYDRV%%:
    >>"%%MD_ERR%%" echo image=%%MD_IMAGE%%
    >>"%%MD_ERR%%" echo daemon=%%DT%%
    >>"%%MD_ERR%%" echo wincdemu=%%WCD%%  fallback=%%WCD2%%
    goto :eof
)
call :md_fail "A MOUNTER WAS FOUND BUT NO DRIVE APPEARED within about 30 seconds, and no other disc is in any optical drive either. This is a mount problem, not a missing-software problem. Mount the image by hand (Daemon Tools tray icon - Device 0 - Mount image, or double-click the image with WinCDEmu) and run the game."
goto :eof

:md_mounted
rem The disc's own autorun lands on top of the game a moment after the drive
rem appears. Killing it before the shell has started it is a no-op, so wait.
ping -n 4 127.0.0.1 >nul
for %%%%K in (setup.exe autorun.exe) do taskkill /f /im %%%%K >nul 2>&1
echo [%%MD_TITLE%%] disc is on %%DISCDRV%%:
goto :eof

rem ======================== subroutines ====================================

rem ONE mount call, in the spelling this build understands. Daemon Tools 3.x
rem answers an UNSUPPORTED switch with a MODAL DIALOG that >nul cannot suppress
rem and that then blocks every later daemon.exe call - System Shock 2 died on
rem exactly that (box-240, 2026-08-29). Never spray switches at it.
:md_domount
set "DTKIND="
echo %%DT%% | find /i "\D-Tools\" >nul && set "DTKIND=347"
if not defined DTKIND echo %%DT%% | find /i "DTLite.exe" >nul && set "DTKIND=lite"
if not defined DTKIND echo %%DT%% | find /i "\DAEMON Tools\" >nul && set "DTKIND=4x"
if not defined DTKIND set "DTKIND=347"
echo [%%MD_TITLE%%] Daemon Tools flavour: %%DTKIND%%
if "%%DTKIND%%"=="347"  "%%DT%%" -mount 0,"%%MD_IMAGE%%"
if "%%DTKIND%%"=="4x"   "%%DT%%" -mount dt, 0, "%%MD_IMAGE%%"
if "%%DTKIND%%"=="lite" "%%DT%%" -mount dt, 0, "%%MD_IMAGE%%"
call :md_waitdisc 14
goto :eof

rem Volume label first, marker file second. The marker is a .gro that exists
rem ONLY on this disc - NOT Install\Bin\SeriousSam.exe, which is on both
rem Encounters' discs and would let the wrong one report success. Descent II
rem shipped MARKER=AUTORUN.INF once and "found" a mounted StarCraft disc.
:md_finddisc
set "DISCDRV="
for %%%%D in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if not defined DISCDRV (
        vol %%%%D: 2>nul | find /i "%%MD_VOLID%%" >nul && set "DISCDRV=%%%%D"
    )
)
if defined DISCDRV goto :eof
for %%%%D in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if not defined DISCDRV if exist "%%%%D:\%%MD_MARKER%%" set "DISCDRV=%%%%D"
)
goto :eof

rem Any optical drive with a disc in it, ours or not. fsutil establishes the
rem TYPE; if it is missing this finds nothing, which is the safe way to be
rem wrong - we then report the real failure instead of launching blind.
:md_anydisc
set "ANYDRV="
for %%%%D in (D E F G H I J K L M N O P Q R S T U V W X Y Z) do (
    if not defined ANYDRV (
        fsutil fsinfo drivetype %%%%D: 2>nul | find /i "CD-ROM" >nul && (
            vol %%%%D: >nul 2>&1 && set "ANYDRV=%%%%D"
        )
    )
)
goto :eof

rem A mount is asynchronous. Starting the game before the drive letter appears
rem is the classic spurious "please insert the CD".
:md_waitdisc
set /a MD_TRIES=%%1
:md_waitloop
call :md_finddisc
if defined DISCDRV goto :eof
set /a MD_TRIES=%%MD_TRIES%% - 1
if %%MD_TRIES%% LEQ 0 goto :eof
ping -n 3 127.0.0.1 >nul
goto :md_waitloop

:md_finddt
set "DT="
call :md_try "%%ProgramFiles%%\DAEMON Tools Lite\DTLite.exe"
call :md_try "%%ProgramFiles%%\DAEMON Tools Lite\daemon.exe"
call :md_try "%%ProgramFiles%%\DAEMON Tools\daemon.exe"
call :md_try "%%ProgramFiles%%\D-Tools\daemon.exe"
call :md_try "%%ProgramFiles(x86)%%\DAEMON Tools Lite\DTLite.exe"
call :md_try "%%ProgramFiles(x86)%%\DAEMON Tools\daemon.exe"
call :md_try "%%ProgramFiles(x86)%%\D-Tools\daemon.exe"
call :md_try "%%SystemDrive%%\Program Files\D-Tools\daemon.exe"
call :md_try "C:\Program Files\D-Tools\daemon.exe"
call :md_try "C:\Program Files\DAEMON Tools\daemon.exe"
call :md_try "C:\Program Files\DAEMON Tools Lite\DTLite.exe"
if defined DT goto :eof
for /f "tokens=2*" %%%%A in ('reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\daemon.exe" /ve 2^>nul ^| find "REG_"') do call :md_try "%%%%B"
if defined DT goto :eof
for /f "tokens=2*" %%%%A in ('reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\DTLite.exe" /ve 2^>nul ^| find "REG_"') do call :md_try "%%%%B"
if defined DT goto :eof
for /f "delims=" %%%%P in ('dir /b /s "%%ProgramFiles%%\daemon.exe" 2^>nul') do call :md_try "%%%%P"
goto :eof

:md_try
if defined DT goto :eof
if exist %%1 set "DT=%%~1"
goto :eof

rem PICK BY ARCHITECTURE. batchmnt64.exe on 32-bit Windows exits 216 with
rem "not compatible with the version of Windows you're running" - and because
rem that was once unchecked, the script fell through to "any disc will do" and
rem started a game against the wrong disc. A wrong-architecture binary does not
rem merely fail, it produces a CONFIDENT WRONG RESULT.
:md_findwcd
set "WCD="
set "WCD2="
set "MD_W64="
if defined PROCESSOR_ARCHITEW6432 set "MD_W64=1"
if /i not "%%PROCESSOR_ARCHITECTURE%%"=="x86" set "MD_W64=1"
if defined MD_W64 (
    call :md_tryw  "%%ProgramFiles%%\WinCDEmu\batchmnt64.exe"
    call :md_tryw  "%%ProgramFiles(x86)%%\WinCDEmu\batchmnt64.exe"
    call :md_tryw2 "%%ProgramFiles%%\WinCDEmu\batchmnt.exe"
    call :md_tryw2 "%%ProgramFiles(x86)%%\WinCDEmu\batchmnt.exe"
) else (
    call :md_tryw  "%%ProgramFiles%%\WinCDEmu\batchmnt.exe"
    call :md_tryw  "C:\Program Files\WinCDEmu\batchmnt.exe"
    call :md_tryw2 "%%ProgramFiles%%\WinCDEmu\batchmnt64.exe"
)
if defined WCD goto :eof
for /f "tokens=2*" %%%%A in ('reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\WinCDEmu" /v InstallLocation 2^>nul ^| find "REG_"') do call :md_tryw "%%%%B\batchmnt.exe"
goto :eof

:md_tryw2
if defined WCD2 goto :eof
if exist %%1 set "WCD2=%%~1"
goto :eof

:md_tryw
if defined WCD goto :eof
if exist %%1 set "WCD=%%~1"
goto :eof

rem Loud, attributable failure - on screen AND in a file an agent can DOWNLOAD.
:md_fail
echo.
echo   ============================================================
echo    %%MD_TITLE%% could not mount its disc.
echo   ============================================================
echo    %%~1
echo   ============================================================
echo.
> "%%MD_ERR%%" echo %%MD_TITLE%% mount failure
>>"%%MD_ERR%%" echo %%~1
>>"%%MD_ERR%%" echo image=%%MD_IMAGE%%
>>"%%MD_ERR%%" echo daemon=%%DT%%
>>"%%MD_ERR%%" echo wincdemu=%%WCD%%
>>"%%MD_ERR%%" echo dt_flavour=%%DTKIND%%
ping -n 11 127.0.0.1 >nul
goto :eof
''' % dict(t, share_iso=SHARE_ISO % (t['_dir'], t['iso']))


PLAY = r'''@echo off
rem %(pretty)s - fleet launcher (single player / co-op client).
rem
rem The exe lives in Bin\ but the engine resolves everything else relative to
rem the CURRENT DIRECTORY, so this .bat cd's to the tree root first. A shortcut
rem straight to Bin\SeriousSam.exe with the wrong "start in" finds no .gro
rem files and dies on a black screen.
rem
rem The disc mount has to happen BEFORE the game starts: SeriousSam.exe runs
rem its CD check in WinMain, before it opens a window, and answers a failure
rem with a modal that nothing on a headless box will ever dismiss.
call "%%~dp0MOUNTDISC.BAT"

cd /d "%%~dp0"

start "" "Bin\SeriousSam.exe"

rem Close this console. A .bat that ends at "start" leaves its window open for
rem the life of the game; a dozen launches leaves a dozen of them stacked on
rem the desktop, and a screenshot then cannot tell "fullscreen game GDI cannot
rem capture" from "the game never started".
exit
'''

HOST = r'''@echo off
rem %(pretty)s - HOST a LAN game.   [fleet LAN pattern]
rem
rem Serious Engine 1 cannot be told to open a listen server from the command
rem line - SeriousSam.exe accepts only +level/+game/+cdpath/+password/+connect/
rem +script/+goto (read out of the binary, not guessed). So the host runs the
rem DEDICATED SERVER that ships with the game and then joins its own server on
rem the loopback, which puts a player at this end too - a two-box proof needs a
rem game on BOTH screens, not a console window on one of them.
rem
rem "DefaultCoop" is Croteam's own config, staged unmodified at
rem Scripts\Dedicated\DefaultCoop\init.ini: co-op, 8 players, infinite credits,
rem UDP 25600, waits for the first player.
rem
rem *** THE SERVER HALF NEEDS NO DISC. *** DedicatedServer.exe has no
rem GetDriveTypeA import, no +cdpath and no "Please insert the game CD" string -
rem the CD check is in SeriousSam.exe only. That is why this shortcut carries no
rem disc_mount requirement and still appears on .123, the one box with no
rem optical drive and no mounter: it can serve a game it cannot itself play.
rem The mount below is therefore best-effort, and the local client is started
rem only if it actually produced a disc.
echo.
echo  Other players need THIS machine's address:
ipconfig | findstr /i "IPv4 Address"
echo.
echo  On every other machine: put that address on the first line of
echo  C:\Games\lanhost.txt and run "Join %(short)s - LAN.bat".
echo.
call "%%~dp0MOUNTDISC.BAT"

cd /d "%%~dp0"

start "%(pretty)s dedicated server" "Bin\DedicatedServer.exe" DefaultCoop
rem Give the server time to load the level and open UDP 25600 before the local
rem client tries to connect. Serious Sam's client does NOT retry - it reports
rem "server not responding" and drops to the menu, which reads exactly like a
rem server that never started.
ping -n 16 127.0.0.1 >nul

if not defined DISCDRV (
    echo.
    echo  ============================================================
    echo   The dedicated server is UP and other machines can join it.
    echo   This machine has no %(pretty)s disc mounted, so the game
    echo   itself cannot start here - see mount-error.txt. That is
    echo   expected on a box with no disc mounter.
    echo  ============================================================
    ping -n 11 127.0.0.1 >nul
    exit
)
start "" "Bin\SeriousSam.exe" +connect 127.0.0.1

exit
'''

JOIN = r'''@echo off
rem %(pretty)s - JOIN a LAN game.   [fleet LAN pattern]
rem
rem WHERE THE HOST ADDRESS COMES FROM - first one that exists wins:
rem   1. the first argument to this .bat
rem   2. lanhost.txt beside this file        (per-title override)
rem   3. C:\Games\lanhost.txt                (fleet-wide; one line, the host IP)
rem   4. only then, ask at the console
rem
rem The file forms are not a convenience, they are the point. A fleet box is
rem normally driven by an agent with nobody at the keyboard, and a bare "set /p"
rem prompt HANGS THE LAUNCHER FOREVER - which looks exactly like a game that
rem failed to start and gets diagnosed as a broken staged tree.
rem
rem The game ALSO has "search local network for servers" in its own menu; this
rem shortcut exists because a broadcast that does not arrive is
rem indistinguishable from a server that never started.
set "HOSTIP=%%~1"
if not defined HOSTIP if exist "%%~dp0lanhost.txt" for /f "usebackq eol=; tokens=1 delims= " %%%%A in ("%%~dp0lanhost.txt") do if not defined HOSTIP set "HOSTIP=%%%%A"
if not defined HOSTIP if exist "C:\Games\lanhost.txt" for /f "usebackq eol=; tokens=1 delims= " %%%%A in ("C:\Games\lanhost.txt") do if not defined HOSTIP set "HOSTIP=%%%%A"
if not defined HOSTIP set /p "HOSTIP=IP address of the machine hosting the game: "
if not defined HOSTIP (
    echo.
    echo  No host address given - nothing to join.
    echo  Put the host machine's IP on the first line of C:\Games\lanhost.txt
    echo  and run this shortcut again.
    ping -n 6 127.0.0.1 >nul
    exit
)
echo  Connecting to %%HOSTIP%% ...
call "%%~dp0MOUNTDISC.BAT"

cd /d "%%~dp0"

start "" "Bin\SeriousSam.exe" +connect %%HOSTIP%%
exit
'''

NOTES = r'''%(pretty)s - staged tree notes
%(rule)s

WHAT THIS TITLE NEEDS THAT MOST DO NOT
    A mounted copy of its own disc image. _disc\%(iso)s is that image, and
    MOUNTDISC.BAT mounts it at every launch. The image is the retail disc,
    converted from the share's MODE2/2352 .bin at sector offset 24 (not 16 -
    a Mode 2 Form 1 sector carries an 8-byte subheader before the payload, and
    reading it at 16 produces a full-size ISO with no filesystem at all).

WHY - the exact predicate, read out of the binary
    SeriousSam.exe walks drive letters C to Z. For each drive whose
    GetDriveTypeA is DRIVE_CDROM it opens "<drive>:\Install\Bin\SeriousSam.exe"
    and passes if that succeeds. There is NO copy protection - no stxt774,
    no stxt371, no BoG_ marker, no secdrv. It is not SafeDisc, and it needs no
    SafeDisc emulation, which is why DAEMON Tools 3.47 satisfies it completely
    where it cannot satisfy Generals or BF1942.

    A CD-ROM-typed drive is NECESSARY BUT NOT SUFFICIENT. An empty drive, or
    one holding another game's disc, fails the check. That is why this title
    was once withdrawn as "disc-locked and unfixable": the boxes it was tested
    on did have optical drives, holding other games' discs.

DO NOT APPLY THE OFFICIAL TSE 1.07 PATCH
    It replaces the retail SeriousSam.exe with a SafeDisc 2 wrapped one and
    ships secdrv.sys beside it - it ADDS the protection retail never had, and
    would make the title unrunnable on this fleet. The TFE 1.05 patch is clean.
    Neither is applied: a LAN pair only has to agree with itself.

%(extra)s
WHAT IS NOT STAGED, and why
    Bin\SeriousEditor.exe, Bin\SeriousModeler.exe, Bin\eview3d.dll and the
    whole Bin\3DExplorationPlugins\ directory. Four of those plug-ins carry an
    impossible PE TimeDateStamp (DWG.X3D and lwo.x3d decode to 2069, archive.x3d
    to 1971) and none of them is needed to play. Every remaining binary in this
    tree is PE32 subsystem 4.0 - nothing Vista-only, which XP's loader would
    refuse before a single instruction ran.

THE ICON
    Serious Sam ships NO icon anywhere: SeriousSam.exe, DedicatedServer.exe,
    the disc's Setup.exe, both official patch installers and both demo
    installers all have an entirely empty PE resource directory, and there is
    no .ico on either disc. %(icon)s is therefore generated from the game's own
    main-menu logo texture (Textures/Logo/sam_menulogo256b.tex inside %(gro)s)
    by scripts/fleet/make-ssam-icon.py in the retro-agent repo.

THE RENDERER IS THE ENGINE'S CHOICE, NOT THE LAUNCHER'S
    The launcher writes the RESOLUTION and deliberately does NOT write
    sam_iDriver. Serious Engine 1 has an OpenGL path (0) and a Direct3D path
    (1), it auto-detects on first run, and it saves the answer in
    Scripts\PersistentSymbols.ini.

    That matters because the OpenGL path is not universal here. Measured on
    .246 (Windows 7, Radeon HD 5450) 2026-08-31: with sam_iDriver=0 the game
    dies before opening a window -

        Fatal Error: Cannot set display mode!
        Serious Sam was unable to find display mode with OpenGL acceleration.

    - and the identical tree with sam_iDriver=1 starts and renders. An earlier
    version of the launcher pinned sam_iDriver=0 at EVERY start, which also
    overwrote the engine's own persisted answer, so a box fixed by hand was
    un-fixed on its next launch.

    To pin a renderer on one box, put it in THAT BOX'S
    Scripts\PersistentSymbols.ini - that file is per-box state and is
    deliberately not staged, so it survives GAMESYNC.

RESOLUTION
    Scripts\Game_startup.ini, which the engine documents as "executed each time
    SeriousSam is started", is rewritten by each launcher from FLEETRES.
    Scripts\PersistentSymbols.ini must NOT be used for this: the engine
    rewrites it on exit, so a resolution staged there is overwritten by the
    first box that runs the game and was wrong on the other seven before that.

NO install.reg
    The game needs no registry key to run. The CD's Install\ directory IS the
    installed tree - a pure copy install, no CD key, no serial, no wizard.
'''


def launch_txt(t):
    """Data lines FIRST - the agent reads only the first 1023 bytes."""
    rows = [
        (t['play'], t['pretty'], ICON),
        (t['host'], '%s - Host LAN Game' % t['pretty'], ICON),
        (t['join'], '%s - Join LAN Game' % t['pretty'], ICON),
    ]
    out = ['\t'.join(r) for r in rows]
    out += [
        '#',
        '# DATA LINES FIRST - the agent reads only the first 1023 bytes of this',
        '# file, so a comment block above the data silently costs a shortcut.',
        '#',
        '# The third column is the EXPLICIT icon and it has to be. All three',
        '# shortcuts reach the same two binaries, so auto-resolution could not',
        '# tell them apart - and it would find no icon at all either way:',
        '# SeriousSam.exe and DedicatedServer.exe both have an EMPTY PE',
        '# resource directory. SeriousSam.ico is generated from the game\'s own',
        '# menu-logo texture by scripts/fleet/make-ssam-icon.py.',
        '',
    ]
    return '\r\n'.join(out)


def requires_json(t):
    return '''{
  "requirements_version": 2,
  "title": "%(dir)s",
  "year": %(year)d,
  "min_cpu_mhz": 233,
  "min_ram_mb": 64,
  "min_vram_mb": 8,
  "gpu_feature_level": "fixed",
  "disk_mb": %(disk_mb)d,
  "notes": "Croteam Serious Engine 1. Published minimum is a Pentium 233 MMX with 64 MB and an 8 MB 3D card; the engine has an OpenGL and a Direct3D renderer and NO software rasteriser, so a machine with no 3D pipeline cannot run it - gpu_feature_level 'fixed', not 'none'. THE DISC REQUIREMENT IS PER SHORTCUT, NOT PER TITLE. SeriousSam.exe checks for Install\\\\Bin\\\\SeriousSam.exe on a DRIVE_CDROM volume (plain GetDriveTypeA + fopen - no SafeDisc, no secdrv, so DAEMON Tools 3.47 satisfies it), but DedicatedServer.exe has no such check at all. Declaring disc_mount at the TITLE level would suppress all three shortcuts and leave .123 - no optical drive, no mounter - with no icon, which is exactly how Descent 2 lost both of its. disk_mb covers the ~%(tree_mb)d MB tree plus the ~%(iso_mb)d MB disc image in _disc\\\\.",
  "shortcuts": {
    "%(play)s": {
      "requires_capabilities": ["disc_mount"]
    },
    "%(join)s": {
      "requires_capabilities": ["disc_mount"]
    },
    "%(host)s": {
      "min_ram_mb": 96,
      "notes": "No disc_mount: the dedicated server has no CD check, so a box with no mounter can still host for the fleet."
    }
  }
}
'''


#: Files this script owns byte-for-byte. The three launchers are NOT among
#: them: stage-fleetres.py PATCHES a launcher after this script creates it
#: (inserting the FLEETRES call and the Game_startup.ini rewrite), so an
#: exact-content check on a launcher would report "stale" for a tree that is
#: in fact fully and correctly staged - and re-writing it would silently
#: DELETE the resolution block. Launchers are create-only, and --check asserts
#: the COMPOSITION instead: both stagers' marks present.
LAUNCHER_MARKS = ('call "%~dp0MOUNTDISC.BAT"', 'call "%~dp0FLEETRES.BAT"')


def files_for(name, t):
    t = dict(t, _dir=name)
    tree_mb = {'SeriousSamFirstEncounter': 369, 'SeriousSamSecondEncounter': 485}[name]
    iso_mb = {'SeriousSamFirstEncounter': 479, 'SeriousSamSecondEncounter': 512}[name]
    extra = t.get('extra_note')
    sub = dict(t, dir=name, icon=ICON, tree_mb=tree_mb, iso_mb=iso_mb,
               rule='=' * 60,
               extra=('\n' + extra + '\n') if extra else '')
    return {
        'MOUNTDISC.BAT': mountdisc_bat(t),
        t['play']: PLAY % sub,
        t['host']: HOST % sub,
        t['join']: JOIN % sub,
        'launch.txt': launch_txt(t),
        'requires.json': requires_json(t) % sub,
        'NOTES.txt': NOTES % sub,
    }


def crlf(text):
    return text.replace('\r\n', '\n').replace('\n', '\r\n')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('library')
    ap.add_argument('--check', action='store_true')
    args = ap.parse_args()

    rc = 0
    for name, t in sorted(TITLES.items()):
        tree = os.path.join(args.library, name)
        if not os.path.isdir(tree):
            print('%-28s MISSING TREE' % name)
            rc = 1
            continue
        for rel in PER_BOX_STATE:
            sp = os.path.join(tree, rel)
            if os.path.exists(sp):
                if args.check:
                    print('%-28s %-46s MUST NOT BE STAGED' % (name, rel))
                    rc = 1
                else:
                    os.remove(sp)
                    print('%-28s %-46s removed (per-box state)' % (name, rel))
            else:
                print('%-28s %-46s absent (correct)' % (name, rel))

        launchers = {t['play'], t['host'], t['join']}
        for fn, body in sorted(files_for(name, t).items()):
            p = os.path.join(tree, fn)
            want = crlf(body).encode('latin1')
            have = open(p, 'rb').read() if os.path.exists(p) else None

            if fn in launchers:
                # Create-only; see LAUNCHER_MARKS.
                if have is None:
                    if args.check:
                        print('%-28s %-46s ABSENT' % (name, fn))
                        rc = 1
                        continue
                    with open(p, 'wb') as fh:
                        fh.write(want)
                    print('%-28s %-46s written (%d bytes)' % (name, fn, len(want)))
                    continue
                txt = have.decode('latin1')
                missing = [m for m in LAUNCHER_MARKS if m not in txt]
                if missing:
                    print('%-28s %-46s MISSING %s'
                          % (name, fn, ' and '.join(repr(m) for m in missing)))
                    rc = 1
                else:
                    print('%-28s %-46s ok (mounts + fleetres)' % (name, fn))
                continue

            if have == want:
                print('%-28s %-46s ok' % (name, fn))
                continue
            if args.check:
                print('%-28s %-46s STALE/ABSENT' % (name, fn))
                rc = 1
                continue
            with open(p, 'wb') as fh:
                fh.write(want)
            print('%-28s %-46s written (%d bytes)' % (name, fn, len(want)))
    raise SystemExit(rc)


if __name__ == '__main__':
    main()
