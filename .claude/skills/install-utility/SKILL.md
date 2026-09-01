---
name: install-utility
description: Install a fleet utility (PowerStrip, Motherboard Monitor, 7-Zip, Daemon Tools) onto a running retro PC over the agent, including the licence/registry steps that stop it nagging at startup. Use when asked to "install PowerStrip on .124", "put the display tuner on that box", "why does PowerStrip prompt at login", or to add a utility to a machine that was built before it was in the image.
---

# Installing a fleet utility on a running box

Newly imaged machines get these from `$OEM$` already. This skill is for a box
that was built earlier, or one where a utility needs reinstalling.

**Everything here is done over the agent on port 9898** — no keyboard at the
machine. `python3 /tmp/fleet.py <ip> '<COMMAND>'`, or the `mcp__retro__*` tools
from chat.

## The one rule that decides the whole approach

**Check whether the installer can be automated before trying.** Most of these
cannot:

| Utility | Installer | Automatable? |
|---|---|---|
| PowerStrip 3.90 | Gentee | **No** — the stub does not even import `GetCommandLineA`, so it never reads argv. No silent switch exists. |
| 7-Zip | NSIS | Yes — `/S` |
| Daemon Tools 3.47 | InstallShield | Partly — `/s /v"/qn"` |
| Motherboard Monitor 5.3.7 | SFX | **No** — and see the warning below |

A utility whose installer cannot be automated is installed **once in the build
VM**, captured, and staged into `$OEM$` — never run during setup.

## PowerStrip — the one people ask about

It is already in the image at `$OEM$\$Progs\PowerStrip` with its registry, so a
freshly imaged box has it. To put it on an older box:

```bash
S='\\192.168.1.122\files\Files\Utility\Retro Software'
python3 /tmp/fleet.py <ip> "EXECW 250 cmd /c start /wait \"\" xcopy \"$S\\powerstrip-captured\\Progs\\PowerStrip\" \"C:\\Program Files\\PowerStrip\\\" /E /I /Y"
python3 /tmp/fleet.py <ip> "EXECW 90 cmd /c copy /Y \"$S\\powerstrip-captured\\drivers\\PSTRIP.SYS\" C:\\WINDOWS\\system32\\DRIVERS\\"
python3 /tmp/fleet.py <ip> "EXECW 60 cmd /c copy /Y \"$S\\powerstrip-reg\\powerstrip.reg\" C:\\ps.reg && regedit /s C:\\ps.reg"
```

`xcopy` needs `start /wait` — see the gotcha at the bottom.

### Why it prompts at startup, and what actually fixes it

Two different things get conflated:

1. **The trial nag.** PowerStrip is 30-day shareware. EnTech published a product
   key *free of charge* for the discontinued PowerStrip 3 — it is on their own
   product page next to the download. That key is in
   `powerstrip-reg/powerstrip.reg`. This is the vendor's licence, not a bypass.
2. **The startup prompt itself.** That is the installer's **"Auto-load with
   Windows"** checkbox, which is TICKED by default. It puts PowerStrip on the
   boot path, where it loads a kernel driver and reprograms display timings on a
   machine nobody has asked it to touch. `powerstrip.reg` deletes the `Run`
   entries in both `HKLM` and `HKCU`.

Applying the key alone leaves it starting at every login. Both halves are needed.

### What you cannot pre-bake

PowerStrip shows a one-time **"New Hardware Found"** dialog per display adapter
on first launch, where it records that adapter's defaults. Those defaults are
hardware-specific, so a captured set from the build VM (a Cirrus adapter) would
be wrong everywhere else. Since it is off the boot path, this only appears when
somebody actually opens PowerStrip — leave it alone.

### Verify

```bash
python3 /tmp/fleet.py <ip> 'EXECW 40 cmd /c reg query "HKCU\Software\EnTech\PowerStrip" /v Key'
python3 /tmp/fleet.py <ip> 'EXECW 40 cmd /c sc query PStrip | find "STATE"'
python3 /tmp/fleet.py <ip> 'EXECW 40 cmd /c reg query "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Run" | findstr /i power'
```
Expect: a key present, the service `STOPPED` (it is `Start=3`, demand-only), and
**no** Run entry. A running PStrip service on an idle box means something started
it — find out what.

## Motherboard Monitor — ask before installing

MBM 5.3.7 works on XP, but it loads `mbmiodrvr.sys`: a 2,944-byte ring-0 port-I/O
shim created with `IoCreateDevice` and **no SDDL**, so it takes the default DACL.
Any local process that can open `\\.\mbmiodrvr` gets arbitrary I/O-port access —
a kernel primitive. On an isolated fleet where everything already runs as
Administrator this changes little in practice, but it is an unsigned always-loaded
ring-0 driver, so **confirm with the operator before putting it on machines**.

## Gotchas that cost real time here

- **`xcopy` needs a readable STDIN, and the agent gives its children none.**
  No output, no files, exit code 0 — xcopy asks "file or directory?" and reads
  the answer from stdin, so with no stdin handle it exits before doing anything.
  **The fix is one redirect: `cmd /c xcopy ... < nul`.** Measured on `.143`
  2026-09-01: `EXEC cmd /c xcopy /?` printed *nothing*, `xcopy /? < nul` printed
  the full help, and the same redirect turned a silent no-op into
  `147 File(s) copied`. `cmd /c start /wait "" xcopy ...` also works — it gives
  the child a console, and hence a stdin — but it detaches and discards the exit
  code, so prefer the redirect. Plain `copy` works fine either way.
- **A GUI installer needs a screen big enough for its buttons.** At 640x480 the
  PowerStrip and Unreal wizards have their Next button below the visible area.
  `C:\setmode.exe 800 600 16` first — and note 1024x768x32 fails on the build
  VM's Cirrus adapter, so drop the colour depth rather than assuming the mode.
- **`Ctrl+A` does not clear a Win32 edit control.** Send backspaces.
- **Registry text built with shell `printf` piped into python goes wrong
  quietly.** It has produced a literal `%s` in an `ImagePath` and a bare LF in a
  CRLF file. Generate a `.reg` in ONE python step, then check it: decode any
  `hex(2)` value as **ANSI** (REGEDIT4 is an ANSI format — decoding it as UTF-16
  is the same mistake that writes it wrong), and assert zero LF-only lines.
