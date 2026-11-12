@echo off
rem ==========================================================================
rem  Shogo - direct engine launch. NO SPLASH, NO PER-LAUNCH CLICK.
rem
rem  VERIFIED ON TWO BOXES. On .145: window class LithTech at 640x480, Progman
rem  drops to 640x480 (real fullscreen), no splash, no shell-DLL dialog, and the
rem  "SHOGO - MOBILE ARMOR DIVISION" title screen renders. Re-verified on .123
rem  from a CLEAN staged tree: the LithTech main menu at 640x480 - single player,
rem  multiplayer, options, credits, quit - with no dialog and no splash. The
rem  per-launch click is genuinely retired.
rem
rem  *** LITHTECH -rez IS LAST-WINS. *** An earlier version of this launcher had
rem  the chain REVERSED, on my inference that Shogo.exe listing the patches
rem  highest-first in its string table meant first-match-wins. THAT INFERENCE
rem  WAS WRONG, and it failed in a way that pointed straight at itself: with
rem  shogo.rez listed LAST it overrode every patch, so the engine resolved the
rem  RETAIL v1 CShell.dll and raised
rem      "Invalid shell DLL version for cshell.dll. DLL version is 1,
rem       current version is 3."
rem  which is exactly the error the order was supposed to fix. The base archives
rem  must come FIRST and the newest patch LAST, so the later ones win.
rem
rem  WHERE THE v3 CShell LIVES: SHOGOP3.REZ, dated 1999-03-09 - the 2.2 patch's
rem  own date. SHOGO.REZ carries the retail v1 and ShogoP.rez the 1.x copy;
rem  SHOGOP2.REZ, SHOGOP4.REZ and SOUND.REZ carry none. Nothing was ever missing
rem  from the tree. (Search for it case-INSENSITIVELY - LithTech names the file
rem  "CShell.dll", so grepping for "cshell" finds nothing and misleads.)
rem ==========================================================================
call "%~dp0FLEETRES.BAT"
if exist "%~dp0FLEETRES.EXE" (
  "%~dp0FLEETRES.EXE" -setline "%~dp0autoexec.cfg" screenwidth `screenwidth` `%FR_W%`
  "%~dp0FLEETRES.EXE" -setline "%~dp0autoexec.cfg" screenheight `screenheight` `%FR_H%`
)

cd /d "%~dp0"
start "" "%~dp0Client.exe" -rez shogo.rez -rez sound.rez -rez ShogoP.rez -rez ShogoP2.rez -rez ShogoP3.rez -rez ShogoP4.rez
exit
