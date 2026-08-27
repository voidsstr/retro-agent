REM ---------------------------------------------------------------
REM CONFIG.SYS.TPL - template for C:\CONFIG.SYS on a fleet DOS box.
REM
REM A TEMPLATE, NOT A STAGED FILE. CONFIG.SYS belongs to the machine and
REM carries its own drivers (CD-ROM, sound, SCSI). Merge these lines in by
REM hand; never copy this over a box's own file.
REM
REM HIMEM plus DOS=HIGH is the minimum a DOS game box wants: it is what puts
REM DOS in the HMA and leaves the 640K for the game. Verified on .243 - the
REM real-mode MEM report that PLAY.BAT writes into DOSGAME.LOG now reads
REM "Extended (XMS) 130,048K ... 129,980K free" and "MS-DOS is resident in
REM the high memory area".
REM
REM SHELL= sets the MASTER ENVIRONMENT SIZE. Without it DOS uses 256 bytes
REM and this machine already needs ~286: the boot variables (~165) plus
REM AUTOEXEC's BLASTER/MIDI (~47) plus PLAY.BAT's MTCPCFG/DGLOG (~62) plus
REM RUN.BAT's PATH prepend. Past the limit SET fails SILENTLY - %DGLOG%
REM expands to nothing, so the logging that diagnoses everything else turns
REM into "echo ... >>" with no target, and mTCP loses MTCPCFG, which is how
REM DHCP and HTGET find their configuration.
REM
REM ---- WHY THIS IS A BOOT MENU ----
REM
REM XMS alone is not enough for every game. Inca (Coktel's Gob engine, and
REM the same for its siblings) wants EXPANDED memory - EMS - and with no
REM EMM386 loaded MEM reports no EMS line at all, so the game says it has no
REM memory to work with even though 127 MB of XMS is sitting right there.
REM That is exactly what it reported on .243.
REM
REM EMM386 is also the classic cause of a Win98 box that hangs on the way
REM into MS-DOS mode, and this box has to come back unattended - it has no
REM supervisor for the agent and recovering it means walking to the
REM keyboard. So EMS is NOT the default: the menu keeps the known-good
REM XMS-only configuration as menudefault and gives EMS as a deliberate
REM choice for the games that need it. If EMM386 ever does wedge this box,
REM the fix is to pick 1 at the next boot, not a rescue disk.
REM
REM 5-second timeout, so an unattended reboot still lands on the safe
REM option without anyone present.
REM ---------------------------------------------------------------

[menu]
menuitem=BASE, Standard - XMS only (safe default)
menuitem=EMS, With EMS + upper memory (games that ask for expanded memory)
menudefault=BASE,5

[common]
REM Applies to both choices. The COMSPEC directory is the second argument;
REM /P makes the shell permanent.
SHELL=C:\COMMAND.COM C:\ /P /E:1024
DEVICE=C:\WINDOWS\HIMEM.SYS

[BASE]
DOS=HIGH

[EMS]
REM RAM (not NOEMS) is the point: NOEMS gives upper memory but NO expanded
REM memory, which is the half that Inca is missing. Conservative options
REM only - no HIGHSCAN and no hand-written I= ranges, both of which are what
REM turn EMM386 from "fine" into "hangs at boot" on 9x hardware.
DEVICE=C:\WINDOWS\EMM386.EXE RAM
DOS=HIGH,UMB

[common]
REM FILES=40 was marginal. INCAX.EXE ships the literal string
REM "Not enough FILES. See CONFIG.SYS." and Inca opens a large set of .ITK
REM and .STK archives at once (59 files, 18 MB, several over 1 MB). 60 costs
REM about a kilobyte and removes a whole class of "it just quit" report.
FILES=60
BUFFERS=20
STACKS=9,256
