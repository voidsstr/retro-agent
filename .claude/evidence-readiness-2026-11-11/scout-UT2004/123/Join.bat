@echo off
rem Connect straight to the fleet's own UT2004 server. No master server and no
rem internet needed - this is a direct connect on the LAN.
rem UE_USERINI: re-seed a User.ini that has no key bindings from DefUser.ini
find /c "[Engine.Input]" "%~dp0System\User.ini" >nul 2>&1
if errorlevel 1 echo fleet: System\User.ini has no key bindings - re-seeding it from DefUser.ini
if errorlevel 1 copy /Y "%~dp0System\DefUser.ini" "%~dp0System\User.ini" >nul

call "%~dp0FLEETRES.BAT"
if exist "%~dp0FLEETRES.EXE" (
  "%~dp0FLEETRES.EXE" -ini "%~dp0System\UT2004.ini" WinDrv.WindowsClient FullscreenViewportX %FR_W%
  "%~dp0FLEETRES.EXE" -ini "%~dp0System\UT2004.ini" WinDrv.WindowsClient FullscreenViewportY %FR_H%
  "%~dp0FLEETRES.EXE" -ini "%~dp0System\UT2004.ini" WinDrv.WindowsClient StartupFullscreen True
)

rem A UNIQUE PLAYER NAME, generated on the box. Two things depend on it:
rem   * UT2004 refuses to go online quietly while the name is still the
rem     stock "Player" - it raises "You have not fully configured your
rem     internet play options" on EVERY launch, which stops an unattended
rem     box dead at a modal dialog.
rem   * a LAN scoreboard of several boxes all called "Player" identifies
rem     nobody.
rem It CANNOT be a staged constant for exactly the reason the Westwood
rem Serial cannot be - one library, many machines - so it is written here
rem from %COMPUTERNAME% at launch.
if exist "%~dp0FLEETRES.EXE" (
  "%~dp0FLEETRES.EXE" -ini "%~dp0System\User.ini" DefaultPlayer Name %COMPUTERNAME%
)

cd /d "%~dp0System"
start "" "UT2004.exe" "192.168.1.132:7777"

rem Close this console. A .bat that ends at "start" leaves its window open for
rem the life of the game, and after a dozen launches box-145 had SIXTEEN of
rem them stacked on the desktop - which actively corrupted testing, because a
rem screenshot then showed a cluttered desktop and you could not tell "the
rem game is fullscreen and GDI cannot see it" from "the game never started".
exit
