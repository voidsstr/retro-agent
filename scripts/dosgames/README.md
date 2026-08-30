# DOS Game Manager (`DOSGAME.EXE`) — fleet DOS games from one menu

A 16-bit real-mode DOS TUI for the fleet's DOS-capable boxes (Win98 DOS 7.1,
the Deskpro 2000, DOSBox): scan the hard drive for installed games, browse the
share's ~3,000-title DOS catalog with typeahead search, install games with
scripted (non-interactive) steps over the LAN, and show per-game gameplay
preview tiles in VGA mode 13h.

## Components

| File | Runs on | Purpose |
|---|---|---|
| `dosgame.c` → `DOSGAME.EXE` | DOS 8086+ | The TUI menu (Open Watcom, large model) |
| `DOSGAME.BAT` | DOS | Wrapper loop: menu writes `RUN.BAT`, exits 42, batch runs it with all conventional memory free, loops back |
| `dosbox_run.sh` | dev host | Headless DOS test loop: DOSBox-X (mingw build) under Wine on private Xvfb `:77` |
| `survey_share.py` | dev host | Reads every share zip's central directory (no downloads), classifies install patterns |
| `gen_catalog.py` | dev host | Survey JSON → `GAMES.CAT` (`title\|zip\|kind\|exe\|size\|tile`) |
| `serve_dosgames.py` | dev host | HTTP bridge (default :8181): `/GAMES.CAT`, `/dos/<zip>` (from the SMB share), `/tiles/<prv>` |
| `retro-dosgames-http.service` | dev host | systemd **user** unit for the bridge — `systemctl --user enable --now retro-dosgames-http` (copy to `~/.config/systemd/user/`) |
| `gen_tiles.py` | dev host | Auto-renders games in DOSBox-X, saves 320x200x256 `.PRV` tiles |

## Install patterns (from the 3,795-archive survey, 2026-07-28)

- 2,893 zips are flat-root; ready-to-run main exe at depth 0 in 1,683.
- Installer names are near-universal: `INSTALL.EXE` (963), `SETUP.EXE` (523),
  `INSTALL.BAT` (341) — at the zip root in 96% of installer archives.
  `SETSOUND.EXE` is sound config, not install.
- So the scripted install is: fetch zip → `UNZIP -qq -o` into
  `C:\GAMES\<stem8>` → (kind `I` + F9 only) run `INSTALL.EXE`/`SETUP.EXE`.
- CD-image titles (iso/bin+cue) and rar/7z need host-side prep; they're
  cataloged as kind `C` and not auto-installable yet.

## Installing it on a real box (one double-click)

