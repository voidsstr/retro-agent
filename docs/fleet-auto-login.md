# Fleet Auto-Login (agent survives reboots)

> ## ⚠️ THIS PAGE IS THE **WINDOWS XP / NT** RECIPE. IT DOES NOT APPLY TO WIN9x.
>
> Everything below — `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon`,
> `AutoAdminLogon`, `ForceAutoLogon`, `net user`, `reg.exe` — **does not exist on
> Windows 98**. Applying it there silently does nothing. See
> [Windows 9x auto-login](#windows-9x-auto-login-9498me) below, which is a
> different mechanism with a different trade-off.

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

---

# Windows 9x auto-login (95/98/ME)

Win9x is **not** NT. There is no `Windows NT\CurrentVersion\Winlogon` key, no
`net user`, and no `reg.exe`; the logon dialog comes from the *network* logon
provider. Tried and measured on **.243 (N5R5L9, Win98 SE)**, 2026-08-11:

| Approach | Result |
|---|---|
| `HKLM\Network\Logon` → `AutoLogon=1`, `username=admin`, `MustBeValidated=0` | **not enough on its own** — still stops at the logon dialog while the account has a password |
| `HKLM\Software\Microsoft\Windows\CurrentVersion\Winlogon` → `AutoAdminLogon` / `DefaultUserName` / `DefaultPassword` (the TweakUI values) | **no effect** — verified by reboot, still prompted |
| Agent moved to `...\CurrentVersion\RunServices` (runs before logon) | agent did not come up; and if it ever does fire alongside `Run`, it starts a **second instance** — see the port-race note below |
| **Blank the account's Windows password**, keep `AutoLogon=1` | **this is the one that works** — no dialog at all |

## The trade-off: a blank password costs you the share

The account's password is what encrypts `C:\WINDOWS\<user>.PWL`, and that cache
is what the box presents to an SMB server. Blanking the password on .243 turned
`\\192.168.1.122\files\Utility` from readable into **"Access denied"** on the
next connection.

So the two changes go together — never do the first without the second:

1. Blank the Windows password via **Control Panel → Passwords → Change Windows
   Password** (old password in, new + confirm left empty). Use the applet, not
   a `.PWL` deletion: the applet re-encrypts the cache instead of discarding it,
   and does not leave Windows prompting to create a new password at next logon.
   Remotely that is `LAUNCH rundll32.exe shell32.dll,Control_RunDLL password.cpl`
   plus a screenshot-click pass.
2. **Re-map the share with explicit credentials at every logon**, so nothing
   depends on the `.PWL` any more:

```
HKLM\Software\Microsoft\Windows\CurrentVersion\Run
  RetroShare = C:\WINDOWS\COMMAND\NET.EXE USE E: \\192.168.1.122\files password /SAVEPW:NO /YES
```

Back up the `.PWL` first (`copy C:\WINDOWS\ADMIN.PWL C:\WINDOWS\ADMIN.PW0`).

## Other Win9x gotchas that cost real time on .243

- **The agent's `REBOOT` command does not reboot a Win9x box.** It stops the
  agent and the machine stays up — observed directly at the keyboard. Neither
  does `rundll32.exe shell32.dll,SHExitWindowsEx 2` (uptime unchanged after).
  Until that is fixed in the agent, a Win9x reboot needs a human. **Do not
  treat "the agent went away after REBOOT" as evidence that a reboot happened**
  — it invalidates any auto-login test built on it.
- **The agent runs as a visible console window; closing that window kills it.**
  It is not a service — Win9x has no service manager.
- **"9898 refused + 9897 open" means two agent instances**, not a dead agent.
  Each start logs `Listening on TCP :9898+:9897` but the second only gets the
  port the first did not take. `PROCLIST` + `PROCKILL`; the survivor does not
  re-bind the free port, so a clean single instance needs a restart.
- **Use 90–180 s connect timeouts.** On the 31 MB Pentium-1 the auth handshake
  takes 40–150 s; a 10 s timeout reports a perfectly healthy agent as hung.
- **Keep the agent in `Run` while testing any alternative.** Removing it to try
  `RunServices` meant a failed test left the box with no agent at all and cost
  the operator a manual start.
