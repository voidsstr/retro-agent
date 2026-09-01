# LAN multiplayer — what is verified, what is hosted, what needs a person

**Every row here was proved on real hardware**, not inferred from a menu, a
config file or a `netstat` line. The standard is the one CLAUDE.md sets: **host
on box A, join from box B, and capture BOTH ends** — the host hosting, the
joiner seeing the host's game, and both players in the match together.

Two things that are deliberately *not* evidence, because both have misled this
project before: a **Network menu existing** (SoF 1 has one and demands the CD),
and a **player count** that has not had bots subtracted (Quake III runs
`bot_minplayers 4`; on the Quake family a player line with **ping 0** is a bot,
and SoF2's player lines carry a third number so the ping field is not where you
think it is).

*Measured 2026-08-30/31 across `.123 .124 .133 .143 .171 .240 .246`. `.145` was
offline (its cable was in the Win98 box) and `.243` (Win98 Pentium) was off the
network awaiting a keyboard.*

*Re-measured and widened 2026-09-01 across `.123 .133 .143 .145 .240 .246`
(`.124` `.171` `.243` switched off). That run added the ALL-SIX-BOXES section
below, found that **Tribes 2 has no client staged anywhere**, found the **Halo
CD keys had gone back to being identical on every box that has the game**, and
lost `.123` mid-session — see the last section.*

---

## VERIFIED LAN — both ends screenshotted

