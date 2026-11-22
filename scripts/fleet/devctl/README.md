# devctl - enable / disable / restart a devnode on an NT fleet box

The Win9x boxes have `devctl9x`; the NT boxes had nothing. Windows 7 ships no
`devcon`, and `.186` (Win7 x64) has no `pnputil` reachable from the agent
either - so a display adapter stuck in **code 43** could only be cleared by a
reboot, on a box whose monitor was already dark.

```
devctl list [<substring>]      # match on instance id OR hardware id
devctl disable <substring>
devctl enable  <substring>
devctl restart <substring>
```

It prints the device's status and problem code **before and after**, because a
`DIF_PROPERTYCHANGE` that returns success and changes nothing is this project's
signature failure.

## ⚠️ BUILD IT 64-BIT FOR A 64-BIT BOX

SetupAPI's install functions refuse a 32-bit caller on a 64-bit Windows with
**`ERROR_IN_WOW64` (0xE0000235, printed as `-536870347`)**. The 32-bit build
lists devices fine and cannot change one.

```bash
i686-w64-mingw32-gcc   -O2 -Wall -s -o devctl.exe   devctl.c -lsetupapi   # 9x/XP
x86_64-w64-mingw32-gcc -O2 -Wall -s -o devctl64.exe devctl.c -lsetupapi   # x64
```

The dev host currently has **only the i686 toolchain** (`gcc-mingw-w64-i686`);
`gcc-mingw-w64-x86-64` is not installed, and installing it needs an interactive
sudo password.

## THE AGENT IS A 32-BIT PROCESS - ON A 64-BIT BOX IT IS LYING TO YOU

This cost the `.186` investigation a whole session before it was spotted, and
it will do so again:

| what the agent reports | the truth on a 64-bit box |
|---|---|
| `SYSINFO` → `"architecture":"x86"` | WOW64. `GetSystemInfo` reports the *process*; only `GetNativeSystemInfo` reports the machine |
| `C:\Windows\System32\...` → "File Not Found" | redirected to `SysWOW64`. `pnputil.exe` lives only in the real System32, so it reads as absent |
| a SetupAPI change → `-536870347` | `ERROR_IN_WOW64` - the call was refused for being 32-bit |

**Decide bitness from `%PROCESSOR_ARCHITEW6432%` or the presence of
`C:\Windows\SysWOW64\ntdll.dll`, never from `SYSINFO`,** and reach the real
system directory through **`C:\Windows\Sysnative\`**. A fleetbook entry of
2026-11-13 recorded `.186` as "Win7 32-bit, so the GTX 1650 has no x86 driver"
purely from the `SYSINFO` line; the box is x64 and that conclusion was false.
