# `carmageddon2-server` — a Carmageddon 2 LAN game hosted on 192.168.1.132

Installed 2026-10-01. **Carmageddon 2 has no dedicated server**: multiplayer is
peer-hosted, with one player picking *NETWORK GAME → HOST* and waiting in the
"LOADING STATUS" lobby while the others join. So this "server" is the game
itself: the staged tree's **`Carma2_SW.exe`** (the software renderer; there is
no Glide or Direct3D in the container) under Wine in docker, driven into that
lobby by `host.py` on every start, with the tree's own **IPXWrapper 0.4.0**
tunnelling its IPX over UDP.

| | |
|---|---|
| binary | `Carma2_SW.exe` v1.02a, from `Games-Library/Carmageddon2` |
| runtime | Wine 8 in docker (`retro-wine:bookworm`, `--net=host`, `xvfb-run`) |
| base dir | `~/carmageddon2-server` (`game/` = the tree, `_run/` = these files) |
| port | **UDP 54792** (IPXWrapper; the game's IPX socket inside it is 0x2FFE) |
| NIC | **`enp129s0` only** (the one carrying 192.168.1.132) — see below |
| joined how | fleet box: *Play Carmageddon 2* → NETWORK GAME; the host shows in **JOIN A GAME** as `FLEET HOST` |
| wine prefix | docker named volume `carma2-wineprefix` |

```bash
systemctl --user status carmageddon2-server
tail -f ~/carmageddon2-server/server.log          # host.py's screen-by-screen log
cat ~/carmageddon2-server/_run/state.json         # what the frame shows now
python3 scripts/game-servers/healthcheck.py       # includes it
bash scripts/game-servers/carmageddon2/install.sh # (re)install from the share
```

## How hosting is proven, not assumed

Every failure here looks like a healthy process: the game can sit on an intro
movie, on the start screen or on the network menu with UDP 54792 bound and
nothing hosting. Two independent checks:

1. **The game's own join handshake** (`probe_carma2` in `gameservers.py`, used
   by `healthcheck.py` and the watchdog). Read out of `Carma2_SW.exe`: a joiner
   broadcasts `sprintf("XXXX%s%0.1d", "CAR2MSG", 1)` and a host answers
   `...CAR2MSG2` — **only while its net mode is "hosting"** (the reply is gated
   on `[0x685e60] == 2`). The probe frames that as an IPXWrapper datagram, sends
   it to 192.168.1.255:54792 out of `enp129s0` and accepts a `CAR2MSG2` only
   from 192.168.1.132 (a fleet box hosting its own game answers too). It must
   broadcast: a unicast to our own address arrives on `lo`, which the NIC pin
   drops.