| Title | Engine / transport | Proven on |
|---|---|---|
| Half-Life | GoldSrc | `.240` + `.123` |
| Half-Life — Team Fortress Classic | GoldSrc | `.171` + `.133` |
| Half-Life — Opposing Force | GoldSrc | `.171` + `.133` |
| Half-Life — Deathmatch Classic | GoldSrc | `.171` + `.133` |
| Counter-Strike 1.6 | GoldSrc | `.171` + `.124` |
| Half-Life Deathmatch | GoldSrc (fleet server) | `.171` + `.124` |
| Quake 1 | NetQuake | `.123` + `.240` |
| Quake II | id Tech 2 | **ALL SIX live boxes in one game 2026-09-01** — `.123` `.133` `.143` `.145` `.240` `.246` on the fleet server `:27910` |
| Quake III Arena | id Tech 3 | **ALL SIX live boxes in one game 2026-09-01** — `.123` `.133` `.143` `.145` `.240` `.246` on the fleet server `:27961` |
| Quake III: Team Arena | id Tech 3 | `.123` + `.240` |
| Hexen II | Quake-derived, peer | `.123` + `.240` |
| SiN Gold | id Tech 2 | `.123` + `.240` |
| Soldier of Fortune II | id Tech 3 | `.123` + `.240` |
| Jedi Academy | id Tech 3 / Raven | `.143` + `.246` (both on the fleet server; `.246`'s console reads `Fleet143 connected`) |
| Jedi Knight: Dark Forces II | Sith / DirectPlay TCP-IP | `.123` + `.240` |
| Mysteries of the Sith | Sith / DirectPlay TCP-IP | `.123` + `.240` |
| Unreal Tournament (436 client) | UE1 | `.143` + `.133` |
| Unreal Tournament (469e) | UE1 | fleet server |
| UT2004 | UE2 | `.240` + `.246` |
| Unreal Gold | UE1 | `.143` + `.246` |
| Deus Ex | UE1 | `.143` + `.246` |
| Red Alert 2 | Westwood, peer | `.246` + `.143`; **re-proved `.123` + `.240` 2026-09-01, both ends in the match** |
| Yuri's Revenge | Westwood, peer | `.246` + `.143` |
| Tiberian Sun | Westwood, peer | `.240` + `.123` |
| StarCraft | peer, UDP LAN | `.246` + `.143` |
| Descent 1 | DOSBox IPX tunnel | `.246` + `.124` |
| Descent 2 | UDP/IP native | `.123` + `.240` |
| Descent 3 | dedicated server | `.240` |
| Doom 3 | id Tech 4 | `.123` + `.246` peer; **`.123` + `.240` on the fleet server 2026-09-01** |
| Return to Castle Wolfenstein | id Tech 3 | `.143` + `.246` peer; **FIVE boxes in one game on the fleet server `:27963` 2026-09-01** — `.133` `.143` `.145` `.240` `.246` (`.123` had already dropped off the LAN) |
| Serious Sam: The First Encounter | Serious Engine 1, dedicated server | `.123` + `.133` + `.240` |

**30 titles.**

---

## ALL SIX BOXES IN ONE GAME — measured 2026-09-01

**User directive: "have them run them on all connected computers together i want
to make sure they can all run multiplayer".** A two-box join is the *floor*, not
the deliverable. Where a title has a dedicated server on this host the whole
fleet can be put in one match at once, and that is what these rows are.

Boxes live that day: `.123` `.133` `.143` `.145` `.240` `.246`
(`.124` `.171` `.243` were switched off).

| Title | In one game at once | Excluded, and why |
|---|---|---|
| **Quake III Arena** (`:27961`) | **6 of 6** — `.123` `.133` `.143` `.145` `.240` `.246` | none |
| **Quake II** (`:27910`) | **6 of 6** — same six | none |
| **Return to Castle Wolfenstein** (`:27963`) | **5** — `.133` `.143` `.145` `.240` `.246` | `.123` had already dropped off the LAN (see below); it is otherwise eligible and was proved two-box on `:27963` an hour earlier |
| **Red Alert 2** (peer) | **2** — `.123` hosted, `.240` joined | not driven wider before `.123` fell over; the lobby is menu-work per box, not a `+connect` |
| **StarCraft** (peer) | **0 this session** — see below | `.123` cannot run it at all (`disc_mount`) |
| **Halo** (peer) | **0** — there is no host | `.133` `.143` gated on SSE2; `.123` offline |
| **Tribes 2** | **0** — there is no client | every box |

**How the evidence was taken, because the method is reusable.** Every id Tech
title takes `+connect <ip>:<port>` and `+exec <cfg>` on the command line, so a
whole fleet joins one server with no menu work at all. Give each box
`seta name "Fleet<octet>"` and a key bound to `say`, and the scoreboard *is* the
proof: `~/lan-proof/quake3/allbox6_q3a_143_scoreboard.png` lists all six names
with real pings, above four bots at ping 0.

> ### `UIKEY` DOES reach an id Tech 3 game in exclusive fullscreen
> CLAUDE.md records that "id Tech 3 ignores synthetic keyboard input in
> exclusive fullscreen", measured on SoF2. **That is a MENU fact, not an engine
> fact.** In-game, at fullscreen 1920x1080 on `.123` and `.240`, `UIKEY F5`
> fired a bound `say` and `UIKEY F6` opened the scoreboard on ioquake3, Quake II
> and RTCW alike. This is what made the six-box proof possible without anyone at
> a keyboard — and it is worth knowing before anyone reaches for the windowed
> workaround again.
>
> **It does NOT generalise to Halo.** See the Halo section: `halo.exe -window`
> reaches its main menu and then ignores `UICLICK` *and* `UIKEY` just as it does
> fullscreen.

> ### Getting a frame off `.246` (Windows 7) — use the game's own screenshot
> GDI returns a black frame for an exclusive-fullscreen surface on Vista+, so
> `SCREENSHOT 0` on `.246` is `extrema=((0,0),(0,0),(0,0))` every time. Bind a
> key to the engine's own screenshot command instead (`screenshotJPEG` on
> ioquake3, `screenshot` on Quake II), press it with `UIKEY`, and `DOWNLOAD` the
> file: ioquake3 writes `%APPDATA%\Quake3\baseq3\screenshots\`, Quake II writes
> `baseq2\scrnshot\`. `allbox6_q3a_246_scoreboard.jpg` was taken that way and
> shows all six players.

### TRIBES 2 — there is no client, and there never was

**This is the whole answer, and it is not a failed test.** Measured 2026-09-01:

- `Games-Library` has **no `Tribes2` directory**. `compat.py record` refuses the
  title outright — *"unknown title 'Tribes2' — it must be a Games-Library
  directory name"* — which is the database saying the same thing.
- **No fleet box has `C:\Games\Tribes2`** (checked on all six).
- `tribes2-server` (docker, UDP `28000`, TribesNext) is up and answers
  `healthcheck.py`. So the fleet has a Tribes 2 *server* and nothing that can
  dial it.

"Tribes 2 has never been tested" was therefore never a gap in testing. It was a
title nobody staged, and a healthy server on a port no client on this network
can reach — the exact shape this project keeps getting caught by, a green tick
standing in for a thing that does not exist.

**What it would take:** the installer is on the share as
`Files/Games/Windows XP/Tribes2_gsi.exe` (564,721,416 B; **an InstallShield PE,
not an archive** — `7z` refuses it, so it must be *run*). A file of exactly the
same size sits at `Files/Games/Demos & Shareware/Tribes2Demo.exe`; md5 them
before assuming either is a demo. Install it in the **build VM** (never on a
fleet box), apply whatever TribesNext client patch this server's build expects
— TribesNext replaced the dead Sierra master **and** encrypts the info response,
so a vanilla client may not be able to join at all, and *that* is the question
to answer before spending an hour on an install — then stage it and `GAMESYNC`.


### Staged, LAN-capable, gather not yet proven

Added in the same batch as RTCW and deployed to all seven live boxes.
Each runs fullscreen and its **transport is proven** (the IPX tunnel is
confirmed connected end to end), but no two-box match has been completed:

| Title | What is proven | What is missing |
|---|---|---|
| Warcraft II: Battle.net Edition | runs; ships IPXWrapper | its 8-bit DirectDraw surface is **uncapturable by GDI on both XP and Win7**, so the agent cannot photograph it at all |
| Warcraft: Orcs & Humans | runs under its own DOSBox | campaign/network screen is mouse-only |
| Shadow Warrior Classic Complete | runs; SETUP.EXE driven end-to-end by keyboard | the in-game gather |
| Master of Orion II | runs; **MULTI PLAYER** present natively | that menu entry is mouse-only |

**Why these four stall on the same thing.** *Absolute clicks cannot drive a
DOSBox game's mouse.* DOSBox turns host mouse **motion** into DOS deltas, and
moving the cursor to an unfocused window produces none — `autolock=false`
does not change it. `UIKEY` works, which is why Shadow Warrior's setup could
be completed; menus with no keyboard path cannot. One person, once, at a
mouse finishes all four.

### WITHDRAWN, THEN RECOVERED — Serious Sam

**Both Encounters are staged again, and the withdrawal was a wrong conclusion
from a right observation.** They were pulled on 2026-08-30 as "disc-locked and
unfixable"; they are back on 2026-08-31 and TFE's LAN is in the table above.

The old entry said `SeriousSam.exe` "imports `GetDriveTypeA` and walks the drive
letters for a CD-ROM-**typed** volume", which is exactly right. What it did not
say is that the check **also opens a file on that volume** —
`<drive>:\Install\Bin\SeriousSam.exe` — and that the binary carries **no copy
protection at all**: no `stxt774`, no `stxt371`, no `BoG_` marker, no `secdrv`.
It is not SafeDisc and needs no SafeDisc emulation, which is what separates it
from Generals and BF1942 and is why **DAEMON Tools 3.47 satisfies it
completely**.

So a CD-ROM-typed drive is **necessary and not sufficient**, and that is what
actually defeated the first attempt: six of the seven live boxes already had
`DRIVE_CDROM` volumes and every one of them held *another game's disc* or
nothing. "The fleet has mounters" was mistaken for "the fleet has this disc".
Measured both ways on 2026-08-31 — `.240` with the SHOGO disc in `F:` raised the
modal; the same box with its own image mounted started the game.

One correction worth carrying forward, because it inverts the usual rule:
**TSE retail is v1.05 and a later official patch DOES exist —
`serious-sam-tse-1.07.exe`, on the share — and it must NOT be applied.** It
replaces the 442,434-byte retail exe with a 1,777,634-byte **SafeDisc 2** one
and ships `secdrv.sys` beside it, dropping `GetDriveTypeA` entirely. It would
convert a title this fleet can run into one it demonstrably cannot — the exact
inverse of Doom 3, where the official 1.3 patch *removed* the wrapper. The TFE
1.05 patch is clean; neither is applied, because a LAN pair only has to agree
with itself.

**The disc requirement is declared per SHORTCUT, not per title**, because
`DedicatedServer.exe` has no CD check at all. `.123` — the only box on the fleet
with no optical drive and no mounter — therefore still receives the title, still
gets an icon, and **hosted the verified LAN game above**: it served `.133` and
`.240` simultaneously while being unable to play itself. A title-level
capability would have suppressed all three shortcuts and left it with nothing,
which is how Descent 2 lost both of its.

TSE is verified running fullscreen on `.240`; its LAN has not been proved on two
boxes yet. **Its main menu reads "THE FIRST ENCOUNTER v1.05" and the tree is
correct** — `SE1_00.gro` ships TFE's menu-logo textures byte for byte. The
campaign it loads is TSE's.

## Hosted on this computer — 24 servers, all `enabled` with linger on

`cs16-server` · `cs16-noblood` · `specialists-server` · `hldm-server`
(+ three A2S browser proxies) · `quake1-server` · `quake2-server` ·
`quakeworld-server` · `quake3-server` · `q3ta-server` · `openarena-server` ·
`sof2-server` · `jka-server` · **`rtcw-server`** · `ut99-server` ·
`ut2004-server` · **`unrealgold-server`** · **`deusex-server`** ·
**`doom3-server`** · **`ssam-tfe-server`** · **`ssam-tse-server`** ·
**`shogo-server`** · `tribes2-server` · `descent3-server` · `farcry-server`

**Seven of those were added on 2026-09-01** (bold), on a brief of "a dedicated
LAN server on this host for every staged title". Which staged title has one,
which cannot have one and why: [`staged-title-server-matrix.md`](staged-title-server-matrix.md).

### The RTCW server is the one that had been listed but never existed

`rtcw-server` sat in the game-servers skill's table as a wish list —
"no directory, no install script, no game data" — while RTCW was staged, played
box-to-box, and in this table above. It is real now: **ioRTCW 1.51c on
:27963**, and `.123` + `.240` both joined it from the staged retail tree, took
Axis and Allied, saw each other's chat, and survived the map rotation.
`scripts/game-servers/rtcw/README.md` has the three traps that decide whether
such a server works at all — the wrong-platform game module, the protocol-60
legacy handshake, and the fact that **27963 is the last port in the Quake III
LAN-scan window** so anything outside 27960-27963 never self-announces.

Check them with `python3 scripts/game-servers/healthcheck.py` — it uses the
**right query packet per engine**, which a single `getstatus` sweep does not.
Quake 1 and Hexen II answer *neither* standard probe (they speak Quake's own
control protocol, and a Hexen II host replies only to the game string
`HEXENII`); Tribes 2 needs the Torque binary form; MOHAA needs a framed
`getinfo`. A probe that guesses reports a healthy server as dead.

## NEEDS A PERSON, ONCE — and exactly what

| Title | What blocks it | The one-time step |
|---|---|---|
| **Soldier of Fortune 1** | multiplayer refused **even with the disc** — see below | a disassembly, or the 1.07f patch. NOT a mounter |
| **BF1942** | SafeDisc 2.80.010 in `Mods\bf1942\Mod.dll` blocks the *client*; the host launcher works | a mounter whose emulation covers SafeDisc 2.80 — a kernel driver and a reboot per box, so a **user decision** |
| **Far Cry** | server hosts unattended; CryEngine takes DirectInput exclusively | one click: Multiplayer → LAN |
| **Halo** | **JOINING is automated; HOSTING is not**, and as of 2026-09-01 the **CD keys were also duplicated** — see "Halo — the keys were the second problem" below. `halo.exe -connect <ip>:<port>` skips the menu, so a client needs nobody; a HOST does. | one person to start the game at the host's menu — OR `haloded.exe`, which is a free official download and is not on the share |
| **Carmageddon 1 / 2** | tunnel proven both ends; the front end ignores click *and* key | click **HOST GAME** / **NETWORK GAME** |
| **Hidden & Dangerous** | launcher bug fixed; stops at profile creation | create a profile, then copy `Savegame\*.bin` into the tree |
| **Aliens vs Predator** | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black | drive its menus at the keyboard |
| **Turok 2** | host works and is listed in the joiner's browser; join fails | investigate the GameManager path |
| **Redneck Rampage** | IPX tunnel proven; the Build gather never happens | **not** the CFG — that was already captured and md5s identical to the staged one. See its README-FLEET |
| **Shogo** | dedicated server stands up; client menu renders intermittently | drive the join at the keyboard |
| **Red Faction** | root cause fixed (`UpdateRate`); join unproven | re-test |

**A menu driven by RELATIVE MOUSE DELTAS cannot be automated at all** —
`UICLICK` sets an *absolute* pointer such menus do not follow. Descent 3, SoF2,
Deus Ex and Far Cry are all in that class. Recognising it early is worth
more than another hour of clicking.

### Retail `halo.exe` cannot host headlessly — REFUTED 2026-09-01, on hardware

Worth writing down because the binary looks like it should. `halo.exe`'s string
table carries the whole dedicated-server side:

    Dedicated server is running on map %s (%d / %d players)
    Game Complete. Dedicated server is now idle.
    sv_map / sv_mapcycle_begin / sv_map_next / sv_players ...
    init.txt

and a bare `dedicated`. So the server code is compiled in, and `-console`,
`-exec` and `init.txt` are all real switches. The obvious conclusion — that a
client can be told to host without touching the menu — **is wrong.**

What was actually tried on `.145`, each launched through a `.bat` that `cd`s to
the tree first (an absolute-path launch dies on `Cannot find 'C:\config.txt'`
before anything else happens), each left 55–80 s, each **screenshotted**:

| attempt | result |
|---|---|
| `-console -exec init.txt`, `init.txt` = `sv_map bloodgulch slayer` | Halo main menu. `sv_map` is *server-only* and a client at the menu is not a server. |
| `-dedicated` | Halo main menu |
| `-dedicated -console` | Halo main menu |
| `-server` | Halo main menu |
| `-console -dedicated` | Halo main menu |

All four unknown switches are silently ignored — no error, no log (`> _ded.log`
came back empty every time), just the menu. `netstat` shows `UDP 0.0.0.0:2302`
bound in **every** case including the plain menu, so **a bound 2302 is not
evidence of a server**; it is Halo's ordinary client port.

Meanwhile `.123` and `.240` were pointed at it with `-connect 192.168.1.145:2302`
and both answered **"Unable to join game."** — note that this is NOT "Your CD Key
is invalid", which is what a licensing rejection looks like. Nothing was wrong
with the keys; there was simply no game.

So the remedy is `haloded.exe` (the free official Halo Dedicated Server; a
case-insensitive sweep finds it nowhere on the share) or one person at the
host's menu. Do not spend another session on switches.

## NO MULTIPLAYER — measured, not assumed

- **Half-Life: Blue Shift** — `liblist.gam` declares `type "SP Mission"`, `maps\`
  is empty, and all 37 maps in `pak1.pak` are `ba_*` campaign. Its `mpentity`
  line is inherited boilerplate.
- **Max Payne** — `MaxPayne.exe` imports no `WS2_32`, `WSOCK32` or `DPLAYX` at all.
- **System Shock 2** — the menu offers New Game / Load / Options / Credits /
  Intro / Quit and nothing else.
- **Daggerfall** — single-player only by design; the GOG DOSBox build staged
  2026-09-01 reaches Load/New/Exit and offers no network entry at all. Recorded
  `no_multiplayer` on `.133` and `.143`.

## Peer-hosted by design — no dedicated server exists

Descent 1 and 2, Carmageddon 1 and 2, Redneck Rampage, Red Alert 2, Yuri's
Revenge, Tiberian Sun, StarCraft, Hexen II, SiN Gold, and both Jedi Knight
titles. For these the two-box proof **is** the deliverable; there is nothing to
stand up on the host, and inventing one would produce a unit that reports itself
healthy while nobody can join it.

---

## The mounter picture — RE-MEASURED 2026-08-31, and the old claim was wrong

**This section used to say "today only `.240` has any mounter". That was false,
and it blocked four titles for a day.** Measured with `sc query d347bus` plus
`wmic cdrom get Drive,Caption,MediaLoaded` on every box:

| box | mounter | virtual drives |
|---|---|---|
| `.123` | **NONE** — and no optical drive at all | — |
| `.124` | DAEMON Tools 3.47 (`d347bus`+`d347prt` RUNNING) | D: |
| `.133` | DT 3.47 | E: (D: is a real TEAC) |
| `.143` | DT 3.47 | D: E: F: G: — **four** |
| `.171` | DT 3.47 | E: (D: is a real LITE-ON) |
| `.240` | DT 3.47 | F: |
| `.246` | WinCDEmu | D: V: |

A DAEMON Tools virtual drive reports `Caption = "Generic DVD-ROM SCSI CdRom
Device"`; that string is how you tell it from a real drive. **Six of seven boxes
can mount an image today.** Re-measure before quoting this table.

