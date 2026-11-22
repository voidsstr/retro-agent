# stallrec - the instrument for "the box hitches every few seconds"

`typeperf` samples once a second and **averages**, so the 100-300 ms hitch a
player actually feels is invisible to it. On `.123` (2026-11-21) a 5-minute
`typeperf` trace of Counter-Strike 1.6 showed `hl.exe` holding 92-100% CPU with
no starvation and no DPC storm, while the player was feeling a hard input stall
every 5-30 seconds. **The instrument was wrong, not the complaint** - and the
first session spent its whole budget on it.

`stallrec.exe` measures the thing itself.

## What a "hit" means

One thread at `THREAD_PRIORITY_TIME_CRITICAL` inside a `HIGH_PRIORITY_CLASS`
process loops `Sleep(1)` and times the gap with `QueryPerformanceCounter`. At
that priority a game at normal priority **cannot** keep it off the CPU. So a
recorded gap is not "the box is busy" - it is the kernel itself refusing to run
anything: a long ISR or DPC, a paging stall, an SMI, or a clock/P-state
transition.

Each hit is bracketed with `GetSystemTimes`, which splits the lost time:

| the line says | read it as |
|---|---|
| `KERNEL (driver)` | a driver ate the core - video, audio, NIC, storage |
| `user code` | some other process actually ran |
| `NOTHING RAN (throttle/SMI/bus)` | idle dominated: clock throttling, an SMI, or a bus stall - no code ran at all |

That third line is the one worth the build: it separates "something is running
that should not be" from "the hardware stopped the CPU", which have completely
different fixes.

## Build and use

```bash
i686-w64-mingw32-gcc -O2 -Wall -s -o stallrec.exe stallrec.c   # no SSE assumed
python3 run_stallrec.py <ip> --seconds 150 --threshold 20      # upload, run, fetch
```

By hand through the agent:

```
UPLOAD C:\RETRO_AGENT\stallrec.exe
EXEC cmd /c start /b "" C:\RETRO_AGENT\stallrec.exe 150 20 C:\stalls.txt
... wait ...
DOWNLOAD C:\stalls.txt
```

**Start it with `start /b`, not `LAUNCH`.** `LAUNCH` gives the child a visible
console, which on a fullscreen game steals the focus and can minimise it - and
then you are measuring a different thing than the player experiences. `EXECW`
also works but blocks the connection for the whole run.

**Measure idle first.** A quiesced box must read 0 stalls at idle; if it does
not, stop and fix that before launching any game, because a stall present at
idle is not the game's.

## Measured

| box | state | result |
|---|---|---|
| `.123` | idle, after the 2026-11-21 service quiesce | **0 stalls / 90 s** |
| `.123` | Counter-Strike 1.6 up, 1280x960 fullscreen, connected to `.132:27015` | **1 stall / 150 s, worst 33 ms** |

Both readings are effectively clean, which is itself the finding: on `.123` the
kernel is **not** what stalls. That moved the search to the input path - the
box's mouse and keyboard are a 2.4 GHz wireless combo receiver
(`HID\Vid_30fa&Pid_1440`, one device presenting both) plugged into a **VIA Labs
USB 3.0 hub** (`USB\Vid_2109&Pid_2817`). A receiver dropout is invisible to
every CPU/DPC measurement and surfaces only as input lag, which is exactly the
symptom. USB 3.0 hubs also radiate in 2.4 GHz, so the hub is both a power-management
suspect and an interference suspect.
