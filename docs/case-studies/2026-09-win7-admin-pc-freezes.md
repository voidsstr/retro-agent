# Case Study: ADMIN-PC (Win7) "Crashing" — Two Display Timeouts, Two Power-Button Resets, a Wedged Agent

**Box:** ADMIN-PC, `192.168.1.195` (it was `.246` until DHCP moved it during the
2026-09-26 network re-cabling). Dell OptiPlex 790, Core i5-2400, AMD Radeon HD 5450
(`1002:68F9`), **Windows 7 Professional RTM 6.1.7600, no service pack**, AMD Catalyst
15.7.1 (driver 15.200.1062.1004, 2015-08-03; `atikmdag.sys` 8.1.1.1500,
`atikmpag.sys` 8.14.1.6463). Agent 1.89.1 at the time of writing.

**Investigated** 2026-09-28 12:15-12:35 EDT, read-only: no reboot, no settings
changed. The scratch directory `C:\RETRO_AGENT\diag\` held the event-log exports
and was removed afterwards (verified gone). **Corrected** the same day after two
independent reviews of the first write-up found overclaims. They are listed in
[Corrections](#corrections-to-the-first-write-up), because the first version had
already been copied into `retro-3dfx/FINDINGS.md`.

## The honest headline

| | |
|---|---|
| **PROVEN** | Two display **TDRs** (bugcheck 0x117 *live* dumps; the OS did not crash), each followed by **a person holding the power button**: 6 s after TDR #1, and 35.5 h after TDR #2, with the session unhealthy in between. Separately, **the agent stopped answering (wedged) from about 23:47 on 09-26** while the kernel, services and SMB kept running for 35 h. The OS never bugchecked. |
| **LIKELY, NOT PROVEN** | The cause of the GPU hangs: **AMD Catalyst 15.7.1** (a driver built for Win7 **SP1**) running on Win7 **RTM**, and/or a **GPU hardware, thermal or power** fault. The evidence cannot tell these apart. |
| **UNEXPLAINED** | See [Unexplained events](#unexplained-events): a mass service termination on 08-31, two unexpected shutdowns without a power-button stamp, and what triggered TDR #2 with no game running. |

So the answer to "why does it crash" is: *it does not crash; the GPU stops
responding, the screen freezes, and someone powers it off.* The question of why the
GPU stops responding is still open.

## What is proven

| # | host EDT | what | proof |
|---|---|---|---|
| 1 | 09-26 **15:11:27** | **TDR #1**, about 5 min into *Red Alert 2: Yuri's Revenge* | `C:\Windows\LiveKernelReports\WATCHDOG\WD-20130925-2116.dmp`: header BugCheck **0x117 VIDEO_TDR_TIMEOUT_DETECTED**, P1 `0x8567A320`, P2 `0x90A38F24` (= `atikmpag.sys+0x9F24`), P3 0, P4 0. The dump's own SystemTime is box `2013-09-26 01:16:28.56 UTC`. Prefetch shows FLEETRES.EXE, RA2MD.EXE and GAMEMD.EXE at box 21:11:13-26 (15:06:11-24 EDT); DWM 9010 "Main executable for Yuri's Revenge" at box 21:11:16 (15:06:14 EDT). |
| 1b | 09-26 **15:11:33** | **Power button held** (hard power-off), about 6 s after the TDR | Kernel-Power 41 logged at the next boot: BugcheckCode 0, **PowerButtonTimestamp** `130246317952448441` = box `2013-09-26 01:16:35 UTC`. 6008: "previous system shutdown at 9:16:32 PM ... was unexpected" (15:11:30 EDT). Next boot at 15:40:11 EDT. |
| 2 | 09-26 **23:43:06** | **TDR #2**. No game had been started since 23:31 (Prefetch). | `WD-20260926-2343.dmp`: BugCheck **0x117**, P1 `0x8579F510`, P2 `0x9083FF24` (= `atikmpag.sys+0x9F24` again), P3 0, P4 0. SystemTime `2026-09-27 03:43:06.31 UTC`, after the clock fix. |
| 2a | 09-26 23:46:37 → ~23:47:56 | **The agent wedged.** It logged and served normally for 3.5 min after TDR #2, then stopped | Last `agent.log` line 23:46:37 (a `GAMEINDEX HASH` served to the host). The host's `retro-gameindex` request that began about 23:47:56 timed out at 23:48:26; more TimeoutErrors at 23:51:58 and 23:53:46, then **ConnectionRefused** from 23:56:38. Nothing after 23:46:37 ever reached `agent.log`, not even the 15-s flusher, although the disk was working (the event log kept writing for 35 h). No Application Error or WER report for `retro_agent.exe`. |
| 2b | 09-26 23:52:06 → 09-28 11:07:06 | **The logged-on session could not start COM servers** | **142 consecutive** Security-SPP 8197 "Failure displaying Software Licensing notification **0x80080005**" (CO_E_SERVER_EXEC_FAILURE), every 15 min, never before in the log, plus DCOM 10010 for `{F87B28F1-DA9A-4F35-8EC0-800EFCF26B83}` at 23:52:06 and 09-28 00:07:06. The kernel and services were alive throughout: the EventLog service, W32Time (a time sync at 09-27 01:00) and SMB (139/445 and NetBIOS answering on 09-28 09:30-09:46 while 9898 did not). |
| 2c | 09-28 **11:13:54** | **Power button held** again | Kernel-Power 41: BugcheckCode 0, PowerButtonTimestamp = `2026-09-28 15:13:53.98 UTC` (11:13:54 EDT). 6008: last alive 11:13:39. Boot 11:14:56 (`LastBootUpTime`). |

**The OS never bugchecked.** All five Kernel-Power 41 events on the box carry
BugcheckCode 0. There is no `C:\Windows\Minidump`, no `MEMORY.DMP` and no
BugCheck 1001 event. A Win7 TDR writes a *live* triage dump to
`LiveKernelReports\WATCHDOG`, which is where to look.

## Corrections to the first write-up

The first version (`DIAGNOSIS.md` in the evidence directory, and the
`retro-3dfx/FINDINGS.md` entry from commit `518aa4d`) said more than the evidence
supports. These are the corrected claims.

1. **`atikmpag.sys+0x9F24` is not a bug signature.** It was presented as "the same
   code offset both times", which reads like one bug hit twice. P2 of a 0x117 is the
   driver's **`DxgkDdiCollectDbgInfo` callback pointer**. That pointer is identical
   for *every* TDR on this driver build, whatever caused it. So it does not
   distinguish a driver bug from a GPU, thermal, PCIe or power fault. The two
   matching values tell us only that both TDRs involved the same driver.
2. **A missing Display 4101 does not prove the TDR never recovered.** Event 4101
   ("Display driver stopped responding and has successfully recovered") is written
   by user-mode `DispCI.dll`. If the session's user mode was stuck, the event
   would be missing even if the kernel-side recovery had worked. The SPP/DCOM
   series (2b) shows that the session was unhealthy. It does not show whether the
   GPU reset completed. The first version's explanation (Win7 RTM's dxgkrnl lacks
   the SP1 TDR-recovery fixes) is a hypothesis, not a finding.
3. **"The hung display immediately blocked the agent's console write" is not
   shown.** The agent logged, and so echoed to its console, normally until
   **23:46:37**, which is **3.5 min after** TDR #2 at 23:43:06. The wedge is
   bracketed between 23:46:37 (the last logged line) and about 23:47:56 (the first
   request that got no answer). Two other explanations fit the evidence just as
   well:
   - the session or console host hung later than the TDR;
   - the agent's console was put into Mark/Select mode. `HKCU\Console` has
     QuickEdit = 0, so a plain click would not do it, but a per-window console
     setting was not checked, so a click into the console is not excluded.

   The source-level mechanism is real and still worth fixing.
   `agent/src/log.c` `raw_out()` echoes each line with
   `WriteFile(GetStdHandle(STD_ERROR_HANDLE))` **while holding `g_log_cs`**, so any
   console write that blocks stalls every logging thread. That is still the case on
   master at `58af178`. It is a plausible way for the agent to wedge. It is **not**
   proven to be what happened here.
4. **The box-to-host clock offset is 6h05m02s, not 6h04m41s.** The agent's clockfix
   set the clock at box `2013-09-26 01:45:38.65 UTC` to `2026-09-26 19:40:57 UTC`
   (event 1). About 16 s later, at box 19:41:13.15, something stepped the clock
   **back 21.23 s** to 19:40:51.92. So the clock the fix set was 21 s fast, and the
   true offset is 6h04m41.65s + 21.23s = **6h05m02s**. Two host-side events confirm
   this:
   - the agent's box-time start at 21:10:43 corresponds to the chat daemon's
     "new agent online" at **15:05:40.8**;
   - the console-close at box 21:10:46 corresponds to the daemon's
     "wait conn lost: ConnectionResetError" at **15:05:44.2**.

   With the old offset, both box events would land about 21 s *after* the host
   saw them. Every pre-fix time in this document uses the corrected offset, so
   **TDR #1 was about 15:11:27 EDT**, not 15:11:47.
5. **The chat daemon's 09-27 22:56:20 long-poll loss was host-side.** The first
   version used it as the moment the wedged agent finally dropped its last
   connection. In fact **six agents** lost their long-poll within 4 min
   (`.243` 22:52:49, `.110` 22:54:53, `.249` 22:56:00, `.195` 22:56:20,
   `.124` 22:56:26, `.171` 22:56:30; `journalctl --user -u retro-chat-daemon`).
   That was an event on the host or the network, not on this box, and it says
   nothing about when the agent died.
6. **The link from `atiuxpag.dll` faults to "Aero never runs here" was dropped.**
   `atiuxpag.dll` 8.14.1.6463 (the Catalyst user-mode half) *did* crash Dwm.exe once
   (box install day) and winsat.exe on 07-30, 08-02 and 08-09. That is real evidence
   that this driver is unhappy on this OS. However, the agent's own XP-era
   Themes-service handling has also stripped Aero on this very box (fleetbook recipe
   #40), so the crashes do not by themselves explain why Aero is off.

## Unexplained events

None of these is explained, and each could matter:

- **08-31 10:51:22: 32 services terminated at once** (System 7031 × 32), with no
  dump. This was during a rapid game-launch test pass (a C&C title launched at
  10:50:19, and the box froze at about 10:50:38), followed at 10:57:27 by a
  shutdown that the kernel power manager initiated (event 109).
- **Kernel-Power 41 with PowerButtonTimestamp 0** on **08-09** (boot 06:44) and on
  **08-31** (unexpected shutdown 14:27:55, during HL/Quake II testing). A zero
  stamp means the button was not held. These are a reset button, a power loss or
  something else. Nothing on the box says which.
- **PSU, RAM and thermal are not excluded.** `MSAcpi_ThermalZoneTemperature` is not
  supported on this box, so the GPU temperature is unknown. No memory test has
  been run.
- **What triggered TDR #2** with no game running is unknown. The last known human
  activity was 23:31:18-39 (network link up, `ipconfig`, the agent's console closed
  and relaunched). The fleet's 600-s Starfield screensaver (fleetbook recipe #11)
  would have started at about 23:41:30, **roughly 90-100 s before** TDR #2. That
  is an **untested candidate**: the screensaver's Prefetch entry was overwritten
  on 09-28, so it cannot be confirmed from the box.
- On 08-10 at 17:38 there was a power-button reset with no live dump (cause
  unknown).

## Ruled out

- **BSOD / OS crash.** No bugcheck in any of the 5 Kernel-Power 41 events, no
  minidump, no BugCheck 1001.
- **A power cut on 09-26 15:11 or 09-28 11:13.** PowerButtonTimestamp is set on
  both, so the button was held.
- **Agent auto-update restarts.** 1.81.1→1.84.1 (09-24), 1.84.1→1.85.2 (09-26
  15:40), 1.85.2→1.85.3 (09-26 23:31:57), 1.85.3→1.89.1 (09-28 11:17). Each was a
  clean "log closed (clean shutdown)" hand-over, with the new instance up within
  about 1 s, and none coincides with a freeze.
- **An agent process crash.** No Application Error 1000 or WER report for
  `retro_agent.exe`, ever.
- **Disk.** No Disk 7/11/153 or volmgr errors in the retained System log (back
  to 08-07). The only disk event is one Disk 51 paging warning on the *second* disk
  (`\Device\Harddisk1`), on 08-29.
- **WHEA / machine-check errors.** None. That makes a CPU/chipset fault less
  likely. It does not clear the GPU, which reports through the display driver
  and not through WHEA.
- **An activation lockout.** Win7 is in *Notification* mode (`slmgr /dli`: License
  Status Notification, 0xC004F009 grace expired). That produces the hourly black
  wallpaper, but it never blocks logon on Win7, unlike XP (see CLAUDE.md, "CHECK
  ACTIVATION BEFORE YOU REBOOT A BOX").
- **Other shutdowns.** Orderly user power-offs (1074, "on behalf of
  `admin-PC\admin`"): 09-04 23:17:20, 09-24 23:07:13 and 09-26 16:03:42 (during the
  network re-cabling). None of these is a crash.

## Timeline

Box clock: from the 09-24 mains outage until the agent's clockfix at 09-26
15:40:57 EDT, the RTC read **2013** and ran **6h05m02s ahead** in time of day.
Pre-fix box times are converted with that offset. The 09-24 entries assume the RTC
did not drift between 09-24 and 09-26; the 09-24 agent start (box 04:45:45)
converts to 22:40:43, and the host claimed the box at 22:42:23, which is consistent. After the fix, the
box matches host EDT to within 2 s (12:20:31 vs 12:20:29 on 09-28).

| host EDT | event | source |
|---|---|---|
| 08-09 06:44 | unexpected shutdown, **no** power-button stamp | Kernel-Power 41 |
| 08-10 17:38 | power-button reset, no live dump | 41 PowerButtonTimestamp |
| 08-31 10:50-10:57 | froze during a game-launch pass; 32 services terminated at 10:51:22; kernel-initiated shutdown 10:57:27 | 6008 / 7031 / 109 / DWM 9010 |
| 08-31 14:27:55 | unexpected shutdown, **no** power-button stamp, no dump | 41 / 6008 |
| 09-04 23:17:20 | orderly power-off by admin; box clock still correct | 1074 |
| 09-24 11:52-17:39 | house mains outage; box off | `docs/host-issues-log.md` |
| 09-24 22:38:38 | boot; **box clock now reads 2013** (the RTC fell back about 13 years while the mains were off, which points at the CMOS cell) | 12 / agent.log |
| 09-24 23:07:13 | orderly power-off by admin | 1074 |
| 09-26 15:04:26 | boot | 12 |
| 09-26 15:05:44 | agent's console window closed (console control event 2), 3 s after it started; no agent until 15:40 | agent.log; host "wait conn lost" 15:05:44 |
| 09-26 15:06:11-24 | Yuri's Revenge launched | Prefetch, DWM 9010 |
| **09-26 15:11:27** | **TDR #1** (0x117) | WD-20130925-2116.dmp |
| **09-26 15:11:33** | **power button held** | 41 PowerButtonTimestamp |
| 09-26 15:40:11 | boot; agent 1.84.1→1.85.2; clockfix 2013→2026 at 15:40:57 | 12, agent.log, event 1 |
| 09-26 16:00-16:02 | NIC link flaps during network rework; IP **.246 → .195** | e1cexpress 27 |
| 09-26 16:03:42 | orderly power-off by admin | 1074 |
| 09-26 16:20:01 | boot with **link down** (the agent saw IP 127.0.0.1) | 12, e1cexpress 27 |
| 09-26 23:31:18-39 | link up, `ipconfig`, agent console closed at 23:31:29 and relaunched (1.85.2→1.85.3 at 23:31:57) | e1cexpress 32, Prefetch, agent.log |
| ~09-26 23:41:30 | 600-s screensaver would start (**unverified**) | fleet config, not box evidence |
| **09-26 23:43:06** | **TDR #2** (0x117) | WD-20260926-2343.dmp |
| 09-26 23:46:37 | last line the agent logged in this boot (served `GAMEINDEX HASH`) | agent.log |
| ~09-26 23:47:56 | first request that got no answer (timed out 23:48:26) | `journalctl --user -u retro-gameindex` |
| 09-26 23:52:06 → 09-28 11:07:06 | **142× SPP 8197 0x80080005**; DCOM 10010 at 23:52:06 and 00:07:06 | Application / System log |
| 09-26 23:56:38 | 9898 **refused** from here on | retro-gameindex |
| 09-27 22:52-22:56 | chat-daemon long-polls to **six** agents time out, including this one: a **host-side** event | retro-chat-daemon |
| 09-28 09:30-09:46 | 139/445/NetBIOS answer, 9898 does not | task report |
| **09-28 11:13:54** | **power button held** (last alive 11:13:39) | 41 PowerButtonTimestamp, 6008 |
| 09-28 11:14:56 | boot; agent 1.85.3→1.89.1 at 11:17:18 | LastBootUpTime, agent.log |

## Other facts found on the way

- `wmic os get InstallDate` = **2013-09-26 01:57:29**. Windows was installed while
  the RTC read 2013, so the RTC had fallen back once before (a second sign of the
  CMOS cell), and the activation grace was long gone. Win7 answers that with
  Notification mode, not a lockout.
- The agent's console window ("Retro Remote Agent Version ...") is **visible on the
  desktop**, and a person closed it twice on 09-26 (15:05:44 and 23:31:29). The
  first close left the box with no agent for 35 min.
- Run key: `C:\RETRO_AGENT\retro_agent.exe -l C:\RETRO_AGENT\agent.log`. Also
  Run: `StartCCC` (AMD CCC), `MapShare`, `StarCraftCD` (WinCDEmu batchmnt).
- The TDR registry is at defaults: no `TdrDelay` or `TdrLevel` under
  `GraphicsDrivers`.
- Console account: `admin` (`AutoAdminLogon`=1, `ForceAutoLogon`=1,
  `DefaultDomainName`=ADMIN-PC; read back 2026-09-28).

## Operator options (none applied — each needs a decision and most need reboots)

Use `scripts/fleet/safe-reboot.py 192.168.1.195` for every reboot. Win7
Notification mode does not block logon, so activation is not the reboot risk here.

1. **Put the driver on a supported OS:** install **Windows 7 SP1 (KB976932)**.
   Catalyst 15.7.1 was built for SP1, and SP1 also brings the dxgkrnl updates.
   This is the smallest change that removes the known mismatch.
2. **Or put a supported driver on the OS:** replace Catalyst 15.7.1 with an AMD
   release whose release notes list **Windows 7 without SP1**. Check the notes
   rather than assuming. Clean the old driver first (the `gpu-driver-cleanup`
   skill).
3. **Hardware checks at the box** (these are what would separate "driver" from
   "card"):
   - the HD 5450's heatsink and fan (if one is fitted) for dust and seating, and
     airflow in the case; reseat the card;
   - the **CMOS coin cell**: the RTC has fallen back to 2013 at least twice;
   - an overnight **memtest86+** run;
   - if the freezes continue after SP1 or a driver change, try a different PSU
     or a different card.
4. **Agent hardening (remote-safe, via auto-update):** move the console echo in
   `agent/src/log.c` `raw_out()` outside `g_log_cs`, or make it non-blocking, so
   a stuck console cannot stall every logging thread. Add a source-invariant test.
   Consider hiding or minimising the console on NT so people stop closing it.
   Worth doing whether or not it caused this wedge.
5. **Host-side visibility:** a periodic *fresh-connection* `PING` (which goes
   through the logged accept path) would have flagged this box at about 23:48
   instead of never.

## Diagnosing the next one

Fleetbook recipe **`win7-box-crashes-check-for-a-tdr-before-calling-it-a-bsod`**
(`python3 scripts/retro_fleetbook.py show win7-box-crashes-check-for-a-tdr-before-calling-it-a-bsod`)
has the steps. In short:

1. `DIRLIST C:\Windows\LiveKernelReports\WATCHDOG` and `DIRLIST C:\Windows\Minidump`.
   A `WD-*.dmp` with no minidump means a TDR, not a BSOD. Decode the header:
   `PAGEDUMP` signature, BugCheckCode at `0x28` (0x117), and the four parameters
   at `0x2C` through `0x38`, on the 32-bit header.
2. Export Kernel-Power 41 as XML and read **BugcheckCode** and
   **PowerButtonTimestamp**. The timestamp is a FILETIME: non-zero means someone
   held the button, zero means it was not held.
3. Establish the box-to-host clock offset from **two host-side events** before
   converting any time. Do not rely on one clock line from the box.
4. **Treat P2 of a 0x117 as a driver identifier, not a bug fingerprint.**

## Evidence

The evidence is in `retro-agent/.claude/evidence-icons/192.168.1.195/crash/` in the
main tree. That directory is untracked, so it is **not** in git; the md5s below
identify the files if they are moved.

| file | md5 | what |
|---|---|---|
| `WD-20130925-2116.dmp` | `3c4967c23fe0a8a19d5c22466cf0f652` | TDR #1 live dump (DUMP_HEADER32 `PAGEDUMP`, 15.7600, DumpType 4, current process `System`) |
| `WD-20260926-2343.dmp` | `8e5dd1a1f5f6334144916e4d30384e42` | TDR #2 live dump |
| `k41.xml` | `f5c1e462390755c64426a3a1362a83e7` | every Kernel-Power 41, with BugcheckCode and PowerButtonTimestamp |
| `sys_boot.txt` | `2e7816fef8b2ca28d3026c172c7c9088` | boot / shutdown / 1074 / 6008 / 41 / time-change events (the 21.23 s step-back is here) |
| `sys_err.txt`, `sys_err_rows.txt` | `8f56692dcdebb3244ff4db2ff68bb95c`, `b3825ddd7031f6abcdfc540a13dc513b` | System Level 1-3, back to 08-07 (the 7031 × 32 is here) |
| `app_err.txt`, `app_err_rows.txt` | `39896cb12599fc9c97b2445a5cd68017`, `f4da61b895a96b726dca0579652a3409` | Application Level 1-2 (atiuxpag faults, hl.exe crashes, nothing from retro_agent) |
| `a2.txt` | `e29b361a1b62543d8f913821eeb7fc25` | Application 8197 / 9010 / 1002 / 4105 (the 142-event SPP series, the DWM game launches) |
| `s2.txt` | `837579fabb81a2908755fa5ddf602546` | System event 26 (processor power only; no Application Popup) |
| `w1.txt`, `w3.txt`, `w4.txt`, `wa.txt` | `93f9edc94469de3fa9e60efb5f174ddc`, `73577ba8b23d7f00ac55415389487bb9`, `144b157289a2c9f6962bb5b8518cd795`, `378c5c49b1043224d3ee345de8bbf0d1` | all events in the incident windows |
| `prefetch.json` | `10b2f7ce00a98f08d26318bda6fba0eb` | `C:\Windows\Prefetch` listing (last-run times, UTC) |
| `wer.txt` | `5cf17fa8469c3a656fe73eb3e4f24261` | WER reports (only Windows Update) |
| `dumps.txt` | `bee2795d74a08d783c6d442b68010b70` | no Minidump, no MEMORY.DMP |
| `agent.log`, `agent.log.1` | `ddbee035497d42a75c455ae193a7579a`, `f0f87ad718d1b2f29fabcc5960882159` | the agent's own log, captured before anything touched the box |
| `DIAGNOSIS.md` | `a16602c7219d4cad4945926742465798` | the **first, uncorrected** write-up, kept as it was |

Host-side: `journalctl --user -u retro-gameindex` (09-26 23:46-23:58) and
`journalctl --user -u retro-chat-daemon` (09-26 15:05-15:41 for the offset;
09-27 22:52-22:57 for the six-agent loss).
