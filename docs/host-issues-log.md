# Dev host (192.168.1.132) issues log

A running record of everything that has gone wrong with the **dev host's
hardware/OS itself**: crashes, hangs, power loss, GPU faults, and the
mitigations applied. This is the box that runs the chat daemon, brain, PXE,
game servers, dashboard, and ollama (see "Host services" in `CLAUDE.md`), so a
host fault takes all of them down at once. Not retro-box or agent bugs. Read this before diagnosing a "the
box rebooted" or "the GPU is gone" report, and **append to it whenever a new
host event happens or a mitigation changes** (see "Updating this log" at the
bottom).

Host: `voidsstr-OMEN-by-HP-45L-Gaming-Desktop-GT22-3xxx`, 192.168.1.132.
Ubuntu, kernel 7.0.0-34-generic (since the 09-26 15:58 boot; 7.0.0-31 before), 24-core Intel Arrow Lake-S, RTX 5090 (32 GB),
NVIDIA 595.91.07 open kernel module. BIOS F.20. Only disk: SATA Fanxiang
S101Q 4 TB (no NVMe). **No UPS.** The GPU tenants are `ollama` (about 15–21 GB
VRAM; also this repo's fleet AI engine) and `local-image-gen` (SDXL, about
7–10.5 GB; from `reusable-agents`), driven by the reusable-agents timers.

---

## Current state (keep this section current)

