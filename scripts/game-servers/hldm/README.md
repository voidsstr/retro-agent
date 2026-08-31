# Half-Life Deathmatch server (`hldm-server`) — added 2026-08-31

A dedicated **Half-Life Deathmatch** (GoldSrc `valve` mod) server on the dev
host, plus the A2S old-query proxy that puts it in a 1999 LAN browser.

| unit | UDP | what |
|---|---|---|
| `hldm-server` | **27021** | the real HLDS (`~/hldm-server`, `hlds_run -game valve`) |
| `a2s-proxy-hldm` | **27020** | old-query proxy → 27021; **this is the port players use** |

```bash
cp hldm-server.service a2s-proxy-hldm.service ~/.config/systemd/user/
systemctl --user daemon-reload
systemctl --user enable --now hldm-server a2s-proxy-hldm
python3 ../healthcheck.py            # both rows must read [ OK ]
```

`cfg/server.cfg` and `cfg/mapcycle.txt` belong in `~/hldm-server/valve/`.

## READ THIS BEFORE YOU POINT A CLIENT AT IT

**The staged `HalfLife1` tree CANNOT join this server, or any other GoldSrc
server on this host.** It is the WON build and speaks network **protocol 45**;
every HLDS here is **protocol 48**. Measured 2026-08-31 — the client refuses
with

    This server is using a newer protocol ( 48 ) than your client ( 45 ).
    You should check for updates to your client.

and the last WON patch (1.1.1.0) is only protocol 47, so patching that tree
does not fix it. Its multiplayer is box-to-box LAN between machines running
that same copy, and that is proven separately.

**The client that CAN join is the `CounterStrike16` tree's engine.** It is the
Steam-era build and it already ships a complete `valve\` mod directory — game
dlls, client dlls, wads — with **no maps**, which it downloads from here
(`sv_allowdownload 1`). That tree now carries a
`Play Half-Life Deathmatch.bat` shortcut doing exactly:

    hl.exe -game valve -full -w %FR_W% -h %FR_H% +connect 192.168.1.132:27020

**VERIFIED 2026-08-31, two boxes, no bots.** `.171` and `.124` both joined from
that shortcut and the server's own `status` read:

    hostname:  NSC Retro Fleet - Half-Life Deathmatch
    version :  48/1.1.2.2/Stdio 10211 insecure
    map     :  crossfire
    players :  2 active (12 max)
    # 1 ... ping 11 ...
    # 2 ... ping  8 ...

Both were rendering crossfire fullscreen at the same moment. Half-Life
Deathmatch has no bots, so a player count here is always people.

**Why the players' address column shows 192.168.1.132.** They connect through
the a2s proxy on 27020, which forwards game traffic on a per-client upstream
socket — so the server sees the proxy's address with a distinct source port
per client, not the fleet box's. That is the proxy working, not a fault. To see
the real client addresses, connect straight to **27021** instead.

The game content came from Valve's own dedicated-server package
(`steamcmd +app_set_config 90 mod valve +app_update 90`), which is also where
the missing `dmc\events\*.sc` files in the `HalfLife1` tree were recovered from.
