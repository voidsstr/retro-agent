REM Run setrefresh_boot.bat hidden (0 = no console flash) at logon.
CreateObject("WScript.Shell").Run "C:\setrefresh_boot.bat", 0, False