### `disc_mount` does not mean "the disc check will pass"

Protection strength is a separate axis, and it decides the approach:

| class | how to identify it | satisfied by a virtual drive? |
|---|---|---|
| plain drive-type / volume-label scan | no `BoG_ *90.0&!!` magic, no `stxt774`/`stxt371` sections; `GetDriveTypeA` + `GetVolumeInformationA` | **yes** — Jedi Academy, Red Faction |
| SafeDisc 1.x – 2.5x | version dwords at the marker **+ 0x20** | **yes**, with DT emulation on — System Shock 2 (1.11.000), Carmageddon 2 (1.01.034), Max Payne (2.51.020) |
| SafeDisc 2.80+ | same, reading `2 / 0x50 / 0x0a` | **no** on this fleet — BF1942, C&C Generals |

The version dwords sit at **marker offset + 0x20**, not immediately after the
13-byte marker: the full magic is `BoG_ *90.0&!!  Yy>` followed by 14 zero
bytes. Reproduced against two knowns (BF1942 `Mod.dll` 2.80.010, `MaxPayne.exe`
2.51.020).

### Two traps found the same day, both of which fake a working mount

**A DAEMON Tools unit can be LOCKED, and the failure is nearly silent.** On
`.124` and `.240`, `daemon.exe -mount 0,"..."` answers with a modal reading
*"Unable to mount image. Unit is locked."* — `-unmount 0` says the same, and
neither `net stop d347bus` nor `d347prt` will stop (kernel drivers), so there is
**no reboot-free way to clear it that was found**. The modals then STACK: `.240`
was carrying three of them, and while one is up every later `daemon.exe` call
hangs. Because the launcher's fallback starts the game against whatever disc is
already parked in a drive, a locked unit presents as "the game ran, against the
wrong disc". `.143` was not locked and mounted first try, `rc=0`.

