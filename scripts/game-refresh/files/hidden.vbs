' hidden.vbs - run a command line with no visible window.
' Used to start refreshkeep.exe beside a game without a console window
' stealing focus from the fullscreen app (which on XP can bounce the game
' back to the desktop the moment it goes fullscreen).
'   wscript //nologo hidden.vbs "C:\RETRO_AGENT\refreshkeep.exe 100 quake2.exe"
If WScript.Arguments.Count = 0 Then WScript.Quit 1
CreateObject("WScript.Shell").Run "cmd /c " & WScript.Arguments(0), 0, False
