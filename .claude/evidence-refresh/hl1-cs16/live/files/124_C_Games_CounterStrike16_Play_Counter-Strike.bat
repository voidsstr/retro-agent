@echo off
rem Counter-Strike 1.6 (No Steam V42, build 4554, protocol 48) - fleet launcher.
rem
rem Counter-Strike.exe does nothing except spawn "hl.exe -game cstrike", so this
rem batch does the same and adds -full, which is what makes the fullscreen
rem requirement part of the STAGED tree instead of a per-box setting.
rem
rem For a screenshot through the agent's GDI capture, run
rem   hl.exe -game cstrike -window -w 800 -h 600
rem by hand instead - a fullscreen GoldSrc window captures as a black frame.
rem In-game, F11 is bound to "snapshot" (cstrike\autoexec.cfg).
call "%~dp0FLEETRES.BAT"

cd /d "%~dp0"
rem
rem MESA_FORCE_SSE=1 IS LOAD-BEARING ON A VOODOO 4/5 (added 2026-09-23, .124 V5 6000).
rem   The AmigaMerlin 3dfxOGL ICD is Mesa-based. At wglCreateContext it runs
rem   Mesa's "OS support for SSE unmasked exceptions" probe: it unmasks
rem   divide-by-zero and executes a divps by zero ON PURPOSE, expecting its
rem   SetUnhandledExceptionFilter hook to swallow the trap. GoldSrc wraps video
rem   init in its own exception handler, which catches the trap first and
rem   shuts the engine down; hw.dll is unloaded while its window still exists
rem   and the next window message jumps into freed memory (0x035e8fa0) -
rem   the "hl.exe - Application Error" popup. Caught under ntsd. This variable
rem   makes Mesa skip the probe (XP supports SSE); it keeps the SSE paths and
rem   is read by nothing but a Mesa ICD, so it is inert on every other card.
set MESA_FORCE_SSE=1
rem
rem -gl IS LOAD-BEARING AND MUST NOT BE REMOVED (added 2026-08-31, measured on .143).
rem   Without it this engine starts on the SOFTWARE renderer. Proved on hardware,
rem   not inferred: HKCU\Software\Valve\Half-Life\Settings\EngineDLL read "sw.dll"
rem   after a plain -full launch and "hw.dll" after the identical launch with -gl,
rem   same box, same resolution. The game still runs and still reports itself as
rem   fullscreen either way, which is exactly why this went unnoticed - nothing
rem   errors, it is just software-rendered.
rem   This is the SAME defect already fixed in the HalfLife1 tree on 2026-08-29
rem   (every launcher there passes -gl); the fix was never applied here.
start "" hl.exe -game cstrike -full -gl -w %FR_W% -h %FR_H%

rem Close this console. A .bat that ends at "start" leaves its window open for
rem the life of the game, and after a dozen launches box-145 had SIXTEEN of
rem them stacked on the desktop - which actively corrupted testing, because a
rem screenshot then showed a cluttered desktop and you could not tell "the
rem game is fullscreen and GDI cannot see it" from "the game never started".
exit
