# ADMIN-PC (192.168.1.195, was .246) - why it "was crashing"

Investigated 2026-09-28 12:15-12:35 EDT, read-only (no reboot, no settings
changed; the scratch dir `C:\RETRO_AGENT\diag\` was created for event-log
exports and removed afterwards, verified gone).

Box: Dell OptiPlex 790, i5-2400, AMD Radeon HD 5450 (1002:68F9), Windows 7
Professional **RTM 6.1.7600, no service pack**, AMD Catalyst 15.7.1
(driver 15.200.1062.1004, 2015-08-03; `atikmdag.sys` 8.1.1.1500,
`atikmpag.sys` 8.14.1.6463). Agent now 1.89.1.

## Verdict

**The OS did not crash (no bugcheck ever). The box FROZE twice on 2026-09-26.
Both times the AMD display driver hung (TDR) and never recovered, and both
times a person ended it by holding the power button.**

| | what | proof |
|---|---|---|
| 1 | 09-26 ~15:11:47 EDT: display driver timeout, about 5 min into **Red Alert 2: Yuri's Revenge** | `C:\Windows\LiveKernelReports\WATCHDOG\WD-20130925-2116.dmp`: BugCheck **0x117 VIDEO_TDR_TIMEOUT_DETECTED**, P2 = `atikmpag.sys+0x9F24`. Prefetch: FLEETRES.EXE, RA2MD.EXE, GAMEMD.EXE at box 21:11:13-26. DWM 9010 "Main executable for Yuri's Revenge" at box 21:11:16. |
| 1b | 7 s later: **power button held**, hard power-off | Kernel-Power 41 (logged at the 15:40 boot): BugcheckCode 0, **PowerButtonTimestamp = box 2013-09-25 21:16:35**. 6008: "previous shutdown at 9:16:32 PM ... unexpected". Off about 28 min, next boot 15:40:32 EDT. |
| 2 | 09-26 **23:43:06 EDT**: the same display driver timeout; no game was started after 23:31 | `WD-20260926-2343.dmp`: BugCheck **0x117**, P2 = `atikmpag.sys+0x9F24` (**the same code offset**). |
| 2b | Recovery never completed. The **desktop was dead for 35.5 h** while the kernel and services kept running | No Display 4101 ("stopped responding and has successfully recovered") anywhere in the System log, and no 0x116 either. Instead there are **142 consecutive** Security-SPP 8197 "Failure displaying Software Licensing notification **0x80080005**", every 15 min from **09-26 23:52:06 to 09-28 11:07:06** (never before in the log), plus DCOM 10010 at 23:52:06 and 09-28 00:07:06. No GUI process could start in the logged-on session. The EventLog service, W32Time (09-27 01:00) and SMB (139/445 and NetBIOS answering on 09-28 09:30-09:46) were all alive. |
| 2c | 09-28 **11:13:54 EDT**: **power button held** again | Kernel-Power 41: BugcheckCode 0, **PowerButtonTimestamp = 2026-09-28 11:13:53.98 EDT**. 6008 last alive 11:13:39. Boot at 11:14:56 (`LastBootUpTime`). |

**No minidumps** (`C:\Windows\Minidump` and `MEMORY.DMP` are absent), no
BugCheck 1001, and every Kernel-Power 41 on the box has BugcheckCode 0.

### Why the agent "crashed" too: it was wedged by the hung display, it did not crash

- The agent answered its last command at **23:46:37** (3.5 min after the TDR).
  Host `retro-gameindex` then got **TimeoutError** at 23:48:26, 23:51:58 and 23:53:46,
  and **ConnectionRefused** from 23:56:38. The chat daemon's long-poll connection
  (PROMPT_WAIT, which is deliberately not logged) stayed up until **09-27 22:56:20**
  (TimeoutError), then got refused.
- Nothing after 23:46:37 ever reached `agent.log`. The hourly GAMEINDEX line, the
  retrowall lines, every "Connection from" and the 15-s flusher all stopped,
  although the disk worked (the event log kept writing for 35 h). There is no
  Application Error or WER report for `retro_agent.exe`.
- **Likely mechanism (source-level, unproven on the box):** `agent/src/log.c`
  `raw_out()` echoes every log line to the console with
  `WriteFile(GetStdHandle(STD_ERROR_HANDLE))` **while holding `g_log_cs`**
  (lines 209-213, still present in master / v1.89.1). On NT the console is
  conhost, a GUI process. When the display hangs, that console write can block
  forever, and then every thread that logs blocks behind `g_log_cs`. That
  matches the observed shape: no new connections are served (the accept path
  logs), the flusher cannot flush, and only the un-logged paths (long-poll,
  discovery) keep answering. The same wedge would follow from a console window
  left in Mark/Select mode (QuickEdit is 0 on this box, so a plain click will
  not do it).
- The agent's own watchdog cannot catch this. It fires only when a command is
  in flight for more than 75 s **and** a known game exe is running, and its
  remedy (`ChangeDisplaySettings`) would itself block on a hung GPU.

### Ruled out

- **BSOD / OS crash**: no bugcheck in any of the 5 Kernel-Power 41 events, no dump, no BugCheck 1001.
- **Unexpected power loss** for the 09-26 and 09-28 events: PowerButtonTimestamp is set on both, so the button was pressed.
- **Agent auto-update restarts**: 1.81.1→1.84.1 (09-24 22:41), 1.84.1→1.85.2 (09-26 15:40:40), 1.85.2→1.85.3 (09-26 23:31:54), 1.85.3→1.89.1 (09-28 11:17:16). Each is a clean "log closed (clean shutdown)" hand-over with the new instance up within about 1 s, each at agent startup, and none coincides with a freeze.
- **agent process crash**: no Application Error 1000 or WER report for retro_agent.exe, ever.
- **Disk**: no Disk 7/11/153 or volmgr errors anywhere in the retained System log (back to 08-07). The only disk event is one Disk 51 paging warning on `\Device\Harddisk1` (a second disk, not the system disk) on 08-29 12:47.
- **WHEA / CPU hardware errors**: none.
- **Windows activation lockout**: Win7 is in *Notification* mode (`slmgr /dli`: License Status Notification, 0xC004F009 grace time expired). That produces the hourly black-wallpaper reset seen in agent.log, but it never blocks logon on Win7.
- **Other orderly shutdowns** were user power-offs (1074 winlogon "on behalf of admin-PC\admin"): 09-04 23:17:20, 09-24 23:07:34, and 09-26 16:03:42 (during the network re-cabling). Those are not crashes.

### What is NOT proven (the cause of the TDRs themselves)

Why the Radeon hangs is **unproven**. The candidates, most likely first:

1. **Driver/OS mismatch**: Catalyst 15.7.1 on Windows 7 **RTM**. AMD's
   15.x drivers target Win7 SP1. This driver's user-mode half is already
   demonstrably broken on this OS. `atiuxpag.dll` 8.14.1.6463 crashes Dwm.exe
   (install day) and winsat.exe (07-30, 08-02, 08-09) at the **same offset
   0x887f**, which is why DWM/Aero never runs here. Win7 RTM's dxgkrnl also lacks
   the SP1 TDR-recovery fixes, which would explain a recovery that neither
   completes (no 4101) nor bugchecks (no 0x116).
2. Legacy DirectDraw/8-bit titles driving the card (TDR #1 was 5 min into
   Yuri's Revenge; the 08-31 10:50 freeze was 19 s after a C&C launch during a
   game-test pass).
3. Hardware: a passively cooled HD 5450 overheating, or a failing card.
   `MSAcpi_ThermalZoneTemperature` is not supported, so temperature is unknown.

TDR #2 happened with no game launched after 23:31 (Prefetch). The last human
activity was 23:31:18-23:31:39 (network link up, `ipconfig`, agent console
closed and relaunched). The 600-s Starfield screensaver would have started at
about 23:41:40, but its Prefetch entry was overwritten today, so that link is
unproven.

## Timeline

Clocks: the box clock ran **2013 and 6h04m41s AHEAD** of real EDT until the
agent's clockfix at 09-26 15:40:57 EDT (CLOCKFIX log line). After that it
matches host EDT to within 2 s (checked 12:20:31 vs 12:20:29). Event-log and
agent-log times before the fix are box times. Converted EDT is shown here.

| host EDT | event | source |
|---|---|---|
| 08-09 06:44, 08-10 17:38 | older unexpected shutdowns (08-10: power button); pre-fleet, no live dump | System 41 |
| 08-31 10:50:38 | froze during a rapid game-launch test pass (C&C at 10:50:19); power-button orderly shutdown 10:57:27 | 6008 / 109 / DWM 9010 |
| 08-31 14:27:55 | unexpected shutdown, no power-button stamp, no bugcheck, no live dump (during HL/Quake2 testing); cause unknown | 41 / 6008 |
| 09-04 23:17:20 | orderly power-off by admin; box clock still correct | 1074; agent log 23:17:05 ≈ host 23:17:07 |
| 09-24 11:52-17:39 | house mains outage (host-issues-log); box was off | docs/host-issues-log.md |
| 09-24 22:38:59 | boot; **box clock now reads 2013** (RTC lost about 13 years while the box was off, which suggests the CMOS battery) | 12 / agent log |
| 09-24 23:07:34 | orderly power-off by admin | 1074 |
| 09-26 15:04:47 | boot | 12 |
| 09-26 15:06:05 | agent's console window closed (console control event 2), 4 s after start; agent dead until 15:40 | agent.log |
| 09-26 ~15:06:35 | Yuri's Revenge launched | Prefetch, DWM 9010 |
| **09-26 ~15:11:47** | **TDR #1** (0x117, atikmpag) | WD-20130925-2116.dmp |
| **09-26 ~15:11:54** | **power button held** | 41 PowerButtonTimestamp |
| 09-26 15:40:32 | boot; agent 1.84.1→1.85.2; clockfix 2013→2026 at 15:40:57 | 41/6008, agent.log |
| 09-26 15:43:56 | earlier session read the event logs here (no minidump found) | agent.log EXEC |
| 09-26 16:00-16:02 | NIC link flaps during network rework; IP .246 → .195 | e1cexpress 27 |
| 09-26 16:03:42 | orderly power-off by admin | 1074 |
| 09-26 16:20:01 | boot with **link down** (agent saw IP 127.0.0.1) | 12, e1cexpress 27 |
| 09-26 23:31:18-39 | link up, `ipconfig`, agent console closed and relaunched (1.85.2→1.85.3) | e1cexpress 32, Prefetch, agent.log |
| **09-26 23:43:06** | **TDR #2** (0x117, atikmpag+0x9F24) | WD-20260926-2343.dmp |
| 09-26 23:46:37 | last line the agent ever logged in this boot | agent.log |
| 09-26 23:48-23:56 | gameindex: Timeout ×3, then Refused | journalctl --user -u retro-gameindex |
| 09-26 23:52:06 → 09-28 11:07:06 | **142× SPP 8197 0x80080005**: session cannot start GUI processes | Application log |
| 09-27 22:56:20 | chat daemon's long-poll finally times out; then refused | journalctl --user -u retro-chat-daemon |
| 09-28 09:30-09:46 | 139/445/NetBIOS answer, 9898 does not (task report) | |
| **09-28 11:13:54** | **power button held** (last alive 11:13:39) | 41 PowerButtonTimestamp, 6008 |
| 09-28 11:14:56 | boot; agent 1.85.3→1.89.1 at 11:17:16 | LastBootUpTime, agent.log |

## Other facts found on the way

- `wmic os get InstallDate` = **2013-09-26 01:57:29**. Windows was installed
  while the RTC read 2013, so the 30-day activation grace was spent long ago.
  Win7 answers that with Notification mode (hourly black wallpaper, nag), not a
  lockout.
- The agent's console window ("Retro Remote Agent Version 1.89.1") is
  **visible on the desktop**, and a person closed it twice on 09-26 (15:06:05
  and 23:31:29). The first close left the box without an agent for 35 min.
- Run key: `C:\RETRO_AGENT\retro_agent.exe -l C:\RETRO_AGENT\agent.log`. Also
  Run: `StartCCC` (AMD CCC), `MapShare`, `StarCraftCD` (WinCDEmu batchmnt).
- TDR registry is at defaults (no TdrDelay/TdrLevel under GraphicsDrivers).

## Recommended fixes (none applied; operator decision)

1. **Graphics stack (needs reboots, so it is not something to do blind):**
   either install Windows 7 **SP1** (KB976932) so Catalyst 15.7.1 is on a
   supported OS (SP1 also brings the dxgkrnl TDR fixes), or replace the AMD
   driver with a release that lists Win7 RTM. Use `scripts/fleet/safe-reboot.py`
   for every reboot. Win7 Notification mode does not block logon, so activation
   is not the risk here.
2. **Hardware check at the box**: the HD 5450 heatsink and airflow (reseat the
   card), and the **CMOS coin cell**. The RTC lost about 13 years between 09-04
   and 09-24 while the box was off through a mains outage.
3. **Agent (code, remote-safe via auto-update):** move the console echo in
   `agent/src/log.c` `raw_out()` **outside `g_log_cs`**, or make it
   non-blocking or thread-offloaded, so a hung display, a frozen conhost or a
   console in Mark mode cannot wedge the whole agent. Add a source-invariant
   regression test. Consider minimizing or hiding the console on NT so people
   stop closing it (and/or treat CTRL_CLOSE as a restart on NT).
4. **Make it visible host-side:** the chat daemon held an "established"
   long-poll to a wedged agent for 23 h. A periodic fresh-connection `PING`
   (which exercises the logged accept path) would have flagged it at 23:48.

## Evidence (this directory)

- `WD-20130925-2116.dmp` (md5 3c4967c23fe0a8a19d5c22466cf0f652) and
  `WD-20260926-2343.dmp` (md5 8e5dd1a1f5f6334144916e4d30384e42): both are Win7
  triage live dumps (DUMP_HEADER32 `PAGEDUMP`, 15.7600, DumpType 4). BugCheck
  0x117, P1 0x8567A320 / 0x8579F510, P2 0x90A38F24 / 0x9083FF24, both at
  `atikmpag.sys` + 0x9F24 in the dump's own driver list; current process `System`.
- `k41.xml`: every Kernel-Power 41 with its BugcheckCode and PowerButtonTimestamp.
- `sys_boot.txt`: boot/shutdown/1074/6008/41/time-change events (200).
- `sys_err.txt` and `sys_err_rows.txt`: System Level 1-3 (400, back to 08-07).
- `app_err.txt` and `app_err_rows.txt`: Application Level 1-2 (atiuxpag faults, hl.exe crashes, no retro_agent).
- `a2.txt`: Application 8197/9010/1002/4105 (the 142-event SPP series and the DWM game launches).
- `s2.txt`: System event 26 (processor power only; no Application Popup).
- `w1.txt`, `w3.txt`, `w4.txt`, `wa.txt`: all events in the incident windows.
- `prefetch.json`: `C:\Windows\Prefetch` listing (last-run times, UTC).
- `wer.txt` (only Windows Update reports) and `dumps.txt` (no Minidump, no MEMORY.DMP).
- `agent.log` and `agent.log.1`: the agent's own log, captured before anything touched the box.
