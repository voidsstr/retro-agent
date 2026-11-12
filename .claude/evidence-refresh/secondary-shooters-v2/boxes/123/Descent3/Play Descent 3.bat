@echo off
rem ==========================================================================
rem  Descent 3 - fleet launcher.  NO DISC, NO MOUNTER, NO REGISTRATION NAG.
rem
rem  THIS USED TO BE THE BIG RESILIENT MOUNT LAUNCHER. It is not any more, and
rem  the reason matters: applying the official 1.4 file set from
rem  http://descent3.com/patch/ replaced main.exe with the build that has no
rem  disc check in it. That is not an assumption - the OLD main.exe contains the
rem  literal string "Invalid CDROM!" and the NEW one does not. The previous
rem  launcher's whole Daemon Tools / WinCDEmu dance therefore has nothing left
rem  to do, and on .123 it was the ACTIVE BLOCKER: the title failed with
rem  "NO DISC MOUNTER IS INSTALLED" on a machine that had no mounter, for a
rem  game that no longer needs one.
rem  The old launcher is kept at
rem    _patches\backup-2026-08-29-descent3-1.4\Play Descent 3.bat.mountlauncher
rem
rem  _disc\Descent3_CD1.iso (666 MB) was removed from the staged tree for the
rem  same reason. Everything the game reads is already installed locally -
rem  d3.hog, d3voice1.hog, d3voice2.hog, extra.hog, extra13.hog, all six movies
rem  in movies\, missions\ and Mercenary\. The only things on that disc that are
rem  NOT in the tree are the DirectX 6 redistributable and the autorun, neither
rem  of which any fleet box needs. Three other copies of the image remain on the
rem  share (Games\Windows XP\Descent_3_CD1.iso and two more), so nothing was
rem  lost. NOTE: GAMESYNC never deletes, so a box that already synced the old
rem  tree still has its own _disc copy - that is 666 MB a box agent can reclaim
rem  by hand.
rem
rem  WHY main.exe AND NOT "Descent 3.exe": "Descent 3.exe" is the launcher stub,
rem  and it is the thing that runs ereg\reg32a.exe - the Interplay Electronic
rem  Registration modal that box-123 found blocking every start and that does
rem  NOT remember being dismissed. Its strings are still in that stub
rem  ("reg32a.exe", "\ereg"). Going straight at main.exe skips it entirely.
rem  -launched is the flag the stub would otherwise pass; main.exe expects it.
rem ==========================================================================
call "%~dp0FLEETRES.BAT"

cd /d "%~dp0"
if not exist "%~dp0main.exe" (
    echo.
    echo  Descent 3 is not installed correctly: main.exe is missing.
    echo  Re-run GAMESYNC for this title.
    ping -n 6 127.0.0.1 >nul
    exit
)
start "" "%~dp0main.exe" -launched -Width %FR_W% -Height %FR_H%

rem Close this console. A .bat that ends without `exit` leaves its window open
rem for the life of the game, and a desktop with a dozen of them stacked on it
rem actively corrupts screenshot-based verification.
exit