| Item | State | Since |
|---|---|---|
| GPU power cap | **400 W** via `nvidia-power-cap.service` (enabled, runs `nvidia-smi -pl 400`; originally from `reusable-agents/install/configure-local-models.sh`, override with `GPU_POWER_LIMIT_W`). The unit was rewritten 2026-09-26 23:22:44 by a `sudo sed` run from `~/development/reusable-agents` (commit `2637516`) and applied at 23:22:45 (`set to 400.00 W from 575.00 W`). It has been re-applied at every boot since; `nvidia-smi` reads 400 W on 09-28 12:18 | 2026-09-26 23:22 |
| Cap history | 400 W (08-30 → 09-23 23:32), **uncapped 575 W** (09-23 23:32 → 09-24 23:30), 450 W (09-24 23:30 → 09-26 23:22; an instant power-off happened under it on 09-26 20:14), 400 W (now; under it: Xid 79 on 09-28 10:59, MCE panics on 09-28 15:48 and 09-29 06:13 (the second under the clock lock), each followed by a wedged-card boot) | |
| GPU clock lock | **210–2400 MHz** graphics (`nvidia-smi -lgc 210,2400`; stock boost reaches 3090 MHz). Added to `nvidia-power-cap.service` (ExecStart; `-rgc` on stop) and to `reusable-agents/install/configure-local-models.sh` (`GPU_CLOCK_LOCK_MHZ`, empty disables it). Applied live 09-28 23:3x after the 15:48 MCE panic at the 400 W floor; under load the clock peaks at 2385 MHz | 2026-09-28 |
| `kernel.hung_task_panic` | 0 (a GPU drop leaves the box up, just without a GPU) | 2026-09-16 |
| `kernel.panic` / `hardlockup_panic` | 30 / 1 (`/etc/sysctl.d/60-lockup-panic.conf`) | 2026-09-08 |
| kdump | enabled; dumps land in `/var/crash/` | |
| `pcie_aspm=off` in GRUB | present, **no effect** (the link can't use ASPM) | 2026-09-16 |
| PCIe link width | **x16** since the 09-24 17:39 boot (`392ee1f9`, the first boot after the 09-24 mains loss). The kernel's `limited by 32.0 GT/s PCIe x8 link at 0000:00:06.0` line is on every retained boot through 09-23 18:30 (`504edd27`) and on none from 09-24 17:39 on, and sysfs `current_link_width` reads 16 on 09-28. The 09-24 19:18 MCE, both power-offs and the 09-28 Xid 79 all happened at x16, which confirms x8 was not the crash cause (see 09-23) | 2026-09-24 17:39 |
| Host IP | **192.168.1.132** is back as the PRIMARY LAN address (static, added to NM "Profile 1" 2026-09-28 23:55; LAN traffic is sourced from it, `ip route get 192.168.1.123` -> src .132). DHCP still hands out **.196** (gateway .254), kept as a secondary address for the default route. A DHCP reservation for .132 on the router would remove the (small) risk of the router leasing .132 to another device | 2026-09-28 23:55 |
| Open physical items | 12V-2x6 connector at both ends; separate PSU cables vs daisy-chain; PSU wattage; a UPS/meter with logging | |

---

## Failure signatures: what each one looks like and how to check

**1. GPU falls off the bus (Xid 79).** The most common failure.
`NVRM: Xid (PCI:0000:01:00): 79, GPU has fallen off the bus`, often with a
`pcieport 0000:00:06.0: PCIe Bus Error ... Physical Layer` in the same second,
and sometimes Xid 154 (`Node Reboot Required`). The box **stays up** on the
Intel iGPU (gnome-shell comes back on i915). ollama errors with
`CUDA error: unspecified launch failure`, or silently falls back to CPU
(2–4 tok/s against about 150–285 on the GPU).
- `lspci -vv -s 01:00.0`: BARs showing `[virtual]`/`[disabled]` means the card is gone.
- **Only a COLD power cycle revives the card.** Warm reboots don't.
- `local-image-gen` `/healthz` still reports ok from cached values. **It is
  wrong**; prove the GPU with a real `POST /generate`.

**2. Hung shutdown after a GPU drop.** Any ordinary reboot with a dead GPU
deadlocks in `nvidia-modeset`. That includes GNOME Restart (09-21, 09-22) and
**`sudo reboot` (09-28)**. The 09-21/09-22 form showed `nvEvoMakeRoom`/`nvkms_yield`
and "Error while waiting for GPU progress" every 5 s. The 09-28 form logged no
such line. There, `systemd-logind` spun in `EvoCheckNotifier` (under
`ApplyProposedModeSetStateOneApiHeadShutDown`, reached from `fbcon_blank` →
the `drm_fb_helper` restore → `nv_drm_atomic_commit`) while holding
`console_lock`. `plymouth-reboot` and every `tty_open` then blocked behind it,
and systemd's timeouts and SIGKILLs could not end it. Both forms end with
`nvidia-persistenced` stop timeouts. The box sat half shut down for 3 h 38 m
(09-21) and 10 h 32 m (09-22). **Don't do an ordinary reboot when the GPU is
dead.** A warm reboot doesn't revive the card anyway (signature 1), so use the
power button. Remotely, use SysRq `b` (`echo b | sudo tee /proc/sysrq-trigger`).
`systemctl reboot -ff` skips logind and plymouth but still runs the drivers'
shutdown hooks; it is untested here with a dead GPU.

**3. MCE panic.** `mce: CPUs not responding to MCE broadcast ... Kernel
panic - not syncing: Timeout: Not all CPUs entered broadcast exception
handler`. kdump captures it, and the next boot shows `BERT: [Hardware Error]: 1
record`. Seen 08-30 and 09-24 19:18 (the second one uncapped).

**4. Instant power-off.** The journal simply stops mid-line: no panic, no kdump,
no BERT record, no rasdaemon row. Either mains power or the PSU's protection
tripping. Tell them apart by whether other LAN devices lost power and whether the
internet dropped first (a mains outage took the ISP down first on 09-24 11:52).

**5. Desktop crash that looks like a reboot.** gnome-shell/Xwayland core
dump (e.g. after a driver/library mismatch or a GPU drop). **Check `uptime` /
`journalctl --list-boots` before believing the kernel rebooted.** The 20 s and
17 s "boots" around a panic are the kdump capture kernel, not extra crashes.

**6. Card wedged after a warm reboot (IOMMU fault storm).** From the first
second of a boot, the kernel logs `DMAR: [DMA Write|Read NO_PASID] Request device
[01:00.0] ... fault reason 0x71` plus thousands of `dmar_fault: N callbacks
suppressed` every 5 s, and nothing on top of the kernel runs (the journal holds
only those lines, agents never fire). Seen 09-28 15:48 → 22:52 after the MCE
panic's automatic warm reboot. **A cold power cycle clears it** (09-28 22:52);
a warm reboot evidently does not. Count it:
`journalctl -b 0 -k | awk '/DMA (Read|Write).*Request device/{n++} /dmar_fault: [0-9]+ callbacks suppressed/{for(i=1;i<=NF;i++) if($i=="dmar_fault:") n+=$(i+1)} END{print n+0}'`
— non-zero means the card needs a cold cycle before the fleet will run.

### Triage commands

```bash
journalctl --list-boots --no-pager | tail            # real reboots vs desktop crashes
journalctl -b -1 --no-pager -o short-precise | tail  # how the last boot ended
journalctl -b -1 -k | grep -E 'Xid [0-9]|fallen off|PCIe Bus Error|mce|panic'
#   careful: plain `grep Xid` also matches the r8169 NIC line "XID 641"
journalctl -b 0 -k | grep -iE 'BERT|Hardware Error'  # firmware-logged error from the previous boot
ls -lat /var/crash/                                  # kdump output
sudo sqlite3 /var/lib/rasdaemon/ras-mc_event.db 'select * from aer_event order by id desc limit 5'
lspci -vv -s 01:00.0 | grep -E 'Region|LnkSta'       # card present? link width?
nvidia-smi --query-gpu=power.limit,power.draw,temperature.gpu --format=csv
systemctl is-active nvidia-power-cap.service ollama
```

Forensic marker: a Claude session's `.jsonl` mtime pins the moment the
desktop died, because the sessions stop writing when it does.

**Reading a kdump:** apport base64 fields are one independent block per line.
Decode each line, concatenate the bytes, then gunzip. Joining the lines first fails.

---

## Incident log (newest first)

### 2026-09-29 16:45:53: requested reboot (clean, not a fault)

- **Boot IDs:** `d08f865b…` (13:18:26 → 16:47:01) → `d3bc0e94…` from 16:47:20.
- **What:** `systemd-logind: The system will reboot now!`, then a normal `reboot.target` and
  `systemd-shutdown`. This was a user or UI reboot request, not a crash. There was no kdump, BERT or
  MCE record, and 0 IOMMU faults on the new boot. The GPU run of 3 h 27 min before it had no faults.

### 2026-09-29 13:16:55: power loss from a tripped building breaker (not a host fault)

- **Cause (operator, same day):** a breaker blew. The breaker is fixed. This was external power loss,
  so it is NOT part of the MCE/GPU crash pattern and should not count toward it. The unplanned-restart
  count for the GPU problem stays at 3 (09-28 15:48, 09-29 06:13, 09-29 11:26).
- **What it looks like, for next time:** the journal just stops, with no kdump, pstore, BERT or MCE
  record. A power loss can leave this same absence of evidence, so ask about power first.

- **Boot IDs:** `843e7fbc…` (11:49:09 → last journal line 13:16:55) → `d08f865b…` from 13:18:26 (~90 s
  gap, 0 IOMMU faults, card healthy, cap and clock lock re-applied at 13:18:38).
- **Fault:** none recorded. There was no kdump (newest `/var/crash` is still `202609291127`), pstore
  was empty, there was no BERT/MCE line on the next boot, and there were no new rasdaemon AER rows.
  The journal ends mid-stream on ollama generating at ~118 tok/s on the GPU, with the hlds server
  running. The kernel didn't get to log anything, so this looks like a hard reset or power loss, not a
  panic. It could also be someone pressing reset: a USB keyboard was re-plugged at 13:09:50 and a GNOME
  session was active from 12:09.
- **Recovery:** a power loss is effectively a cold cycle, so the card came back unwedged.
  Fleet, sites and image-gen were back without intervention.

### 2026-09-29 11:26:35: third MCE panic in 20 h, 66 min into a cold boot (signature 3)

- **Boot IDs:** `8cd5be88…` (10:20:51 cold boot → 11:26:35, uptime 3976 s) → kdump capture boot
  `98bbcf98…` (11:27:17 → 11:28:15, `saved vmcore in /var/crash/202609291127`, `Rebooting.`) →
  `8798186c…` (11:37:13 → 11:48:46, 0 IOMMU faults, ended in an orderly `systemd-shutdown`) → `843e7fbc…` from
  11:49:09 (0 faults).
- **Fault:** `mce: CPUs not responding to MCE broadcast (may include false positives): 0` → `Kernel panic -
  not syncing: Timeout: Not all CPUs entered broadcast exception handler`. The journal ends on routine
  hlds (CS) server lines. The GPU was under normal fleet load.
- **Mitigations in effect:** 400 W cap and the 2400 MHz clock lock. MCE panics now at 09-28 15:48, 09-29
  06:13 and 09-29 11:26, so the interval is shrinking (14 h, then 5 h), and this one came barely an hour
  after a cold start. Power and clock limits are not controlling this. The open physical items
  (12V-2x6 connector, PSU cabling and wattage, BIOS) are now the leading suspects.

### 2026-09-29 06:13:55: MCE panic under the 2400 MHz clock lock, then another wedged-card boot until ~10:07 (signatures 3 + 6)

- **Boot IDs:** `ed800384…` (09-28 22:53:12 → 09-29 06:13:55, 7 h 21 m) ended in a kdump-captured panic,
  `/var/crash/202609290614`: `mce: CPUs not responding to MCE broadcast (may include false positives):
  0,2,5,7` → `Kernel panic - not syncing: Timeout: Not all CPUs entered broadcast exception handler`. The
  journal ends mid-generation (ollama `n_gen = 434, tg = 143.50 t/s` at 06:13:55).
- **Mitigations in effect:** 400 W cap **and** the new graphics clock lock (210–2400 MHz, applied 09-28 ~23:30).
  **The clock lock did not prevent it.** This is the second MCE panic in about 14 h.
- **Wedged-card boot again:** the kdump warm reboot started `0643ac04…` at 06:14:20 and the IOMMU rejected
  GPU DMA from its first second (**904,771** faults). The boot logged only 357 non-kernel journal lines in
  3 h 53 m, so the fleet was dark again. The journal stops at 10:07:23. The next boot, `8cd5be88…`, began
  10:20:51 after a 13-minute gap (a power cycle, likely manual) and is clean (0 faults).
- **Takeaway:** as on 09-28, `kernel.panic=30` plus kdump turns every MCE panic into hours of outage,
  because the warm reboot leaves the card wedged until someone power-cycles it.

### 2026-09-28 23:55: 192.168.1.132 restored as the host's primary LAN address (no reboot)

- **Found:** after the 22:53 boot the host again held only DHCP **192.168.1.196**;
  `retro-gameindex`'s status.json reported `our own servers did not answer`
  for every fleet server (all 20 probed at .132) and `host-duties.py` read
  1/28, while `ss -ulpn` showed every server bound to 0.0.0.0 and answering on
  127.0.0.1. `ip neigh` showed .132 `INCOMPLETE` and TCP to .132 returned
  `No route to host`, i.e. nothing else on the LAN held .132.
- **Change:** `nmcli con mod "Profile 1" +ipv4.addresses 192.168.1.132/24`
  then `nmcli dev reapply enp129s0` (the desktop user has
  `settings.modify.system`, no sudo needed). The static address came up as the
  kernel PRIMARY, so the 192.168.1.0/24 prefix route is sourced from .132 and
  UDP game servers bound to 0.0.0.0 reply from the address clients sent to.
  DHCP .196 stays as a secondary and keeps the default route. It is in the NM
  profile, so it survives a reboot.
- **Verified:** `host-duties.py --quiet` 27/28 game servers answering at .132
  (Tribes 2 remains mute, as before); NAS .122:445 reachable.
- **Revert:** `nmcli con mod "Profile 1" -ipv4.addresses 192.168.1.132/24 && nmcli dev reapply enp129s0`.
- **Still open:** the loopback drop-ins from the 00:36 entry
  (`retro-gameservers-watch` `20-probe-loopback.conf`, the a2s relays'
  `20-target-loopback.conf`) remain; loopback stays correct for both, so they
  were left in place. A DHCP reservation for .132 on the router is still the
  cleaner long-term answer.

### 2026-09-28 15:48:25 → 22:52:36: after the panic's warm reboot the GPU faulted for 7 h and the fleet never ran (signature 6)

- **Boot IDs:** `a9388ad8…` (15:48:24 → 22:52:36, 7 h 04 m). `ed800384…` began
  22:53:12, 36 s later.
- **Fault:** from **15:48:25, one second into the boot**, the IOMMU rejected DMA
  from the GPU nonstop: `DMAR: [DMA Write NO_PASID] Request device [01:00.0] fault
  addr 0xf7eff000 [fault reason 0x71] SM: Present bit in first-level paging entry
  is clear` (reads too), about 3,400 `dmar_fault: N callbacks suppressed` every
  5 s. **18,393,353 faults** in total until the journal ends.
- **Fleet impact:** this boot's journal holds only those kernel lines (5,760 an
  hour) and **no userland entries at all**, and the reusable-agents run history
  has **zero agent runs from 15:48 to 22:52** (e.g. the 10-minute
  `aisleprompt-recipe-image-verifier` never ran). The kernel was up but nothing
  on top of it ran for 7 h: no agents, no articles, no price refresh. Where boot
  stalled is unproven (the journal has nothing to show it).
