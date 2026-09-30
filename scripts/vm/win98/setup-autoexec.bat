@ECHO OFF
REM Win98 SE build VM - unattended setup floppy (retro fleet, 2026-09-30).
REM Stage 1: MBR code, system files, then Setup from C:\WIN98 with MSBATCH.INF.
REM Once C:\WINDOWS\WIN.COM exists Setup has rebooted: STOP, so the host can
REM detach this floppy and let the VM boot the hard disk.
SET EXPAND=YES
PATH=A:\
IF EXIST C:\WINDOWS\WIN.COM GOTO STAGE2
ECHO W98SETUP-STAGE1
A:\FDISK.EXE /MBR
A:\SYS.COM C:
C:
CD \WIN98
SETUP.EXE C:\WIN98\MSBATCH.INF /IS /IE /NM
GOTO END
:STAGE2
ECHO W98SETUP-STAGE2 - detach the floppy and boot the hard disk
:END
