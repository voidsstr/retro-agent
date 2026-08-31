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


# A REFUSED CONNECTION IS USUALLY CONTENTION, NOT DEATH.
#
# The Win9x agents are single-threaded: they serve one client at a time and
# REFUSE every other connection while busy. So while a box agent is working
# .243, this watcher's probe is refused -- and reporting that as DOWN produces
# a fake power-cycle event, which tells the working agent its box vanished and
# invites it to abandon or mis-record whatever it was measuring.
#
# Measured 2026-08-31 on .243: a raw TCP connect to 9898 succeeded while five
# consecutive protocol probes were refused, because a sibling agent held the
# single slot throughout. CLAUDE.md says the same thing about box-owner.py --
# "a refused 9898 is usually listen-backlog contention, so retry".
#
# Retries are therefore spent ONLY on refusal. A timeout or an unreachable host
# is not retried: those are what a box being switched off actually looks like,
# and retrying them would delay a real event by the full backoff on every
# sweep, for every powered-off machine -- and most of this fleet is off most of
# the time.
REFUSAL_RETRIES = 3
REFUSAL_BACKOFF = 4.0


async def probe(ip):
    refused = 0
    while True:
        try:
            c = RetroConnection(ip, 9898)
            await c.connect(SECRET, timeout=TIMEOUT)
            break
        except ConnectionRefusedError:
            refused += 1
            if refused > REFUSAL_RETRIES:
                # Persistently refusing is a real fault, but it is NOT the same
                # fault as silence: something is listening and turning us away.
                return ip, "BUSY-OR-REFUSING"
            await asyncio.sleep(REFUSAL_BACKOFF)
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


# DEBOUNCE: a box must MISS TWICE before we call it gone.
#
# Measured 2026-08-31, and the reason matters. While three box agents were
# testing, this watcher reported .143, .240 and .243 down within seconds of
# each other. Probing each with patience separated three different things:
#
#   .143  answered on the FIRST retry            -> transient, it never left
#   .240  OSError x4, no route                   -> genuinely powered off
#   .243  ConnectionRefusedError x4              -> listening and refusing
#
# Only one of those was a real departure. A busy box answers slowly -- these
# agents are being driven hard, and .143 needed a longer timeout than a quiet
# sweep does -- so a single missed probe is not evidence. Requiring two
# consecutive misses costs one sweep of latency on a real power-off and removes
# the false events entirely.
#
# This matters beyond tidiness: a false DOWN tells the agent working that box
# that its machine vanished, and the standing instruction on a vanished box is
# to stop and record cells as untested. A flapping watcher would therefore
# manufacture exactly the gaps this whole exercise exists to close.
MISSES_BEFORE_DOWN = 2


async def main():
    state = {}
    misses = {}
    first = True
    while True:
        results = await asyncio.gather(*[probe(h) for h in HOSTS])
        for ip, now in results:
            was = state.get(ip)

            # Debounce only the transition INTO a non-answering state; a box
            # coming back is reported immediately, because a late UP wastes an
            # agent's time while a late DOWN costs nothing.
            if now in ("DOWN", "BUSY-OR-REFUSING") and was == "UP":
                misses[ip] = misses.get(ip, 0) + 1
                if misses[ip] < MISSES_BEFORE_DOWN:
                    continue
            else:
                misses[ip] = 0

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
            elif now == "BUSY-OR-REFUSING":
                print("%s FLEET-BUSY %s refused %d probes - most likely a "
                      "single-threaded agent fully occupied by another client, "
                      "NOT a box that went away" % (stamp, ip, REFUSAL_RETRIES),
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
