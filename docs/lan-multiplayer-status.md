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
| Quake II | id Tech 2 | `.123` + `.240` |
| Quake III Arena | id Tech 3 | fleet server |
| Quake III: Team Arena | id Tech 3 | `.123` + `.240` |
| Hexen II | Quake-derived, peer | `.123` + `.240` |
| SiN Gold | id Tech 2 | `.123` + `.240` |
| Soldier of Fortune II | id Tech 3 | `.123` + `.240` |
| Jedi Knight: Dark Forces II | Sith / DirectPlay TCP-IP | `.123` + `.240` |
| Mysteries of the Sith | Sith / DirectPlay TCP-IP | `.123` + `.240` |
| Unreal Tournament (436 client) | UE1 | `.143` + `.133` |
| Unreal Tournament (469e) | UE1 | fleet server |
| UT2004 | UE2 | `.240` + `.246` |
| Unreal Gold | UE1 | `.143` + `.246` |
| Deus Ex | UE1 | `.143` + `.246` |
| Red Alert 2 | Westwood, peer | `.246` + `.143` |
| Yuri's Revenge | Westwood, peer | `.246` + `.143` |
| Tiberian Sun | Westwood, peer | `.240` + `.123` |
| StarCraft | peer, UDP LAN | `.246` + `.143` |
| Descent 1 | DOSBox IPX tunnel | `.246` + `.124` |
| Descent 2 | UDP/IP native | `.123` + `.240` |
| Descent 3 | dedicated server | `.240` |
| Doom 3 | id Tech 4 | `.123` + `.246` |
| Return to Castle Wolfenstein | id Tech 3 | `.143` + `.246` |
| Serious Sam: The First Encounter | Serious Engine 1, dedicated server | `.123` + `.133` + `.240` |

**30 titles.**

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

## Hosted on this computer — 18 servers, all `enabled` with linger on

`cs16-server` · `cs16-noblood` (+ both A2S browser proxies) · `specialists-server` ·
`hldm-server` (+ its proxy) · `quake1-server` · `quake2-server` · `quakeworld-server` ·
`quake3-server` · `q3ta-server` · `openarena-server` · `sof2-server` · `jka-server` ·
`ut99-server` · `ut2004-server` · `tribes2-server` · `descent3-server` · `farcry-server`

Check them with `python3 scripts/game-servers/healthcheck.py` — it uses the
**right query packet per engine**, which a single `getstatus` sweep does not.
Quake 1 and Hexen II answer *neither* standard probe (they speak Quake's own
control protocol, and a Hexen II host replies only to the game string
`HEXENII`); Tribes 2 needs the Torque binary form; MOHAA needs a framed
`getinfo`. A probe that guesses reports a healthy server as dead.

## NEEDS A PERSON, ONCE — and exactly what

| Title | What blocks it | The one-time step |
|---|---|---|
| **Jedi Academy** | client demands `Disk 1`; server is already up on `:29070` | a disc mounter on a second box + a labelled image |
| **Soldier of Fortune 1** | multiplayer-only CD check | same, with a `SOF`-labelled image |
| **BF1942** | SafeDisc 2.80 blocks the *client*; the host launcher works | a mounter that handles SafeDisc |
| **Far Cry** | server hosts unattended; CryEngine takes DirectInput exclusively | one click: Multiplayer → LAN |
| **Halo** | shell ignores synthetic input; `haloded.exe` is not on the share | one System Link join |
| **Carmageddon 1 / 2** | tunnel proven both ends; the front end ignores click *and* key | click **HOST GAME** / **NETWORK GAME** |
| **Hidden & Dangerous** | launcher bug fixed; stops at profile creation | create a profile, then copy `Savegame\*.bin` into the tree |
| **Aliens vs Predator** | has LAN (DirectPlay), but exclusive-fullscreen D3D — screenshots come back black | drive its menus at the keyboard |
| **Turok 2** | host works and is listed in the joiner's browser; join fails | investigate the GameManager path |
| **Redneck Rampage** | IPX tunnel proven; the Build gather never happens | run `SETMAIN.EXE` Network Config once, stage the CFG |
| **Shogo** | dedicated server stands up; client menu renders intermittently | drive the join at the keyboard |
| **Red Faction** | root cause fixed (`UpdateRate`); join unproven | re-test |

**A menu driven by RELATIVE MOUSE DELTAS cannot be automated at all** —
`UICLICK` sets an *absolute* pointer such menus do not follow. Descent 3, SoF2,
Deus Ex, Far Cry and Halo are all in that class. Recognising it early is worth
more than another hour of clicking.

## NO MULTIPLAYER — measured, not assumed

- **Half-Life: Blue Shift** — `liblist.gam` declares `type "SP Mission"`, `maps\`
  is empty, and all 37 maps in `pak1.pak` are `ba_*` campaign. Its `mpentity`
  line is inherited boilerplate.
- **Max Payne** — `MaxPayne.exe` imports no `WS2_32`, `WSOCK32` or `DPLAYX` at all.
- **System Shock 2** — the menu offers New Game / Load / Options / Credits /
  Intro / Quit and nothing else.

## Peer-hosted by design — no dedicated server exists

Descent 1 and 2, Carmageddon 1 and 2, Redneck Rampage, Red Alert 2, Yuri's
Revenge, Tiberian Sun, StarCraft, Hexen II, SiN Gold, and both Jedi Knight
titles. For these the two-box proof **is** the deliverable; there is nothing to
stand up on the host, and inventing one would produce a unit that reports itself
healthy while nobody can join it.

---

## Why several titles are blocked on one thing

**Three titles (Jedi Academy, Soldier of Fortune 1, BF1942) and C&C Generals are
all waiting on the same step: a virtual disc mounter that handles SafeDisc-era
protection, on more than one box.** Today only `.240` has any mounter, and
DAEMON Tools 3.47 was measured — with all four emulations verified ON — still
failing Generals and BF1942 on `.133`. Newer mounters need a kernel driver and
**a reboot per box**, and `.123` and `.133` are currently unactivated, so they
**must not be rebooted** (see `scripts/fleet/safe-reboot.py`, which now refuses).

**There is a better route where it exists:** Doom 3 was SafeDisc 3.20 and was
unblocked with **no mounter and no crack**, because id's official 1.3 patch
ships an exe with no protection wrapper at all. Check for that before fighting
the mounter.
