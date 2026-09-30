@ECHO OFF
REM Win98 build VM first logon: Run key + auto-logon, once. WIN.INI run= calls this every logon; the flag makes it a no-op after the first.
IF EXIST C:\RETRO_AGENT\VMSETUP.FLG GOTO END
C:\WINDOWS\REGEDIT.EXE /s C:\RETRO_AGENT\VMSETUP.REG
ECHO done > C:\RETRO_AGENT\VMSETUP.FLG
CALL C:\RETRO_AGENT\AGENTRUN.BAT
:END
