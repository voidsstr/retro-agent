@echo off
rem Fleet launcher. -full = the fullscreen mandate; -nosierra suppresses the
rem dead SierraUp updater. Add -window to this line (or run hl.exe -window
rem by hand) if you need a screenshot through the agent's GDI capture -- an
rem exclusive-fullscreen GoldSrc window captures as a black frame.
rem
rem -toconsole is what gets past the WON LAUNCHER DIALOG, and it is the
rem MINIMAL switch that does: box-240 A/B-ed the whole matrix on .240 and
rem -console alone only adds a "Console" entry to the launcher menu, while
rem -toconsole alone starts the engine (window class Half-Life +
rem DirectDrawDeviceWnd at 0,0-1024,768 instead of the #32770 dialog).
rem -game valve was tried and refuted; HalfLife1\hl.exe is the 942,080-byte
rem WON launcher while CounterStrike16\hl.exe is an 86,077-byte engine
rem loader, so Counter-Strike proves nothing about this build.
rem
rem HONEST CAVEAT: this lands on the engine CONSOLE screen, not the main
rem menu - one press of ~ or ESC gets to the menu. No switch in the exe
rem reaches the menu directly (every -switch string was dumped and checked).
rem The alternatives are worse: +map c0a0 hard-codes a front door, and
rem leaving the launcher up fails the fullscreen requirement outright.
call "%~dp0FLEETRES.BAT"

cd /d "%~dp0"
rem
rem RENDERER AND RESOLUTION, added 2026-08-29 after .246 reported the engine
rem banner as "(sw build 1786)" - the SOFTWARE renderer - with -full landing at
rem 640x480 on a 1920x1080 panel.
rem   -gl              force the OpenGL renderer. WON Half-Life defaults to
rem                    software when nothing selects otherwise, and "-full" only
rem                    means fullscreen, not accelerated.
rem   -w 1024 -h 768   the mode. install.reg also seeds ScreenWidth/Height and
rem                    EngineModeW/H in HKCU because the WON engine keeps video
rem                    settings in the REGISTRY, not in any file in this tree -
rem                    the switches and the registry must agree, so if you change
rem                    one, change the other.
rem
rem -toconsole IS DELIBERATE AND IS KEPT. It lands on the engine console with a
rem bare ] prompt rather than the menu, which costs one ESC - and that is
rem annoying but it is the LEAST BAD option: a full switch matrix was run on
rem .240 and -toconsole is the only thing that gets past the WON launcher dialog
rem at all. -console alone merely adds a menu entry. Nothing reaches the menu
rem directly. Do not "fix" this by removing it without re-running that matrix.
rem
rem WHY FR_W43/FR_H43 AND NOT FR_W/FR_H  (fixed 2026-08-30)
rem   THE WON ENGINE HAS A FIXED 4:3 MODE TABLE AND NO WIDESCREEN MODE AT
rem   ALL. Handed a 16:9 mode it does not fall back to something sensible -
rem   it falls back to the BOTTOM of its table, 400x300, and takes the whole
rem   desktop down with it.
rem
rem   MEASURED on .240 (1920x1080 panel), same launcher, one token changed,
rem   DISPLAYCFG read after +map crossfire:
rem     -w 1920 -h 1080  ->  desktop 400x300,  window 400x300   BROKEN
rem     -w 1280 -h 960   ->  desktop 1280x960, window 1280x960  CORRECT
rem   1280x1024 is also accepted but the engine SILENTLY remaps it to
rem   1280x960, which is the same 4:3 answer by a longer route.
rem
rem   So this line must use the 4:3 pair. FLEETRES.BAT publishes FR_W43 /
rem   FR_H43 for exactly this case - "resolution for an engine that is
rem   4:3-only" - and a 4:3 CRT box gets the same answer from both pairs, so
rem   this costs those boxes nothing.
rem
rem   This was INVISIBLE on the CRT boxes and broken on every widescreen one,
rem   which is why it survived: the game still ran, still said fullscreen,
rem   and nothing reported an error.
start "" hl.exe -nosierra -full -gl -w %FR_W43% -h %FR_H43% -toconsole

rem Close this console. A .bat that ends at "start" leaves its window open for
rem the life of the game, and after a dozen launches box-145 had SIXTEEN of
rem them stacked on the desktop - which actively corrupted testing, because a
rem screenshot then showed a cluttered desktop and you could not tell "the
rem game is fullscreen and GDI cannot see it" from "the game never started".
exit
