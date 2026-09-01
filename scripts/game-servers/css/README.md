# `css-server` — Counter-Strike: Source on 192.168.1.132:27025

| | |
|---|---|
| engine | **srcds**, Steam **app 232330** (`Counter-Strike Source Dedicated Server`) |
| origin | fetched from Valve with SteamCMD, **`+login anonymous`** — free, no account, **no CS:S licence needed for a server** |
| base dir | `~/css-server` |
| game port | **27025/udp** (27015–27021 are taken by the CS 1.6 / HLDM servers and their A2S proxies) |
| SourceTV | 27035/udp, pinned |
| maps | 20 stock + **1,066 from Extreme MapPack v2** |
| unit | `systemctl --user status css-server` |

## Why this exists when CS:S itself is NOT staged

Every copy of CS:S on the share is cracked — three copies that are really two
releases, both carrying the RevEmu / "REVOLUTiON" Steam emulator, one with a
literal `# Online Crack` folder. None of it can be staged.

**The dedicated server is a different question entirely.** Valve distributes
srcds free to anyone, so this host can run a legitimate CS:S server without a
CS:S licence and without touching that media. That is what this is.

Be clear-eyed about the consequence: **no fleet box can be a client.** A CS:S
client needs Steam, and Valve ended XP/Vista support 2019-01-01 and Win7/8.1
support 2024-01-01. This server is for a machine that can run a modern Steam
client — the OMEN, or anything else on the LAN — not for the retro boxes.

## The map pack

`Files/Games/Windows XP/Counter-Strike Source Extreme MapPack v2/` is an Inno
Setup installer in five disk slices. **Its file sizes look wrong and are not** —
three slices are exactly 2,100,000,000 bytes, which reads like a padded or
truncated download. They are Inno `TDiskSliceHeader` volumes: each begins with
the magic `69 64 73 6b 61 33 32 1A` (`idska32`) and the `LongWord` at offset 8
is that slice's self-declared size. All five match their actual size exactly.
The set is complete.

It contains **no executable at all** — 1,066 `.bsp`, 1,064 `.nav`, a few `.txt`,
and four `app/cstrike/custom/bruss.org.ru_*.vpk` branding files. The maps are
plain community content; the VPKs are the repackager's advertising and are
**deliberately not installed**.

Extracted on Linux with `innoextract` — no Windows and no VM needed:

```bash
innoextract --list --silent "<pack>/Setup.exe"          # verify before extracting
innoextract --silent --output-dir ~/css-mappack \
            --include "app/cstrike/maps" "<pack>/Setup.exe"
cp -n ~/css-mappack/app/cstrike/maps/*.{bsp,nav,txt} ~/css-server/cstrike/maps/
```

## Two things that cost time, both worth knowing

**`-ip` IS REQUIRED.** Without it srcds resolved the hostname to `127.0.1.1`
out of `/etc/hosts` and bound *there*. UDP 27025 **was listening** — and was
unreachable from the LAN and from `127.0.0.1` alike. A bound port is not a
reachable server; the log line to check is
`Network: IP 192.168.1.132, mode MP, dedicated Yes`.

**Do NOT use `-strictportbind`.** The HLDS servers on this host all pass it, so
it looks like the house style. srcds wants several ports, not just the game
port, and its SourceTV default 27020 is already held by `a2s-proxy-hldm`. With
`-strictportbind` that collision is **fatal** (`status=100`, restart loop) even
though the game port was free. `+tv_port 27035` pins the alternative instead of
letting it silently pick whatever is free — it chose 27022 on the first run.

## Querying it

Modern srcds answers `A2S_INFO` with an `A` challenge that must be echoed back:

```python
req = b'\xff\xff\xff\xffTSource Engine Query\x00'
sock.sendto(req, (host, port));  d, _ = sock.recvfrom(4096)
if d[4:5] == b'A':                      # challenge, not the answer
    sock.sendto(req + d[5:9], (host, port));  d, _ = sock.recvfrom(4096)
```

A plain one-shot query times out and looks like a dead server.

## Steam SDK symlink

srcds logs `dlopen failed ... ~/.steam/sdk32/steamclient.so`. Create it once:

```bash
mkdir -p ~/.steam/sdk32
ln -sf ~/steamcmd/linux32/steamclient.so ~/.steam/sdk32/steamclient.so
```
