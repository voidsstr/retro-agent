# Descent II (original Win95 engine): the Electronic Registration Card - 2026-10-03, `.124`

**Symptom.** The baseline sweep (`../sweep_baseline/`, 2026-10-01) recorded the
"Descent II - original Win95 engine" shortcut as a CHECK. A dialog titled
"Electronic Registration Card" was up, with `REGCARD.EXE` running beside
`DESCENTW.EXE`.

**Cause, from the binaries.**

- `DESCENTW.EXE` (staged, 940,032 B) builds `<CD>:\d2data\` in a global (the
  drive found by volume label `DESCENT_II`, VA `0x48bbf0`). At startup it appends
  `regcard.exe` and calls `CreateProcessA` (VA `0x48c250`). It then waits for the
  card to exit (`WaitForInputIdle`, then a `WaitForSingleObject` loop), and does
  this on **every** launch, unconditionally. A missing file only makes the call
  fail.
- `REGCARD.EXE` (16,896 B, 1996-10-17, `D2DATA\` on the disc image `d2disc.iso`)
  calls `GetPrivateProfileIntA("Registration Counters", "Times Bypassed", 0,
  "EREGREG.INI")`, then `cmp eax, 3 / jae` straight to exit. Below 3, it calls
  `_RegCard_Register` in `EREGREG2.DLL`: the full-screen form. A bare file name
  means `%windir%\EREGREG.INI`.
- Closing the card does **not** count as a bypass. After the baseline sweep closed
  it, `.124`'s file read `Times Registered=0`, `Times Bypassed=0`, `Times Run=1`
  (`eregreg_before_124.ini`).

**Proof by hand (`hand_bypass3/`).** With `Times Bypassed=3` written into
`.124`'s file, the shortcut went straight to the Interplay intro: no dialog, PASS.
The frames are 320x200, the game's own fullscreen DirectDraw mode. Their colours
are the known 8-bpp capture limit; the geometry is real.

**The fix is in the library.** It lives in the prelaunch step of
`provisioning/discmount/specs/Descent2.json`, from which the mount launcher is
generated, so it survives a regenerate:

- If `%windir%\EREGREG.INI` does not already say 3 or more, the launcher writes
  `[Registration Counters]` / `Times Bypassed=3` there.
- On Vista and later it also writes the UAC VirtualStore copy. Under UAC, cmd
  cannot write `%windir%`, while a legacy exe's INI read is redirected to that
  copy. **The VirtualStore path is not yet measured on a Win7 box.**
- If neither copy took, it says so.

The launcher is published to the share as 26,428 B, md5 `ceda6c59`, verified
through `/mnt`, and matches template + spec.

**Proof from the library (`library_fix/`).** On `.124`:

1. Purged `C:\Games\Descent2` (verified gone), its 7 desktop shortcuts and the
   hand-written `eregreg.ini`.
2. `GAMESYNC RESET` + `START`: `state=done`, `failed_files 0`, 47 done + 69 gated
   = `titles_total` 116 = the library's 116 titles. The launcher on the box is md5
   `ceda6c59`, and there is no `eregreg.ini`.
3. Sweep: **PASS**, no dialog while running or on close. The game had the focus at
   30 s and 60 s, and the board stayed healthy.
4. The launch wrote `[Registration Counters]\r\nTimes Bypassed=3\r\n`
   (`eregreg_written_by_launcher_124.ini`).

Test: `tests/python/test_descent2_regcard.py`. It pins the launcher lines and,
when the share is mounted, reads `REGCARD.EXE` out of the disc image. It checks
the counter's section, key, file and threshold against what the launcher writes.
