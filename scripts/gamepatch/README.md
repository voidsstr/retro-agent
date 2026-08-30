# LAN patch-level audit

`audit.py` answers one question for every fleet box: **can this machine's game
clients actually join the dedicated servers on this host?**

```bash
python3 scripts/gamepatch/audit.py          # every box in the gameindex DB
python3 scripts/gamepatch/audit.py -v       # also show what passed, and why
                                            # anything is "unknown"
python3 scripts/gamepatch/audit.py --host 192.168.1.145 --json
```

Exit status is non-zero only when something is a real **mismatch**. It never
writes to a box.

## Three states, never two

    ok        the marker was read and matches what the server needs
    mismatch  the marker was read and does NOT match
    unknown   the marker could not be read

`unknown` is not `mismatch`. A powered-off box, or a game directory that has
moved, must not be reported as a client that will fail to connect — that sends
someone to the wrong machine with the wrong fix. `Marker.state` accepts no
other values and an `unknown` must carry a reason.

## What each engine is judged on

| engine | marker | why not the obvious thing |
|---|---|---|
| GoldSrc | `steam.inf` `PatchVersion` ∈ known protocol-48 set; **no `steam.inf` anywhere = WON-era = fail** | comparing PatchVersion to the *server's* for equality reported 56 mismatches, none real — 1.1.2.5 and 1.1.2.7 are both protocol 48 |
| Quake III | any `pak*.pk3` in `baseq3` | requiring the retail `pak0..pak8` set failed .145, which has only pak0..pak6 and was at that moment playing on the server |
| Quake II | a recognised engine exe | protocol 34 is stable across 3.20 and every source port |
| UT99 | the `OldUnreal469*.u` stamp package; **a 436 tree also passes** | the first version looked for `VulkanDrv`/`XOpenGLDrv`/`SDLDrv` — the *Linux server's* renderers, which no Windows client has, so all 15 UT99 installs read as 436/451 |

It also flags **files differing only in case** in a UT99 `System/` directory:
two such files are one file on a Windows client, and which one survives the
copy is arbitrary.

## Scope: an engine is not a game

Only games we host a server for are audited (`HOSTED`). The gameindex records
`q3` for Jedi Academy and SoF2 — whose paks live in `base/`, not `baseq3` — and
`unreal` for Unreal Gold as well as UT99. Auditing those against our Quake III
and UT99 servers reported healthy installs as broken clients that were never
going to connect to those servers anyway.

## Verified on hardware, 2026-08-29

Every rule above was checked by launching the real client on .145 against the
real server and confirming in the **server's own player list** (or the client's
console log) — not by reasoning about version numbers:

| game | client | server | result |
|---|---|---|---|
| CS 1.6 | 1.1.2.5 | 48/1.1.2.7 | `Connection accepted by 192.168.1.132:27018` |
| UT99 | OldUnreal 469c | 469a | joined as `pigga` |
| Quake III | ioquake3, pak0..pak6 | ioquake3 1.36 | joined as `BOX145` |
| Quake II | quake2.exe | Yamagi 8.60, protocol 34 | joined as `Player` |
| The Specialists | WON HL 1.1.1.0 | 48 | **process exits on `+connect`** |
| UT99 | true 436 (no OldUnreal stamp) | 469a | joined as `pigga` |

**436 and 469 interoperate**, which matters because `UnrealTournament436` is a
deliberately staged library title for the boxes whose CPUs predate SSE2 and so
cannot run 469 at all — it is on 7 of the 9 boxes. A 436 tree is therefore a
supported client, not a leftover to be upgraded, and the audit passes it.

Two traps worth remembering, both of which nearly produced a wrong answer here:

- **A server's player count does not move for a player sitting at team
  select.** The CS client was connected and playing the map while A2S still
  reported 0 players. "Player count did not change" is not evidence of a failed
  connect — read the client's own log.
- **A blocked dialog looks exactly like a protocol failure.** UT99 sat on its
  "Recovery Mode" dialog and never attempted the connection at all. One
  screenshot distinguished "refused by the server" from "never asked".