2. **The frame.** `host.py` reads `xwd -root` every 3 s, names the screen
   (`start` / `netmenu` / `lobby`, else `unknown`/`racing`), counts the lobby's
   player rows and writes `_run/state.json`. The probe reports that state and
   the remote player count (rows minus the host's own car).

## Lifecycle (host.py)

intro movies → Escape (at most 6, and never once a menu has appeared — an
Escape on the start screen opens the main menu) → **start screen**: click
NETWORK GAME → **network menu**: click HOST (Driven to Destruction, the
default track) → **lobby**: `HOSTING`. When at least one remote player has sat
in the lobby with no change for **45 s** (`C2_START_AFTER_S`), it clicks
**START**. One race per container: after `C2_RACE_MAX_S` (30 min), or if it
cannot get back to a lobby for 5 min, or never reaches one within 240 s,
`host.py` exits, `entry.sh` stops Wine and systemd (`Restart=always`) starts a
fresh host. Snapshots of the hosting, race-start and failure frames are left in
`_run/*.xwd`.

## The traps that cost time

### 1. The host is multi-homed ON THE FLEET SUBNET

`enp129s0` carries .132 (and a DHCP secondary .196) and Wi-Fi `wlp128s20f3`
carries .129 — the same /24. IPXWrapper broadcasts to each interface address's
subnet broadcast, and Linux routes `192.168.1.255` out of **Wi-Fi**
(`ip route get 192.168.1.255` → `dev wlp128s20f3 src 192.168.1.129`). Its
default "wildcard" interface also spans every docker bridge and tailscale
(whose all-zero MAC *is* the wildcard key). Two fixes, both verified:

* `run-carmageddon2-server.sh` writes IPXWrapper's own config
  (`HKCU\Software\IPXWrapper`, layout from its `src/config.c`) on every start:
  the wired NIC primary and enabled, the wildcard and every other NIC disabled.
  `entry.sh` reads it back with `wine reg query` and says so loudly if absent.
* `bindiface.so` (LD_PRELOAD) sets `SO_BINDTODEVICE enp129s0` on the socket that
  binds UDP 54792. **In Wine 8 the bind is executed by the 64-bit `wineserver`**,
  not by the 32-bit game, so the shim is 64-bit (the 32-bit processes print a
  harmless "wrong ELF class" line). Proof it took: `ss -uaep 'sport = :54792'`
  shows `0.0.0.0%enp129s0:54792`, and `_run/bindiface.log` records the pin. A
  probe sent out of Wi-Fi gets no answer; one out of `enp129s0` does.

### 2. No sound card → a NULL write before the first frame

With no audio device DirectSound fails to initialise and Carma2 writes through a
NULL pointer at 0x51694B (`Unhandled page fault on write access to 00000000`).
`asoundrc` gives ALSA a null sink to open.

### 3. A bare `xdotool click` is lost

The menus poll DirectInput; the button must be held (`mousedown`, 300 ms,
`mouseup`). Keyboard Escape works for the intros.

### 4. The frame is 24-bit packed and DirectColor

Under Wine's 8-bit palette emulation `xwd -root` reports a 24 bpp ZPixmap with
a padded 4096-byte stride and a **DirectColor** visual whose channel bytes index
the colormap. Read as 32 bpp the picture is striped; without the colormap it is
false-coloured. `host.py`'s `Frame` handles both (pure stdlib — the image has no
PIL).

### 5. The lobby title blinks

"LOADING STATUS" flashes, so a reference including it matched every other frame
and the driver flapped lobby/unknown. `frame_refs.json` matches each screen on
several small regions that avoid the red headings.

### 6. `xvfb-run` needs `--init`

Without `docker run --init`, xvfb-run as PID 1 never sees Xvfb's ready signal
and the entry script never starts.

## Per-installation values

None needed. `install.reg` (seeded into the prefix) is the renderer/parental
lock answers; there is no CD check (`Carma2_SW.exe`'s SafeDisc 1.01.034 is
residue — the entry point is in `.text`) and no CD key or serial. IPXWrapper's
node number on the wired interface is that NIC's MAC, distinct from every box.
The host's name is set in the server copy's `data/OPTIONS.TXT`
(`NetName`/`PlayerName` = `FLEET HOST`) by `install.sh`, never in the library.

## What is proven, and what is not (2026-10-01)

* **Proven on the host:** it reaches the lobby by itself on every start (~55 s,
  three cold starts by systemd), `ss` shows the socket as `0.0.0.0%enp129s0:54792`,
  and the `CAR2MSG1` probe sent out of `enp129s0` gets `CAR2MSG2` from
  192.168.1.132, while one sent out of Wi-Fi gets nothing. The watchdog and
  `healthcheck.py` both report it OK. Evidence: `.claude/evidence-carma2/`.
* **NOT proven: a fleet box joining it.** The only boxes with the title online
  during this work (`.123`, `.197`) were running another session's Descent
  DOSBox LAN test the whole time and were not touched. The two-box proof is the
  next step: on an XP box, *Play Carmageddon 2* -> NETWORK GAME, look for
  `FLEET HOST` under JOIN A GAME, join, wait 45 s for the race to start, and
  photograph both the box and the host (`docker exec c2srv xwd -root`, decode
  with host.py's `Frame`).
