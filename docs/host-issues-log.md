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
Ubuntu, kernel 7.0.0-31-generic, 24-core Intel Arrow Lake-S, RTX 5090 (32 GB),
NVIDIA 595.91.07 open kernel module. BIOS F.20. Only disk: SATA Fanxiang
S101Q 4 TB (no NVMe). **No UPS.** The GPU tenants are `ollama` (about 15–21 GB
VRAM; also this repo's fleet AI engine) and `local-image-gen` (SDXL, about
7–10.5 GB; from `reusable-agents`), driven by the reusable-agents timers.

---

## Current state (keep this section current)

| Item | State | Since |
|---|---|---|
| GPU power cap | **450 W** via `nvidia-power-cap.service` (enabled, runs `nvidia-smi -pl 450`; installed by `reusable-agents/install/configure-local-models.sh`, override with `GPU_POWER_LIMIT_W`) | 2026-09-24 23:30 |
| Cap history | 400 W (08-30 → 09-23 23:32), **uncapped 575 W** (09-23 23:32 → 09-24 23:30), 450 W (now) | |
| `kernel.hung_task_panic` | 0 (a GPU drop leaves the box up, just without a GPU) | 2026-09-16 |
| `kernel.panic` / `hardlockup_panic` | 30 / 1 (`/etc/sysctl.d/60-lockup-panic.conf`) | 2026-09-08 |
| kdump | enabled; dumps land in `/var/crash/` | |
| `pcie_aspm=off` in GRUB | present, **no effect** (the link can't use ASPM) | 2026-09-16 |
| PCIe link width | **x8 on an x16 slot**, on every boot since the first one retained (Aug 17). Not the crash cause (see 09-23) | |
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

**2. Hung shutdown after a GPU drop.** Clicking Restart in GNOME with a dead
GPU deadlocks on `nvidia-modeset` (`nvEvoMakeRoom`/`nvkms_yield`, "Error
while waiting for GPU progress" every 5 s, `nvidia-persistenced` stop timeout).
The box sat half shut down for 3 h 38 m (09-21) and 10 h 32 m (09-22).
**Don't use GNOME Restart when the GPU is dead**; use `systemctl reboot -ff`,
SysRq `b`, or the power button.

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
(`hp-bioscfg` exposes nothing).

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