**A launcher's `VOLID` can silently never match.** Max Payne's shipped
`set "VOLID=Max Payne"` against an image whose real ISO9660 label is
`MAX_PAYNE`; `find /i` does a substring match, so it could never hit, and only
the MARKER fallback was saving it. `scripts/validate-staged-library.py` now
reads the label out of the image itself and fails the library on a mismatch —
it found this one on its first run.

## Soldier of Fortune 1 — the disc is NOT the gate (refuted 2026-08-31)

Worth stating separately because the staged tree asserted the opposite. A
SOF-labelled retail image was mounted on `.143` (DAEMON Tools, `E:` reading
`SOF`) and on `.246` (WinCDEmu, `D:` reading `SOF`), with no `mount-error.txt`
on either. **Both hosts still raised the game's own "WON Error! Please insert
the SOF CD and try again."** — the identical message `.123`, which has no
mounter at all, produces. Same failure with and without the disc means the disc
is not what the check wants, and the 795 MB image was removed from the library
again rather than shipped to seven boxes for nothing.

The message is not a literal in `SoF.exe`: it is a localised string in
`base/pak0.pak` under `REFERENCE NEED`, inside a block headed
`DESCRIPTION "Won error messages"` alongside "You disconnected" and "Server
Quit!". Next step is a disassembly of the `%c:\` drive-loop's caller, or the
official 1.07f patch — **not another image**.

