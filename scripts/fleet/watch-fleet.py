#!/usr/bin/env python3
"""Emit one line each time a fleet box's agent transitions UP or DOWN.

Written for a session where the operator is power-cycling machines by hand and
wants newly-arrived boxes picked up without being asked for. Each stdout line
is an event; the caller reacts to `UP` by dispatching work to that box.

THREE THINGS THIS GETS RIGHT, each of which has cost this project real time:

* **A successful TCP connect is NOT liveness.** When .243's agent died, port
  9897 kept accepting sockets while answering nothing, and 139 stayed open
  because the OS was fine. So this speaks the real protocol and requires a
  PONG. `ACCEPTS-BUT-DEAD` is reported as its own state, because "the agent
  crashed" and "the machine is off" need different responses from a human.
* **Only TRANSITIONS are events.** The fleet is deliberately powered off most
  of the time, so a steady state of DOWN is normal and must not generate
  traffic. Reporting the level rather than the edge would make the common case
  the noisy one.
* **Slow boxes are not absent boxes.** .171 and the Pentium answer slowly
  enough that a short timeout drops them from sweeps entirely, so the timeout
  here is generous and every host is probed concurrently -- the sweep costs
  wall-clock only on its slowest member.

Exits only on signal; intended to run under Monitor with persistent: true.
"""
import asyncio
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))
from client.retro_protocol import RetroConnection      # noqa: E402

SECRET = "retro-agent-secret"
HOSTS = ["192.168.1.%d" % n for n in (123, 124, 133, 143, 145, 171,
                                      240, 243, 246)]
# Generous: the Pentium 1 and .171 both answer slowly, and a false DOWN would
# be reported as a power-cycle that never happened.
TIMEOUT = 10.0
INTERVAL = 20.0


async def probe(ip):
    try:
        c = RetroConnection(ip, 9898)
        await c.connect(SECRET, timeout=TIMEOUT)
    except Exception:
        return ip, "DOWN"
    try:
        r = await c.command_text("PING", timeout=TIMEOUT)
        return ip, ("UP" if "PONG" in r.upper() else "ACCEPTS-BUT-DEAD")
    except Exception:
        # Connected, then would not answer: the listener outlived its agent.
        return ip, "ACCEPTS-BUT-DEAD"
    finally:
        try:
            await c.close()          # graceful: an abrupt RST crashes Win98
        except Exception:
            pass


async def main():
    state = {}
    first = True
    while True:
        results = await asyncio.gather(*[probe(h) for h in HOSTS])
        for ip, now in results:
            was = state.get(ip)
            if was == now:
                continue
            state[ip] = now
            if first:
                continue          # the opening sweep is a baseline, not news
            stamp = time.strftime("%H:%M:%S")
            if now == "UP":
                print("%s FLEET-UP %s is online (agent answering PONG)"
                      % (stamp, ip), flush=True)
            elif now == "ACCEPTS-BUT-DEAD":
                print("%s FLEET-DEAD-AGENT %s accepts TCP but does not answer "
                      "- the machine is up and its agent is not" % (stamp, ip),
                      flush=True)
            else:
                print("%s FLEET-DOWN %s stopped answering" % (stamp, ip),
                      flush=True)
        if first:
            up = sorted(i for i, s in state.items() if s == "UP")
            print("%s FLEET-BASELINE up=%s" % (time.strftime("%H:%M:%S"),
                                               ",".join(up) or "(none)"),
                  flush=True)
            first = False
        await asyncio.sleep(INTERVAL)


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