- **End:** the journal stops at 22:52:36 (next boot: `system.journal corrupted
  or uncleanly shut down`). There is no kdump, no BERT record and no panic line,
  and the host was back 36 s later, so it was an external reset or power cycle
  (who or what is unknown).
- **After:** the `ed800384…` boot has **0** DMAR faults, `lspci` shows Region 0
  mapped and `LnkSta: Speed 32GT/s, Width x16`, and ollama serves on the GPU. So
  whatever reset the box at 22:52 also revived the card, while the kdump warm
  reboot at 15:48 did not.
- **Takeaway (likely, consistent with signature 1):** the panic path
  (`kernel.panic=30` → kdump → warm reboot) can bring the box back with the card
  still wedged, and then nothing runs until someone cold-cycles it. The
  reusable-agents KTLO sweep now reports `gpu_iommu_faults_this_boot` and
  `boots_24h` so a faulting card is caught on the next tick.

### 2026-09-28 15:48:03: MCE panic at 400 W and x16 (signature 3)

- **Boot IDs:** `520bf9f1…` (11:09:30 → 15:48:03, 4 h 38 m).
- **Fault:** kdump `/var/crash/202609281548/dmesg.202609281548`, at uptime
  16726 s: `mce: CPUs not responding to MCE broadcast (may include false
  positives): 0` then `Kernel panic - not syncing: Timeout: Not all CPUs entered
  broadcast exception handler`. Nothing hardware-related precedes it in that
  dmesg (no bank decode, no Xid, no AER), and the next boot has **no BERT
  record**, unlike the pattern described in signature 3.