## What genuinely still needs a person, and of what kind

| | what is actually needed |
|---|---|
| **BF1942 client**, **C&C Generals + Zero Hour** | a **user DECISION**: a SafeDisc-2.80-capable mounter means a third-party kernel driver and a reboot per box, and `.123`/`.133` are unactivated and must not be rebooted. No agent should install one unilaterally. |
| **Jedi Academy on `.124`/`.240`** | a **reboot** of those two boxes, to clear the locked DAEMON Tools unit. Everything else is staged and proven — it plays on `.143` and `.246`, and on a locked box the launcher now says so in `mount-error.txt` instead of hanging. |
| **Soldier of Fortune 1 multiplayer** | **engineering**, not media — see above. |
| **Far Cry, Carmageddon 1/2, AvP, Shogo, Descent 3, Deus Ex** | a **person at the keyboard**, once: their menus are driven by relative mouse deltas, which `UICLICK` cannot reach at all. |
| **Halo** | a person **at the HOST only**, to start the game from its menu. Every client is automated by `-connect`. Or put `haloded.exe` on the share and no person is needed at all. |
| **Hidden & Dangerous** | a person to create a profile once; then `Savegame\*.bin` is staged. |
| **Turok 2** | investigation: host and browse work, `+connect` answers "Unable to contact the GameManager." |
| **Red Faction** | investigation, with two untested leads recorded in its tree: `rf.exe` has **no `-connect`/`+connect` switch at all** (so "Add Server" is the only manual route) but it does have **`-trackerip`**, and the fleet's own favourites agent already records that RF LAN games are found by **broadcast** — a different control from the Get Servers / Add Server path everything has been tried on. Its dedicated server works and needs no disc. |
| **Redneck Rampage** | **not** the network CFG: that was captured through the game's own setup and md5s identical to the staged one, and `-net` has been A/B'd twice. `rr.exe` documents `/i# Network mode (1/0)` in its own switch list, which is untried. |