`…\Retro Automation\dos-setup\` on the share holds a ready bundle: run
**`SETUPDOS.BAT`** from Windows and it copies ~870KB into place (`C:\DOSGAME`,
`C:\DOSCHAT`, `PLAY.BAT`/`CHAT.BAT`/`DOSGAME.BAT` at the root). Then reboot to
MS-DOS mode and type **`PLAY`** — that brings up the network and opens the menu.

`PLAY.BAT` auto-detects the network card by trying each packet driver **on its
own interrupt**: a Crynwr driver that fails to find its card still disturbs the
vector it was handed, and a later driver loading on that same vector comes up
half-broken (DHCP then just times out with no clue why). Order matters too —
3C509 and NE2000 detect cleanly, while the ancient 3C50x drivers probe hard
enough to claim a card that isn't theirs, so they go last. Whichever wins, its
interrupt is written into `MTCP.CFG` for DHCP/HTGET.

## On-box layout (deployed to `…\Retro Automation\dosgame\` on the share)

```
C:\DOSGAME\DOSGAME.EXE      the menu (run via C:\DOSGAME.BAT)
C:\DOSGAME\DOSGAME.CFG      gamedir=C:\GAMES + url=http://<devhost>:8181/dos
C:\DOSGAME\GAMES.CAT        catalog (refresh via HTGET from the server)
C:\DOSGAME\UNZIP.EXE        Info-ZIP (FreeDOS build, 386+/DPMI)
C:\DOSGAME\NET\NE2000.COM   packet driver (swap per NIC)
C:\DOSGAME\NET\DHCP.EXE     mTCP DHCP
C:\DOSGAME\NET\HTGET.EXE    mTCP HTTP fetch (handles long names in URLs —
                            SMB drive-mode copy can't: 8.3 mangling)
C:\DOSGAME\NET\MTCP.CFG     mTCP config (SET MTCPCFG=C:\DOSGAME\NET\MTCP.CFG)
C:\DOSGAME\TILES\*.PRV      preview tiles (768-byte pal + 64000 px)
```

AUTOEXEC lines for a networked box:
```
SET MTCPCFG=C:\DOSGAME\NET\MTCP.CFG
LH C:\DOSGAME\NET\NE2000.COM 0x60 <irq> <iobase>
C:\DOSGAME\NET\DHCP
```

## Dev-host test loop

```
make                                   # Open Watcom (toolchain-dos/watcom)
bash dosbox_run.sh <Croot> "<cmd>"     # headless DOSBox-X run
DOSGAME.EXE /selftest                  # writes DGSELF.TXT (scan+catalog dump)
```

Verified end-to-end in DOSBox-X (2026-07-28): drive scan → menu → launch via
RUN.BAT swap; typeahead search over 2,982 catalog entries; **full LAN install**
(NE2000+slirp: packet driver → DHCP → HTGET zip from serve_dosgames.py →
UNZIP → playable dir); tile rendering pipeline.

## The registry: how "installed" is actually known (v0.2)

Up to v0.1 the menu re-derived *what is installed and how do I run it* on every
start, purely by scanning the disk. That inference is lossy, and it broke the
program's whole purpose on real hardware: install a game whose `INSTALL.EXE`
copies the files into **its own** directory (`C:\WOLF3D`) rather than the
`C:\GAMES\<stem>` we unpacked into, and the menu would list only the leftover
unpack directory — whose one executable is `INSTALL.EXE` — as *"run setup"*.
Pressing Enter re-ran the installer, forever, and the game was never offered
as playable.

So an install now **records what it produced**, in `C:\DOSGAME\INSTALL.LST`:

```
G|<title>|<dir>|<exe>     a playable game: run <exe> in <dir>
X|<title>|<dir>|          a spent unpack dir: hide it from the menu
```

The record is written by a **post-install pass** (`DOSGAME /postinst`) that the
generated `RUN.BAT` invokes after the installer. It finds the game by
difference: `DOSGAME /snapdirs` lists the top-level directories of every scan
root *before* the installer runs, and the post pass compares that snapshot with
what is there afterwards. Whatever appeared is where the game went. It prefers
the launcher the catalogue named (`exe`, field 4) over the scan's
"first `.EXE` in directory order" guess — which happily picks `SETSOUND.EXE`.

`/postinst` also answers a question batch cannot: *did anything runnable come
out of this?* It exits non-zero when not, and `RUN.BAT` branches on that to
tell the user. (`if exist DIR\*.*` is **true for an empty directory**, so a
corrupt download used to be indistinguishable from a good one.)

The disk scan still runs, for games that predate this program; the registry
simply wins where both have an opinion, and stale rows (directory or launcher
gone) are dropped on load.

## `DOSGAME.TXT` — a STAGED title declares its own real-DOS launcher (v0.3)

The fleet's staged library (`\\192.168.1.122\files\Files\Games-Library`) is
deployed by the agent's `GAMESYNC` into **`C:\Games\<Title>`** — which is
already one of this program's default scan roots (`scan=C:\GAMES;C:\`). So a
staged game is in front of the DOS menu the moment it lands. What was missing is
**which file to start**, and for a staged tree `pick_launcher()` cannot work it
out, because those trees are built for **Windows**: each carries a DOSBox of its
own, several `Play <Game>.bat` wrappers that start it, and 32-bit Windows
binaries beside the DOS ones.

Measured in DOSBox against the real file lists
(`tests/test_pick_outcomes.sh`, fixtures `QUAKE1` and `DESCENT1`):

| directory | the guess picks | what that is in real DOS |
|---|---|---|
| `C:\GAMES\QUAKE1` | `GLQUAKE.EXE` | a Win32 PE — *"This program cannot be run in DOS mode"* |
| `C:\GAMES\DESCENT1` | `DESCENT1.BAT` | a **cmd.exe** batch, opening with `cd /d` — a switch `COMMAND.COM` does not have |

Neither is a bug in the heuristic: no ranking of 8.3 names can know which of two
real executables is the DOS one. **So the tree says it**, in one line, in the
same shape as the library's own `launch.txt`:

```
DESCENTR.EXE<TAB>Descent
# field 1  the launcher, 8.3, in THIS directory (required)
# field 2  the title to show in the menu (optional)
# '#'/';' comments and blanks ignored; the FIRST data line wins.
```

**The file's own name is the constraint.** Real DOS sees 8.3 only, so
`dosnative.txt` would arrive as `DOSNAT~1.TXT` — a mangled name that depends on
what else is in the directory. `DOSGAME.TXT` is 7.3 and is the same string on
every box.

- **Precedence:** the registry (`INSTALL.LST`, which includes an operator's F2
  override) still wins — `scan_game_dir` returns on `reg_covers_dir` before this
  is read. Then the declaration. Then the guess.
- **A declaration naming a file that is not in the directory is NOT honoured**,
  and says so in `DOSGAME.LOG`. A tree that was gated out, or copied short, must
  degrade to the guess rather than to a launcher that cannot start.
- **A declared title leaves `game_t.dir` empty**, the same way a registry row
  does, so the catalogue's fuzzy name match cannot overwrite it with a near
  miss. `C:\GAMES\DESCENT1` titled "Descent" must not become whichever Descent
  row the catalogue scored highest.

**Why this matters at all:** DOSBox needs roughly a gigahertz of host CPU to
emulate a 486, so on the fleet's genuine Pentium 1 (`.243`, a 1997 Compaq
Deskpro 2000 on Windows 98 SE) the capability gate correctly refuses every
DOSBox shortcut — while the binaries those emulators are running are **native
to that machine**. Descent 1's own `DESCENT.FAQ`, staged in its tree, puts the
requirement at *"486 or Pentium processor, 8 MB RAM"*. This is the path by which
that box gets to play it.

Staged and re-staged by **`scripts/fleet/stage-dosnative.py`**, which refuses to
declare a launcher whose PE header says it is a Windows binary — the check is on
the image, not the file name, because `QUAKE.EXE` and `GLQUAKE.EXE` sit in the
same directory. `scripts/validate-staged-library.py` enforces the same rules on
the share; `tests/python/test_dosnative.py` covers both.

## The diagnostic log — `C:\DOSGAME\DOSGAME.LOG`

This program runs where nothing can watch it: a real-mode DOS session with the
screen taken over, on a box that was rebooted out of Windows to use it. When
something goes wrong there the only evidence anyone can collect afterwards is a
file, so **every decision is logged**, and the generated `RUN.BAT` appends to
the *same* file — so the batch half of the story lines up with the program half:

```
17:19:02 menu     14  install: "Wolfenstein 3D" kind=I zip=WOLF3D.ZIP
17:19:02 menu     15  install: stem=WOLF39NA dir=C:\GAMES\WOLF39NA exe(catalog)=WOLF3D.EXE
17:19:02 menu     16  install: fetch command tail is 65 bytes (DOS truncates above 126)
run:    fetching the archive with HTGET
run:    archive downloaded; unpacking
17:19:02 snap      6  snap:   recorded 4 directories before the installer runs
run:    running the game's own installer
17:19:02 post      7  pick:   C:\GAMES\WOLF39NA -> INSTALL.EXE (installer only; needs setup run)
17:19:02 post      8  post:   nothing runnable in the unpack dir itself
17:19:02 post      9  post:   installer created C:\WOLF3D - checking it
17:19:02 post     10  post:   it has the catalog's launcher WOLF3D.EXE
17:19:02 post     11  post:   OK - "Wolfenstein 3D" is playable: C:\WOLF3D\WOLF3D.EXE
17:19:02 post     12  registry: RECORD G "Wolfenstein 3D" dir=C:\WOLF3D exe=WOLF3D.EXE
run:    install finished OK
```

What it captures: the config actually loaded; every registry row and why a
stale one was dropped; every directory the scan considered and *why* it was
skipped (system dir, scan root, registry-owned, path too long, nothing
runnable); which launcher was picked and on what evidence; catalog rows loaded
vs. total; the install decision with its stem, target directory and
command-tail budget; each batch step with `HTGET`/`UNZIP` output and
errorlevels redirected in; and the full post-install reconciliation.

- Each line is **flushed immediately** — a log still sitting in a buffer when
  the machine wedges tells you nothing, and wedging is the case worth
  diagnosing.
- The tag column (`menu` / `snap` / `post`) identifies which process wrote the
  line; the helper passes are separate program runs appending to one file.
- Timestamps are time-of-day only plus a sequence number: these boxes have no
  reliable date (this one reports 1980).
- The file is recycled past 256 KB rather than filling a 1.2 GB disk.
- Turn it off with `log=0` in `DOSGAME.CFG`; move it with `logfile=`.

### Headless modes (also how it is tested)

| Command | Does |
|---|---|
| `DOSGAME /selftest` | dump the scan+catalog state to `DGSELF.TXT`, no video |
| `DOSGAME /play:<substr>` | write a launch script for the first matching installed game, exit 42 |
| `DOSGAME /install:<substr>` | write an install script for the first matching catalog game, exit 42 |
| `DOSGAME /snapdirs` | snapshot directories (used inside `RUN.BAT`) |
| `DOSGAME /postinst` | reconcile + record (used inside `RUN.BAT`); non-zero = nothing runnable |

Create `C:\DOSGAME\QUIET.FLG` to make generated scripts skip their
"press a key" prompts — that is what lets the whole install→play path run
unattended in `tests/run_dos_tests.sh`.

## Two ways into real-mode DOS, and they run DIFFERENT files

A Win98 box has two routes, and knowing which one you took is half of any
diagnosis:

| Route | Runs | Starts Windows first? |
|---|---|---|
| Shut Down -> **"Restart in MS-DOS mode"** | `C:\WINDOWS\DOSSTART.BAT` | yes - then shuts it down |
| Hold **CTRL/F8** at boot -> **"Command prompt only"** | `C:\AUTOEXEC.BAT` | **no** |

**`DOSSTART.BAT` is the MS-DOS mode file, NOT `AUTOEXEC.BAT`.** A box can be
perfectly set up at the boot prompt and completely bare in MS-DOS mode. Both
are staged/templated here: `DOSSTART.BAT` is ours and the agent stages it
(v1.30.0+); `AUTOEXEC.TPL` is a **template only** - the agent never writes a
box's own `AUTOEXEC.BAT`, because that file carries ITS drivers (CD-ROM,
mouse, sound) and staging over it would silently disarm hardware.

Route 2 is the fallback worth knowing: it never starts Windows, so it cannot
be broken by anything in the Windows-to-DOS transition - no video mode to
restore, no redirector to tear down, no driver to quiesce. It is the same code
path the box survives on every boot. It was useless here until `AUTOEXEC.BAT`
learned to set `PATH` and print the `PLAY` hint, because it landed on a bare
`C:\>` that looked like the same failure.

**Both files write a marker to `C:\DOSGAME\DOSGAME.LOG` as their first
action, and that is the point.** "Restart in MS-DOS mode" stuck at a bare
cursor on .243 and left *nothing* behind: nothing on the box logged anything
between the Shut Down dialog and the operator typing `PLAY`. Now:

- `dosstart: ---- reached MS-DOS mode ----` present -> DOS came up; the hang is
  after that, or the screen never came back to text mode.
- marker **absent** -> the machine never got out of the Windows shutdown, and
  nothing in DOS is involved at all.

## Hard-won gotchas

- **Every DOS batch file we ship MUST be CRLF, and git must be told so.**
  `COMMAND.COM` does not reliably parse an LF-only `.BAT`: on .243 one answered
  `Bad command or file name`, and with `@echo off` as its first line it printed
  `OFF` and stopped. These files had been **LF in git the whole time** and
  worked on the fleet only because somebody had once published them from a
  Windows machine, which converted them by accident - the share's `PLAY.BAT`
  was 2,274 bytes against git's 2,217, exactly one CR per line, and nothing
  recorded why. `retro_upload` and `copy` are byte-for-byte, so publishing
  straight from the Linux dev host would have put the LF versions on every DOS
  box and broken `PLAY`, `NETUP` and `DOSSTART` in one go. Pinned by
  `.gitattributes` (`*.BAT text eol=crlf`) and asserted by `run_dos_tests.sh`.
- **⚠️ THE SHARE'S `DOSGAME.EXE` IS NOT THIS REPO'S BUILD, AND ITS SOURCE IS
  LOST (found 2026-08-30).** `git HEAD` rebuilds byte-exactly to **111,170 B**;
  the share has carried **113,012 B** since 2026-08-26. The extra 1,842 bytes
  are real work — four log strings that appear in **no commit, on no branch, in
  no worktree and in no file on this host**:

      pick:   %s is a self-extracting archive, not the game
      pick:   %s -> %s (self-extracting archive; needs setup run)
      pick:   %s -> %s (skip-listed, but it is the only thing that runs here)
      registry: DROP %s - launcher "%s" is a self-extracting archive, not the
                game; re-deriving

  i.e. a launcher-choice refinement plus a registry-repair rule, built,
  published to the fleet, and never committed. Searched exhaustively:
  `git log --all -S` over the whole history with no path filter, `git grep`
  across every reachable commit, and every `dosgame.c` on the host.

  **So `make` + `copy` over the share DELETES that work permanently.** The
  2026-08-30 `DOSGAME.TXT` support was therefore committed and *not* published:
  publishing is a trade — a staged-library fix for a shareware-install fix —
  and a person has to make it. `python3 check-published.py` (also
  `make check-published`, and printed by `tests/run_dos_tests.sh`) says whether
  the fleet is running what this repo builds. It reports and never fails,
  because a check that failed the suite today would train everyone to ignore
  it; switch it to `--strict` the day this is resolved.

  The lesson is the same one the CRLF entry below teaches from the other
  direction: **the share and the repo drift in BOTH directions, and neither
  notices.** Compare before you publish, and publish from a build you can
  reproduce.

- **The share can be STALE relative to this repo, silently.** After the CRLF
  conversion five of the six shipped batch files were byte-identical to the
  copies running on .243 - and `NETUP.BAT` was not. The share is still carrying
  the pre-`93ecdbc` version, which writes `echo PACKETINT 0x60 > MTCP.CFG` with
  a space before the `>` and therefore a **trailing space** in every value it
  writes. Comparing sizes against the share after a conversion like this is a
  cheap way to find out what else never got published.

- **Never index a parallel array by a `games[]` index.** `scan_local()` moves
  rows: it OVERWRITES one (`games[j] = games[i]`, when a playable copy replaces
  a run-setup stub of the same 8.3 name) and MEMMOVEs the tail down when it
  drops a duplicate. The first cut of the title resolver kept each folder name
  in an array indexed alongside `games[]` and filled it *during* the scan, so
  after five replacements on .243 (`scan=C:\GAMES;C:\`) every key sat against
  the wrong row. On that box the result was that nothing at all got a title -
  which reads as "the feature silently does nothing" - but on a fixture with
  the same shape it did something worse and titled `C:\STARCR~1` **"Doom"**. A
  per-row fact belongs IN the row (`game_t.dir`); anything genuinely parallel
  must be built AFTER the scan has finished moving things (`title_begin()`).

- **An Apogee/id DEICE set must be entered through its own `INSTALL.BAT`.**
  `keen1_shareware.zip` is `DEICE.EXE` + `KEEN.1` + `KEEN.DAT` + `INSTALL.BAT`,
  and the scan kept whichever installer-shaped file DOS returned *first* -
  `DEICE.EXE`. DEICE on its own only rebuilds the packed self-extractor and
  stops; the vendor's batch is what carries the install to the end:

  ```
  @ECHO OFF
  DEICE                      <- rebuilds KEEN.EXE into \KEEN
  IF ERRORLEVEL == 1 GOTO END
  KEEN.EXE                   <- self-extracts the actual game  <-- never ran
  DEL KEEN.EXE
  ```

  So the install produced exactly one file, `/postinst` called that "too few to
  be an install", and Commander Keen 1 could never be played. `setup_exes[]` is
  now a **preference order** (`INSTALL.EXE` > `SETUP.EXE` > `INSTALL.BAT` >
  `SETUP.BAT` > `DEICE.EXE`), not a membership test - whenever `DEICE.EXE` is
  present, something else in the directory knows how to drive it.
- **Some sets on the share are only disk 1.** `heretic_shareware1.zip` is
  1,439,232 bytes against the `SIZE=2863638` its own `HTIC_V10.DAT` declares -
  disk 2 is not in the archive at all, so the installer stops at an "insert
  disk 2" prompt that no answer can satisfy. `deice_short()` compares the
  `.DAT`'s `SIZE=` with the `NAME.<n>` parts actually present and refuses the
  launch up front, naming the shortfall. (The complete set is on the share as
  *Heretic Shadow Of The Serpent Riders*, 2.88 MB.) Match `SIZE=` at the START
  of the line - `EXPSIZE=` is the *unpacked* size and would call every complete
  set short.
- **Disk-set parts are numbered in the EXTENSION**: `KEEN.1`, `HTIC_V10.1`,
  `HTIC_V10.2`. Only `NAME._1` was recognised, so the commoner shape counted as
  zero disks and a stalled multi-disk install was reported to the operator as
  *"the installer wrote nothing at all (cancelled, or the download is bad)"* -
  with every byte of the game sitting in the directory.
- **Never take a step's verdict from `ERRORLEVEL` in a generated script.** A
  LAN install on .243 that succeeded end to end - zip fetched, unpacked,
  `/postinst` recorded the game - still logged `HTGET failed - is ... serving?`
  and `UNZIP failed - corrupt zip, disk full, or no DPMI` immediately above
  `install finished OK`, which reads as a broken network install and sends
  anyone reading the log after the LAN. COMMAND.COM keeps the last value
  anything set, and `DOSGAME.EXE` itself exits **42** to hand control to
  `RUN.BAT`, so a tool that terminates without setting a return code leaves 42
  standing and every `if errorlevel 1` below it fires. Test the artifact
  (`if not exist <zip> ...`) instead. `/postinst`'s exit code is a real
  verdict the exe sets deliberately, and is still branched on.
- **A game found by the disk scan is only known by its DIRECTORY.** That put
  `KEEN1`, `STARCR~1` and `JAGGED~1` on the Installed tab while the Available
  tab beside it listed the same games as *keen1 shareware* and *StarCraft*.
  `title_try()` now scores every catalogue row against the unresolved folders
  during `load_catalog`'s existing pass (so it costs no extra file I/O), on
  squashed-name shape plus the launcher, and takes only an **unambiguous** best
  match - a tie keeps the folder name, because `C:\HEXEN` could be any of half
  a dozen Hexen rows and a confidently wrong name is worse than a dull correct
  one.

- **Games are rarely all in one folder.** `scan=` takes a semicolon-separated
  list and defaults to `C:\GAMES;C:\`, because a real box keeps games at the
  drive root (`C:\DOOM`, `C:\ROTT`, …). Scanning the root means excluding the
  system folders by name, and joining paths without doubling the separator —
  `C:\` + `DOOM` must not become `C:\\DOOM`, which breaks the launch batch.
- **A game folder is often not "installed" yet.** Real boxes are full of
  half-unpacked downloads, so the Installed tab classifies what it finds:
  `play` (a runnable exe, preferring one named after its folder — TYRIAN ships
  `TYRIAN.BAT` next to `UNZIP32.EXE`), `run setup` (only a self-extractor like
  `DEICE.EXE` + packed data — the Apogee/id shareware layout), or
  `unpack + setup` (only a `.ZIP`). Archive and system tools
  (`UNZIP32.EXE`, `PKUNZJR.COM`, `CACHE.COM`, `CWSDPMI.EXE`) are never picked
  as the game, and a scan root is never listed as a game of its own —
  `C:\GAMES` showed up as a game called "CACHE.COM" before that.
- **Enter runs the installer for installer-type archives.** Most of the
  catalogue is `INSTALL.EXE` + a packed payload; extracting and stopping there
  leaves a directory the menu won't even list as a game.
- **DOS silently truncates a command tail at 126 bytes.** Measured, not
  assumed: batch lines of 160, 200 and 215 characters all delivered exactly
  126 bytes of arguments in DOSBox — no error, no warning. The install script
  used to paste the URL-encoded zip name (61 chars on average, **137** at
  worst) onto the server URL, so **845 of the 2,982 catalogue entries** fetched
  a chopped-off URL, 404'd, and reported *"Download failed — check the
  network"*, sending everyone hunting a networking problem that did not exist.
  Fixed by fetching `/z/<STEM>` — a fixed-length name — and the client refuses
  to write a script whose command line cannot fit.
- **An 8-character truncation is not a unique directory name.** The old install
  stem collided for **1,268 of 2,982** catalogue rows: all eleven Duke Nukem
  titles installed into `C:\GAMES\DUKE_NUK`, unzipping over each other, and
  installing any one of them marked the other ten as installed. The stem is now
  5 readable characters + 3 base-36 characters of a 16-bit FNV-1a hash of the
  full name — zero collisions across the catalogue. **`serve_dosgames.py`
  computes the identical stem** to resolve `/z/<STEM>`; change one and you must
  change the other (`tests/run_dos_tests.sh` diffs the two implementations).
- **`if exist DIR\*.*` is TRUE for an empty directory** (`.` and `..` match), so
  batch cannot tell "unzipped nothing" from "unzipped fine". `if not exist
  DIR\nul` *does* correctly distinguish a missing directory. Judgements about
  content belong in the exe, reported back through `errorlevel`.
- **A `.BAT` launcher must be `CALL`ed.** Chaining to one (a bare `TYRIAN.BAT`)
  silently abandons the rest of `RUN.BAT` — verified in DOSBox: the line after
  a bare `.BAT` never runs, the line after `call GAME.BAT` does. The outer
  menu loop does still resume, so it looks harmless until you add post-launch
  bookkeeping.
- **Watcom's default DOS stack is 2K.** The scan call chain
  (`main → scan_local → scan_root → scan_game_dir → pick_launcher`, each with
  its own `MAX_PATH_L` buffers and a `find_t`) plus Watcom's `sprintf` came
  close enough to that to be a hazard, and a blown stack on a real box is a
  hung machine. Build with `-k8192`.
- **Roughly a quarter of the share's archives are not flat-root**, so the game
  sits one directory below where UNZIP put it. The scan only ever looked at
  depth 1, so those games were listed *nowhere at all*. It now descends one
  level when the top level has nothing runnable.
- **A >64K static array silently wraps the data segment** in the large model
  (Watcom, no warning) — entries past ~#420 came back corrupted. `games[]`
  must stay well under 64K (MAX_GAMES 256 + disk-backed typeahead filter).
- **Share zip names are long filenames** — DOS sees `KEEN4_~1.ZIP`, so a
  drive-letter copy by catalog name fails. HTTP fetch (HTGET) is the primary
  install path; `drive=` mode only works for 8.3-named archives.
- **dosbox-staging's Linux build hard-requires GLX** (absent on Xvfb/Xvnc) —
  that's why the loop runs the DOSBox-X mingw build under the repo Wine.
- DJGPP-built UNZIP needs a 386+ (fine for the fleet; not a real 286).
- DOSBox-X AUTOTYPE: `enter`/`tab` and plain chars deliver; `esc`/function-key
  names do not — end automated runs by timeout + file assertions instead.
- `allow_reuse_address` must be set on the **server class**, not the instance:
  `ThreadingTCPServer.__init__` binds immediately, so an instance assignment
  comes too late and a restart dies on "Address already in use" while the old
  socket sits in TIME_WAIT.
