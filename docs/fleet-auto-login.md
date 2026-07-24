# Fleet Auto-Login (agent survives reboots)

Every connected fleet machine is set up for **Windows XP auto-login** so that after a
reboot it logs directly into the console session and the retro agent restarts on its
own. This is what lets us reboot a headless box (driver install, `SYSFIX`, etc.) and
have it come back on the LAN without a keyboard/monitor.

Two pieces make it work together:

1. **Auto-login** — Winlogon logs the console account in automatically at boot.
2. **Agent auto-start** — the agent is in the per-machine `Run` key, so it launches
   inside that auto-logged-in session.

If either is missing, a reboot leaves the box sitting at the login screen with no
agent → unreachable until physical access.

## Configuration

All values under `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon`:

| Value | Type | Setting |
|-------|------|---------|
| `AutoAdminLogon` | REG_SZ | `1` |
| `ForceAutoLogon` | REG_SZ | `1` — re-applies auto-login even after a manual logoff/lock ("fully enabled") |
| `DefaultUserName` | REG_SZ | the box's **real console account** (must be an existing local account) |
| `DefaultDomainName` | REG_SZ | the box **hostname** (these are local accounts, not domain) |
| `DefaultPassword` | REG_SZ | `password` (plaintext — XP auto-login requires it stored) |

Plus the agent auto-start:

| Key | Value | Data |
|-----|-------|------|
| `HKLM\Software\Microsoft\Windows\CurrentVersion\Run` | `RetroAgent` | `C:\RETRO_AGENT\retro_agent.exe -l C:\RETRO_AGENT\agent.log` |

## Fleet convention

The console account's **Windows password is `password`** on every box (same as the
SMB-share convention). Keeping it uniform means `DefaultPassword=password` is always
correct.

## Per-box console accounts (2026-07)

| IP | Hostname | GPU | Console account (`DefaultUserName`) | Notes |
|----|----------|-----|-------------------------------------|-------|
| .124 | ADMIN | Voodoo3 | `voidsstr` | reference config; left as-is during driver-opt session |
| .143 | 1GHZ | Voodoo5 | — | untouched during driver-opt session |
| .123 | 2004-XP | Radeon HD 3850 / GeForce4 Ti | `Administrator` | |
| .240 | USER-41EA3B3330 | Matrox Parhelia LX | `User` | (was stale `admin`; fixed) |
| .145 | DELL | Intel HD | `voidsstr` | (was stale `admin`; fixed) |

## How to (re)apply on a box

Do these together so the account password and `DefaultPassword` can never diverge:

```bat
:: 1. find the REAL console account (do NOT assume "admin")
echo %USERNAME%

:: 2. set that account's password to the fleet convention
net user <account> password

:: 3. set the five Winlogon values
set W=HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon
reg add "%W%" /v AutoAdminLogon   /t REG_SZ /d 1        /f
reg add "%W%" /v ForceAutoLogon   /t REG_SZ /d 1        /f
reg add "%W%" /v DefaultUserName  /t REG_SZ /d <account> /f
reg add "%W%" /v DefaultDomainName /t REG_SZ /d <hostname> /f
reg add "%W%" /v DefaultPassword  /t REG_SZ /d password  /f

:: 4. ensure the agent auto-starts
reg add "HKLM\Software\Microsoft\Windows\CurrentVersion\Run" /v RetroAgent /t REG_SZ ^
  /d "C:\RETRO_AGENT\retro_agent.exe -l C:\RETRO_AGENT\agent.log" /f
```

## Gotchas (learned the hard way)

- **`DefaultUserName` must be a real local account.** Several boxes shipped with a
  stale `DefaultUserName=admin` that doesn't exist as an account — auto-login then
  fails silently and the box never comes back after a reboot. Always read the actual
  console user with `echo %USERNAME%` and use that.
- **Never set `DefaultPassword` alone.** If you point it at a value that isn't the
  account's real password, the next reboot locks the box out (no login → no agent →
  unreachable, needs physical access). Always change the account password with
  `net user` in the same operation.
- **The loopback credential test lies.** `net use \\127.0.0.1\IPC$ /user:host\user
  password` returns *error 1326 (bad password)* even when the password is correct,
  because of XP's local-account network-access policy. A failure there is **not**
  evidence the console password is wrong — confirm by the account existing and its
  password being set to the same value as `DefaultPassword`.
- **`ForceAutoLogon=1`** is what makes auto-login robust against a stray manual logoff;
  plain `AutoAdminLogon=1` alone can drop out of auto-login in some states.