---

## Halo — the keys were the second problem, and they were silently wrong

**Measured 2026-09-01 with `scripts/halo/audit_keys.py`: every live box that has
`halo.exe` — `.145`, `.240`, `.246` — carried the SAME `DigitalProductID`**
(fingerprint `dc775ec92b`, byte-identical to the leftover blob on `.133` and
`.143`, which do not have Halo at all). Halo allows **one simultaneous player
per CD key**, so with that state exactly ONE box could ever have been in a Halo
game; the other two would have been thrown out with *"Your CD Key is invalid"* —
the same wording a genuinely bad key produces.

This is worth writing down because it had gone quiet. The keys were assigned
per box on 2026-08-31 and were distinct then. **The mechanism that undid it is
measured, not guessed: `Games-Library/Halo/install.reg` carries a literal
`DigitalProductID`**, and GAMESYNC merges that file byte-identically onto every
machine. So every Halo deploy silently restores one shared key across the whole
fleet, and the per-box assignment survives only until the next sync of this
title.

**That is the same class of bug as Red Alert 2's `Serial`,** which is generated
on the box by the launcher for exactly this reason — a per-INSTALLATION value
cannot live in `install.reg`. Halo's case is harder than RA2's, because a CD key
cannot be derived from the machine the way a volume serial can; it has to come
out of a pool. So this needs a decision rather than a patch:

* **strip `DigitalProductID` from `install.reg`** and let `assign_keys.py` own
  it — clean, but a freshly imaged box then has no key at all until somebody
  runs the assigner, and Halo will not start without one; or
* **keep it as the first-boot fallback** and make re-running `assign_keys.py`
  part of the deploy loop for this title, the way the RA2 launcher's serial
  block is part of its launch.

Whichever is chosen, **`audit_keys.py` should be run after any Halo GAMESYNC**,
because today the regression is completely silent: the game installs, launches,
reaches its menu and only fails at a *server's* key check, with wording that
blames the key rather than the duplication.

**Fixed the same day:**

```bash
python3 scripts/halo/audit_keys.py          # fingerprints, never keys
python3 scripts/halo/assign_keys.py --keys-file <file> \
        --boxes 192.168.1.240,192.168.1.246
```

`.240` and `.246` took `fleet-gamekey-halo-pc-3` / `-4` from the vault; `.145`
kept key 1. Confirmed by a direct `REGREAD` of the blob on all five boxes: three
distinct 491-byte `DigitalProductID` values on the three boxes that have the
game, and the old shared one only on the two that do not.

> **`audit_keys.py` and `assign_keys.py` fingerprint DIFFERENT things** — the
> assigner prints a hash of the KEY, the auditor a hash of the resulting DPID
> BLOB — so the two never agree and a reader comparing them concludes the write
> failed. It did not. Verify with `REGREAD`, which is the post-condition.

### Windowed does NOT unlock Halo's menu — refuted on hardware 2026-09-01

The obvious next idea, once `UIKEY` turned out to work in-game on id Tech 3
(above), is that Halo's menu might accept synthetic input **windowed** the way
SoF2's does. It does not.

On `.240`, `halo.exe -window -vidmode 1024,768,60` (through a `.bat` that `cd`s
to the tree — an absolute-path launch dies on `Cannot find 'C:\config.txt'`)
came up as an ordinary 1032x795 window titled `Halo`, main menu drawn,
`CAMPAIGN` highlighted. `UICLICK` on **MULTIPLAYER** and then `UIKEY DOWN` both
left the highlight exactly where it was.
Evidence: `~/lan-proof/halo-2026-09-01/halo_240_win_down.png`.

So Halo still needs `haloded.exe` or one person at one keyboard. The keys are no
longer the obstacle; the host is.

## StarCraft on `.123` — the blocker has a name

`.123` is the only box on the fleet with **no optical drive and no image
mounter**, and `StarCraft/requires.json` declares
`requires_capabilities: ["disc_mount"]` because `Play StarCraft.bat` mounts
`_disc\STARCRAFT.iso` with DAEMON Tools before starting the game — 1.16.1 keeps
a CD-presence check that a copy of the disc contents does not satisfy. So
StarCraft cannot start on `.123` at all, LAN or single player, and the fix is a
mounter — which is a kernel driver, which is a reboot, on a box that is
unactivated. **That is a user decision, not an agent's.**

The other four live boxes all mounted and reached the title screen fullscreen
640x480 on 2026-09-01 (`.143` `.145` `.240` `.246`). The **LAN gather was not
re-proved this session**: StarCraft's 8-bit DirectDraw surface captures with the
wrong palette, so the menu buttons are not legible in a screenshot and blind
clicking was not attempted. The existing `.246` + `.143` record stands
unchallenged — and unrepeated.

One new defect worth fixing: on `.145`, which has **four** DAEMON Tools virtual
drives (D E F G), the first launch raced its own mount and left a modal
`cmd.exe - No Disk: There is no disk in the drive. Please insert a disk into
drive E:` on the desktop, while `E:` did in fact end up holding the `STARCRAFT`
volume. The launcher's wait-for-drive loop is not tight enough on a box with
that many virtual units. The second launch was clean.

## `.123` went off the LAN twice mid-session — treat it as fragile

`.123` (`NSC-B20C188E96D`, Radeon HD 3850 AGP) dropped off the network at
**14:20:14** during a six-box Quake II run, came back at 14:25:08 with its
uptime reset, and went again at **14:26:58**. It **reboots** — ARP, ICMP and
every TCP port including 445/139 go away together, which is a box, not a dead
agent (a dead agent leaves SMB up).