- **Load:** ollama was mid-generation (last journal line 15:47:59, `n_gen = 1127,
  tg = 124.55 t/s`) and three agent units were starting at 15:48:03. Opus
  authoring had just resumed on the claude-pool (implementer runs at 15:05 and
  15:26).
- **Cap in effect: 400 W** (the card's minimum), PCIe x16, kernel 7.0.0-34,
  driver 595.91.07. This is the third MCE panic with kdump evidence (08-30,
  09-24 19:18, 09-28 15:48) and the second at 400 W.
- **Response:** `kernel.panic=30` rebooted it automatically at 15:48:24, and the
  card came back faulting (see the entry above).

### 2026-09-28 10:59:35: Xid 79 at 400 W and x16, then `sudo reboot` hung for 4 min (signatures 1+2)

- **Boot IDs:** `4b2b821c…` (up since 09-26 23:16:18, 35 h 43 m) ends at
  11:07:30 mid-shutdown. `520bf9f1…` began 11:09:30.
- **Fault:** `10:59:35.680206 NVRM: Xid (PCI:0000:01:00): 79, pid=6540, name=KMS
  thread, GPU has fallen off the bus`, then `Xid ... 154, GPU recovery action
  changed from 0x0 (None) to 0x2 (Node Reboot Required)` 1.4 ms later. There is
  no `PCIe Bus Error`/AER line, no MCE and no kdump, and the next boot has no BERT
  record. The only new `/var/crash` file is `_usr_bin_Xwayland.1000.crash` from 10:59.
- **Knock-on:** `GNOME Shell crashed with signal 6` at 10:59:36. The user session
  closed at 10:59:59, which is when every Claude session's `.jsonl` stopped
  writing. `gnome-remote-desktop` core-dumped. ollama hit `CUDA error:
  unspecified launch failure` → `ggml_abort`, and the log shows `uvm encountered
  global fatal error 0x60, requiring os reboot`. The greeter came back without the GPU
  (`nvidia-modeset: ERROR: GPU:0: Failed detecting connected display devices`).
- **Load:** ollama had just started a 1729-token prompt: it created a context
  checkpoint at 10:59:35.445, **0.24 s before the Xid**, and the failing call
  was `cudaStreamSynchronize`. So this is another drop at the start of a
  generation burst, under routine agent traffic.
- **Cap in effect: 400 W**, applied at 09-26 23:22:45 in this boot (see Current
  state). The PCIe link was x16. The kernel was 7.0.0-34. This is the first Xid 79
  since 09-23 13:16. Xid 79s have now happened at 400 W on both x8 (09-21, 09-22,
  09-23) and x16, so neither the cap nor the link width stops them.
- **Shutdown:** user `remote` (uid 1001, not the `voidsstr` account the Claude
  sessions use) logged in through GDM at 11:00:26, while Chrome Remote Desktop was
  connected. At 11:03:27 it ran `sudo /usr/sbin/reboot` from `/dev/pts/1`.
  `systemd-logind` logged `Removed session c2` (the greeter on the dead GPU) at
  11:03:27.86 and nothing after that. Its watchdog fired at 11:06:00, SIGABRT did
  nothing, and SIGKILL at 11:07:30 is the last line of the journal. The hung-task
  report at 11:06:43 shows why:
  - logind was **running** and holding `console_lock`, in `vt_k_ioctl` →
    `do_unblank_screen` → `fbcon_blank` → the `drm_fb_helper` restore →
    `nv_drm_atomic_commit` → `nvSetDispModeEvo` →
    `ApplyProposedModeSetStateOneApiHeadShutDown` → `EvoCheckNotifier`. That is a
    modeset polling for a notifier from a GPU that is gone.
  - `plymouthd` (from `plymouth-reboot`, which timed out at 11:04:57) was blocked
    on `console_lock`. So was every stop helper that reached `tty_open`, which is
    why `nvidia-persistenced`, `tailscaled`, `user-runtime-dir@*` and
    `systemd-user-sessions` all timed out.
  - gnome-shell's DRM closes queued behind the modeset lock too.

  Most likely, logind was switching the greeter's VT back to text mode. That part is
  inferred from the stack; the deadlock chain itself is in the log.
- **Recovery:** the next boot started 2 min after the journal ended, with the card
  healthy (BAR 0 assigned, x16, no Xid, `nvidia-smi` answers). A warm reboot
  doesn't revive a dropped card (signature 1), so this was most likely a **cold**
  power cycle by hand. That is unproven.
- **After the boot (12:18):** the cap is 400 W, the GPU is idle at x16, and
  there is no Xid. All host units are `active`. 24/25 game servers answer on
  127.0.0.1. CS:Source is still `activating`; it was crash-looping (`status=100`,
  restart counter 7233) before the drop, so that is not a host fault.
  `host-duties.py --quiet` reports "game servers 1/28 responding" only because it
  probes .132 (see 09-28 00:36).
- **Response:** logged here. Signature 2 now covers `sudo reboot` and the
  logind/`console_lock` form, and the Current state table has the link width
  (x16 since 09-24), host IP and kernel.

### 2026-09-28 00:36: still on 192.168.1.196 - the game-server watchdog was restarting healthy servers ~88x/hour

- **Boot ID:** unchanged since 2026-09-26 23:16:17 (`uptime -s`); DHCP lease
  renewed 2026-09-27 23:16:36 as **192.168.1.196** (gateway .254). Nothing on
  the LAN answers ARP for .132 now.
- **Signature:** `gameservers.py` (probing `RETRO_GAMESERVER_HOST`, default
  .132) reported 1/25 up and `host-duties.py` 1/28 responding, while every
  server answered on 127.0.0.1 and `ss -ulpn` showed them bound to 0.0.0.0.
  `retro-gameservers-watch` read that as "active but mute" and restarted
  servers: **528 restarts in the 6 h to 00:36** - each one drops every
  connected player.
- **Also affected, not changed:** every staged "Join ... fleet server"
  launcher, the favourites DB and the in-game server lists point at .132, so
  from the fleet the servers are unreachable at the address they expect. The
  2026-09-26 entry records the operator creating the NM profile by hand; the
  host's own address is the operator's decision (fix options: a static
  192.168.1.132 as the profile's first address with gateway .254, or a DHCP
  reservation for .132 on the new router).
- **Response:** drop-in
  `~/.config/systemd/user/retro-gameservers-watch.service.d/20-probe-loopback.conf`
  sets `RETRO_GAMESERVER_HOST=127.0.0.1` for the watchdog, restarted 00:36:53:
  24/25 answer on loopback (Tribes 2 mute, CS:Source still activating). No
  network settings were changed. Remove the drop-in once the host is back on
  .132 if the LAN-side probe is wanted again.
- **Also found and fixed (01:15):** the three GoldSrc A2S relays
  (`a2s-proxy-cs16` 27015, `a2s-proxy-cs16-public` 27016, `a2s-proxy-hldm`
  27020, from retro-agent-private's `install-a2s-proxy.sh`) had
  `--target 192.168.1.132:<hlds port>` baked into their units, so every CS 1.6 /
  HLDM join through the advertised port dead-ended. Drop-ins
  `20-target-loopback.conf` repoint them at `127.0.0.1` (relay and hlds share the
  host - correct whatever the LAN address). Verified: an A2S_INFO to
  `192.168.1.196:27015` answers "NSC Retro Fleet Arena (CS 1.6)".

### 2026-09-26 20:14:45: instant power-off at 450 W (signature 4)

- **Boot IDs:** `e335b889…` (began 15:58:34) stops at 20:14:45 in the middle of
  ollama prompt-cache log lines: no shutdown, no panic, no kdump output (the
  newest `/var/crash` file is the 09-24 19:18 dump), no Xid/AER/MCE/thermal
  line in the kernel log, and no BERT record in the next boot's kernel log.
  `4b2b821c…` began 23:16:18, so the box was off about **3 h 1 min** until
  someone pressed power.
- **Cap in effect: 450 W.** `nvidia-power-cap.service` logged "set to 450.00 W"
  at 15:58:46 in the dying boot. So 450 W did not prevent signature 4 (the
  09-24 23:11 one was at 575 W).
- **Load:** ollama was actively serving a generation (slot processing a new
  prompt) at the cutoff. The retro-agent vcr-kmd sessions were doing offline
  work and 86Box was not running. No Claude session touched host power, BIOS
  or the GPU.
- **Afterwards:** at 23:22:44 the unit file was rewritten to `-pl 400` and
  re-run ("set to 400.00 W from 575.00 W"). That was not done from the
  retro-agent vcr-kmd session. By 2026-09-27 00:10 the host was still on
  192.168.1.196 but the NAS (.122:445) and `.124` answered again, so the fleet
  LAN problem from the 15:58 entry is gone.
- **Cause:** a PSU over-power/over-current trip from GPU power spikes is still
  the most likely explanation; **unproven**, since there is no PSU or UPS
  telemetry. Whether 400 W is enough is the open question. If the box powers
  off again at 400 W, the physical items in "Current state" (12V-2x6
  connector, separate PSU cables, PSU wattage, a logging UPS) come next.
- **Response:** recorded here, and the current-state table updated to 400 W.

### 2026-09-26 15:58:13: orderly reboot, then the host moved to 192.168.1.196 and lost the fleet LAN

- **Boot IDs:** `68aeaf02…` ended 15:58:13 with a normal `systemd-reboot.service`
  shutdown (journal closes cleanly, no Xid, no MCE, no BERT). `e335b889…` began
  15:58:34. Operator-initiated, likely; nothing in the logs says who.
- **After the boot the network changed.** The `netplan-enp129s0` connection that
  had held a DHCP lease on **192.168.1.132** since 08-17 failed at 15:59:34
  (`ip-config-unavailable`: no DHCP answer). At 16:10:16 a new NetworkManager
  connection, "Profile 1", was added by hand and activated, and at 16:18:49 it
  took **192.168.1.196** from gateway **192.168.1.254**. The netplan YAML is gone.
- **Effect:** from .196 the NAS (.122), every retro box and .1 get no ARP answer,
  and the CIFS mount `/mnt/retro-share` fails (`-113`, host unreachable). Only
  .139 and .249 (whitebeast) answer. Everything that assumes .132 is now wrong:
  `retro-pxe` (`server_ip` pinned to .132), the game-server address the fleet's
  favourites point at, and the chat daemon's reach.
- **Load at the time:** idle GPU; a retro-agent session was reading event logs
  off `.246`.
- **Response:** recorded here, and the operator was asked whether the network
  move was intended. No network settings changed from this session.

### 2026-09-24 23:11:26: instant power-off at 575 W (signature 4)
Boot `1fa9e5a6` ended mid-ollama generation (149 tok/s, 15.3 GB model on the
card, `local-image-gen` also resident). No panic, kdump, BERT record,
rasdaemon row, Xid/AER, or thermal messages. LAN and DNS were fine up to the cutoff; whitebeast
and the router were up afterward. The box was off 11 m 44 s until someone pressed power
(boot `68aeaf02`). No Claude session touched host power, BIOS, or GPU. The load
was the same routine pattern as the previous 2.5 h. **Most likely a PSU
over-power/over-current trip from GPU power spikes while uncapped; unproven**
(no PSU/UPS telemetry). **Response:** cap set to 450 W at 23:30:43 (reusable-agents commit
`ac25f82`, `install/configure-local-models.sh`).

### 2026-09-24 ~19:18: MCE panic at 575 W (signature 3)
99 min into boot `392ee1f9`, ollama mid-request, cap disabled. The first MCE since
the cap was added on 08-30. kdump saved `/var/crash/202609241918`, but the
capture kernel's reboot hung and the box sat dead until 20:38:30. The next boot
logged a BERT record. No Xid/AER. A mains flicker can't be ruled out.

### 2026-09-24 11:52:16: mains power loss
Boot `504edd27` ended cleanly with no panic. The internet had been down since about
11:27 with the LAN still up (ISP/upstream failed first). Off 5 h 47 m.

### 2026-09-23 23:32: power cap removed
`nvidia-power-cap.service` stopped and disabled (not by an agent), so the limit went
400 → 575 W and the card drew 554–574 W. Earlier that boot (after the reseat),
it had run 5 h 22 m clean with three tenants co-resident at 98 % VRAM.

### 2026-09-23 18:30: card reseated, still x8
The user pulled and reseated the card. Healthy afterward, but still `Width x8`.
Timeline check: x8 appears on the **first retained boot (Aug 17)**, 8 days
before GPU load began (Aug 25) and 11 days before the first crash (Aug 28). So x8
has been there from day one and **is not the crash cause**. It does cost about half the host bandwidth
(22.3 GB/s measured). The BIOS PCIe settings can't be reached from Linux
(`hp-bioscfg` exposes nothing). *Later:* the link has trained at x16 since the
09-24 17:39 boot, after the mains power loss, and the crashes continued there
(see Current state).

### 2026-09-23 13:16:48: Xid 79 after about 3 h 20 m of agent load (signature 1)
ollama had been restarted 10:03. The drop came with **no precursor**: 0 AER
errors in the previous 75 min, and RxErr latched the same second as the Xid. So AER
polling detects a drop but can't predict one. The desktop crashed onto i915 (that looked like a
reboot, but the kernel stayed up). The GPU stayed dead about 5 h.

### 2026-09-23 morning: stress test passed
A 9-minute load up to 400 W plus about 11 min of ollama model swaps: 0 errors. **It
dropped 3 h later anyway.** Short clean runs are weak evidence.

### 2026-09-22 12:37: Xid 79, then a 10.5 h hung shutdown (signatures 1+2)
Killed two live Claude sessions. A 44 s power cycle at 23:09 cold-revived the
card (no reseat).

### 2026-09-21: Xid 79, 3 min 18 s into a boot
The previous boot had lost its GPU 09-16 16:19 and stayed usable for 4.7 days
without it. A GNOME Restart at 09:16 hung for 3 h 38 m. After the power cycle
(12:55), the card dropped again at 12:59:15 on one chat request. That was the
shortest time-to-failure seen.

### 2026-09-15 / 09-16: diagnosed with kdump
The 09-08 panic sysctls produced 5 vmcores (09-10 ×2, 09-12, 09-15 ×2). All
were `hung_task: blocked tasks` behind `nvidia-modeset`, triggered by a Physical-Layer
RxErr followed by an Xid 79 on root port 00:06.0. The drops cluster inside the
:00/:15/:30 agent-timer fan-out (16 timers, 5 GPU consumers).
`hung_task_panic` was set back to 0 after recovery, so a drop now leaves the
box up instead of panicking it.

### 2026-09-09 20:38: driver/library mismatch took down the desktop (signature 5)
The NVIDIA 595.84 → 595.91.07 upgrade was applied 09-08 **without a reboot**, and
`NVRM: API mismatch` fired every 2 s for 30 h until gnome-shell, Xwayland, and
remote desktop all core-dumped. **Upgrade the NVIDIA stack only in a
reboot window.** Also note that unattended-upgrades skips `-updates`, where the
NVIDIA packages land, so check `apt list --upgradable | grep nvidia` by hand.

### 2026-09-08: mitigations
Added the panic sysctls (so hangs now dump and reboot) and upgraded to 595.91.07.

### 2026-08-28, 08-30, 09-07: first hard hangs
All happened under heavy overnight ollama + local-image-gen load. 08-30 was captured by
kdump as the MCE broadcast panic, which led to the 400 W cap
(`nvidia-power-cap.service`, "MCE crash mitigation"). 09-07 left no logs; the
GPU wedged about 14 s before the rest of the box.

---

## Updating this log

**Any session that sees a host crash, hang, reboot, GPU fault, or power event,
or that changes a mitigation (power cap, sysctls, driver, GRUB, BIOS, hardware
work), must update this file in the same session:**

1. Add an incident entry at the top of "Incident log": the date/time, the
   boot ID from `journalctl --list-boots`, which signature it matches, the
   evidence (quote the key log line), the load at the time, the cap in effect,
   and the response.
2. Update the "Current state" table if anything changed.
3. Keep it factual. Label causes "likely" or "unproven" unless the logs prove
   them, and correct older entries when new evidence contradicts them (as the
   09-23 x8 entry does).
4. Commit it together with the related fix, or on its own.