What it was doing: it had run six consecutive exclusive-fullscreen,
mode-switching titles in about forty-five minutes — Quake III (1920x1080),
Quake II (1280x960), RTCW (1152x864), Red Alert 2 (1920x1080x**16 bpp**), then
Quake III and Quake II again — and the first drop landed on a `SCREENSHOT 0`
(a GDI `BitBlt` of the whole screen) taken of Quake II's fullscreen OpenGL
surface. **The second drop happened with no command sent to the box at all**,
which is the fact that matters: whatever this is, it is not driven by the test
harness. Nothing rebooting-class was ever issued there — no `REBOOT`, no
`SHUTDOWN`, no installer, no driver work, no `netsh`, no `DISPLAYCFG set`.

`.123` is one of the two boxes that **must never be rebooted** (unactivated
Windows). Windows survived both cycles. Until someone looks at it, do not put a
long fullscreen mode-switching sequence on that box unattended.


## Halo 2 (Vista) — why LIVE and NETWORK are greyed out, and the way in

**Not tested on hardware yet. This section is host-side research plus binary
analysis, and it says so.** Halo 2 is staged and its single player runs on
`.246` (Win7) and `.145` (XP); both its **LIVE** and **NETWORK** menu entries are
dimmed.

**The cause is documented by Microsoft.** KB 927007 (the 2007/2008 revision —
the 2015 snapshot has the sentence deleted) states verbatim: *"For Halo 2 for
Windows Vista, the LIVE and NETWORK game options will be unavailable (dimmed)"*
when the product is **not activated**. Consistent with the hardware:
`HKLM\SOFTWARE\Microsoft\Microsoft Games\Halo 2\1.0` has `GameInstallDir` and
**no `DigitalPID2`** on `.123`, `.145` and `.246`. It is not a sign-in problem —
a real GFWL profile exists on both working boxes and NETWORK is grey anyway.

**Halo 2 does have a real LAN mode, separate from LIVE**, and the retail disc
ships a dedicated server for it: `Dedicated Server/Halo 2 Dedicated Server.msi`
(984,064 B) inside `Halo2.iso`. Its own readme splits the requirements:

> If you want to run a **LAN** server, you need: … CPU / RAM / disk / **Network
> card** / Windows Vista or Windows 2003 Server.
> If you want to run a **LIVE** server, you **also** need: … a Silver or Gold
> Games for Windows – LIVE account. **A Halo 2 for Windows Vista product key.**

**The key is listed only under LIVE**, and the binary agrees: `h2server.exe`
(11,056,504 B) imports **nothing** from `sldl_dll`, `sldlext` or `PCCompat` —
the licensing layer exists only in `halo2.exe`, which imports seven functions
from `sldl_dll` and eight from `PCCompat`. `h2server.exe` is a **console** app
with **SubsystemVersion 4.00**, so XP's loader accepts it, and the installer's
`Setup.ini` lists `WinXP=1` even though the readme says Vista. Switches in its
string table: `-lan -live -createservice -deleteservice -highquality -nosound
-novsync -windowed`. Default port **UDP 23056**. Its `h2server.exe.cfg` and the
staged `halo2.exe.cfg` already carry the **same `titleid` and the same
`lankey`**, so client and server are matched out of the box — do not edit
either.

**The honest limit of that finding:** the dimming is a *client-local* decision
taken when the menu is built, and the LAN browser sits *behind* the NETWORK
entry, so standing up a server almost certainly will not un-grey it. Two things
to try, cheapest first, and the first is the one that can kill the plan:

1. **Install the current official GFWL on `.246` and relaunch.** All three boxes
   run GFWL **1.0.6027** (`xlive.dll` 7,677,744 B) — the on-disc build
   PCGamingWiki calls broken. Microsoft's CDN still serves
   `xliveredist.msi` (21,598,208 B), which installs **3.5.0092.0** (`xlive`
   payload 15,453,832 B) and still declares XP SP2 support. **NETWORK coming
   back while LIVE stays grey is the win condition.** Snapshot the existing
   `system32\xlive.dll` first — Halo 2's single player currently works and a
   2011 client replacing a 2007 one could regress it.
2. If NETWORK is still grey the activation gate is real, and the route is the
   two genuine keys in the vault (`fleet-gamekey-halo-2`, `-2-2`) through the
   in-game GFWL guide — with the caveat that GFWL activation servers were
   degraded in 2025 and their 2026 state is unknown, which would be a legitimate
   dead end rather than a failure.

**Boxes: `.145` and `.246` only.** `.123` loads Halo 2 and renders a black
window, and it is now unstable besides.
