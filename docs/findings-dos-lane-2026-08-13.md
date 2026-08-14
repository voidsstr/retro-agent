# DOS lane findings — 2026-08-13/14

**Provenance, and why this file exists.** These sections were written during a
DOS-lane session and appended to `retro-3dfx/FINDINGS.md`, because
retro-agent's CLAUDE.md names that path as the findings log. `retro-3dfx` is
the Voodoo 5 lane's repo, not this one. The content therefore sat **unstaged
and uncommitted, on no branch**, in a checkout that was 15 commits behind its
own remote — where any `git checkout`, `reset --hard`, `stash` or `pull` by
that lane would have destroyed it. A peer session spotted it and told us before
that happened.

Copied here verbatim so it is committed somewhere we own. Nothing in
`retro-3dfx` was modified to produce this file. Where these findings should
finally live — here, there, or split — is still open; if they land in
`retro-3dfx/FINDINGS.md` properly, this file should be deleted rather than left
to drift out of sync.

Every finding below is DOS lane: the game manager, its batch files, MS-DOS mode
and the Windows agent. None of it touches the H5/SGL driver lane.

Original insertion points in `FINDINGS.md`: @@ -14,6 +14,512 @@ it until a Voodoo card goes back in.

---

## Playing a game made it the answer to "where did the install go?" (2026-08-13)

Reported as "after installing anything the menu defaults to Duke Nukem 3D and
every game launches Duke Nukem". `post_install`'s last resort looks for a
directory **written to** during the install, because a floppy-era installer
picks its own target and that is invisible to a "what is new?" diff. It took
the **first** directory that had been touched at all - and `C:\GAMES\DUKE3D`
holds `DUKE3D.CFG` (2:15a) and `DD.CFG` (2:06a), written when someone **played**
Duke 3D. So every later install matched it, and Blake Stone and Shadow Warrior
were both recorded as `C:\GAMES\DUKE3D\DUKE3D.EXE`.

**One touched file is not evidence.** An installer writes a program plus its
data - many files at once; a game that merely ran writes a config or a save.
Score every candidate by how many files it gained, take the busiest, and reject
fewer than three.

Knock-on: an `X` row ("this unpack dir is spent") whose game row is removed
keeps **hiding a directory whose install never finished**, so the operator
cannot even see it to retry. `load_registry` now drops an X row with no
matching game row.

## A DOS timestamp packed as `year << 26` overflows and INVERTS (2026-08-13)

Found while building a fixture for the bug above. `dos_stamp_now()` packed the
**full year** shifted left by 26 - and `1980 << 26` needs 37 bits, so on a
16-bit compiler (`unsigned long` = 32) the year silently wrapped **modulo 64**:

```
1980 % 64 = 60      2026 % 64 = 42      ->  a 1980 file sorts NEWER than 2026
```

Every "was this written recently?" answer inverted across a year boundary. It
never bit on **.243** only because its **CMOS battery is dead** and every stamp
there is 1980 - the broken clock hid the broken arithmetic. Both helpers now
produce DOS's own packed **date word in the high half, time word in the low
half**, which is monotonic and fits 32 bits exactly:
`((unsigned long)ft->wr_date << 16) | ft->wr_time`.

**A dead RTC is a diagnostic hazard in its own right**: file times on that box
run *backwards* between sessions (each boot restarts near 00:00), so "newer
than" comparisons across reboots are meaningless there.

## /kflush fixed DHCP - confirmed on hardware (2026-08-13)

```
02:02:36  kflush: keyboard buffer drained
          DHCP request sent, attempt 1: Offer received, Acknowledged
net:      DHCP lease obtained - downloads should work
```

against `attempt 1: Aborting` in the session before it. Note the box loaded
**3C509 on int 0x60** that time and NE2000 on 0x63 in others - it answers to
more than one packet driver, so do not read the driver line as a fault.

With the lease working, a failed `HTGET` is the **host-side bridge**, not the
DOS box: `serve_dosgames.py` must run on the **Windows** host (this WSL is NAT'd
on 172.19.x and invisible to the LAN, and Windows interop is disabled here, so
it cannot be started from the Linux side at all). The fetch failure now names
the server instead of listing three possible causes.

## MS-DOS mode runs DOSSTART.BAT, NOT AUTOEXEC.BAT (2026-08-13)

Win98's "Restart in MS-DOS mode" does not run `C:\AUTOEXEC.BAT` - it runs
**`C:\WINDOWS\DOSSTART.BAT`**, and on .243 that file **did not exist**. So the
box could be perfectly configured at the real-mode boot prompt (where
AUTOEXEC/CONFIG.SYS do apply, e.g. F8 -> "Command prompt only") and completely
bare in MS-DOS mode: no PATH to `C:\DOSGAME`, no BLASTER, no hint that `PLAY`
exists. If MS-DOS mode behaves differently from the F8 prompt, this is why.

## No SHELL= line means a 256-byte environment, and SET fails SILENTLY (2026-08-13)

`C:\CONFIG.SYS` had no `SHELL=` line, so real-mode DOS gave COMMAND.COM the
**default 256-byte master environment**. Measured need on .243 is ~286 bytes:
boot variables (COMSPEC/PATH/PROMPT/TMP/TEMP/windir/winbootdir ~165) +
AUTOEXEC's BLASTER/MIDI (~47) + PLAY.BAT's MTCPCFG/DGLOG (~62) + RUN.BAT's
PATH prepend (~12).

Past the limit `SET` does not fail loudly - it prints "Out of environment
space" and **carries on with the variable unset**. `%DGLOG%` then expands to
nothing, so every logging line becomes `echo ... >>` with no target (which is
how the diagnostics that explain everything else disappear), and mTCP loses
`MTCPCFG`, which is how DHCP and HTGET find their configuration at all.

Fix: `SHELL=C:\COMMAND.COM C:\ /P /E:1024`. **Boot-critical** - a `SHELL=`
naming an interpreter that is not there is an unbootable machine, so confirm
`C:\COMMAND.COM` exists, back up to `C:\CONFIG.BAK`, and read the file back.
Recovery is F8 -> "Safe mode command prompt only" (bypasses CONFIG.SYS) then
`COPY C:\CONFIG.BAK C:\CONFIG.SYS`.

## "Exit To Dos.pif" on .243 is NOT an MS-DOS-mode PIF (2026-08-13)

Both `C:\WINDOWS\Exit To Dos.pif` and `command.PIF` are 967-byte copies of the
plain **MS-DOS Prompt** PIF: they carry `MICROSOFT PIFEX` / `WINDOWS 386 3.0` /
`WINDOWS VMM 4.0` sections but **no `CONFIG  SYS 4.0` or `AUTOEXECBAT 4.0`
section**, which is what Windows writes for a real MS-DOS-mode entry (compare
the stock `MS-DOS Mode for Games.pif`, 3,181 bytes, which has both). Launching
one opens a DOS **window inside Windows**, not MS-DOS mode - and a windowed box
running DOSGAME's full-screen mode-13h menu can easily look like a hang. The
supported routes are Shut Down -> "Restart in MS-DOS mode", or **F8 at boot ->
"Command prompt only"**, which is the one that has actually worked here.

## fgets does not consume the newline on a maximum-length line (2026-08-13)

`fgets(buf, n, f)` stores at most n-1 characters and stops **without** the
'\n' when the line is exactly that long. DOSGAME read PENDING.TXT straight
into `title[MAX_TITLE + 1]`, so a title of exactly 40 characters left the
newline in the stream; the next `fgets` returned only that newline, `unpack`
came out empty, and post_install declared **"PENDING.TXT incomplete"** for an
install that had *succeeded*. RUN.BAT then printed "Nothing runnable was
found", no registry row was written, and that game could never reach the
Installed tab however many times it was installed.

**123 of the shipped catalogue's 2,982 titles are exactly 40 characters**,
because gen_catalog truncates to 40 — the bug is aimed squarely at the maximum
the producer emits. The same off-by-one on `want[13]` silently blanked the
tile for the 459 rows whose launcher is a full 12-character 8.3 name
(`HTIC_V10.EXE`). **Size a read buffer bigger than the field it fills, or read
the line separately and copy in.**

## An installer recorded as the launcher is a PERMANENT trap (2026-08-13)

`gen_catalog.py` emitted `exe or "INSTALL.EXE"`, so every kind-'I' archive
holding only an installer plus a packed disk set named INSTALL.EXE in the
catalogue's exe field. post_install trusted it and wrote
`G|<title>|<dir>|INSTALL.EXE|`. Because a 'G' row makes `reg_covers_dir()`
hide that directory from every later scan, there was **no way back**: Enter on
the game re-ran the installer forever, with no snapshot and no reconciliation
— the exact loop the registry was built to end.

The same trap via the UI: F2's `next_launcher` accepted every program in the
directory with none of `pick_launcher`'s filtering, so one press on a DEICE
disk set recorded `DEICE.EXE` as the game, forced kind 'R' (discarding the
snapshot + /postinst wrapper), and wrote the covering 'G' row.

Two rules fell out of this: **an installer is never an answer to "what plays
this"** (post_install and next_launcher both filter through
`is_setup_exe`/`is_skip_exe` now), and **a remembered choice must carry its
class** — needs-setup rows are stored as a new `'S'` flag that reloads as kind
'I' instead of masquerading as ready-to-run.

## A game's own installer was HIDING the installed game (2026-08-13)

The real reason "games that showed as installed re-ran the installer" on .243,
and it had nothing to do with launcher selection. `scan_local`'s de-duplication
keyed on the **title**, which for a scanned directory is just its 8.3 name, and
always discarded the entry from the **later** scan root. With
`scan=C:\GAMES;C:\` that silently deleted five playable games:

```
C:\ROTT   kind=R ROTT.EXE   lost to  C:\GAMES\ROTT   kind=I INSTALL.EXE
C:\DOOM, C:\DUKE2, C:\RAPTOR, C:\WACKY   the same way
```

Same 8.3 name under two roots is the NORMAL shape here — `C:\GAMES\ROTT` is the
unpacked DEICE disk set, `C:\ROTT` is what its installer produced — not a
duplicate. The survivor was always kind `I`, which the menu renders as
"run setup" and whose Enter runs the installer.

**Arithmetic proof in the box's own log**, which is how it was finally pinned:
3 registry rows + 21 "scan: FOUND" under `C:\GAMES` + 8 under `C:\` = 32
entries created, and the very next line reads `loaded: 27 installed`. Five
vanish with no log line at all, because the loop had no `logf` and the loss
only ever surfaced in a total. **A silent delete is nearly unfindable — log
every drop.**

Two directories escaped and confused the picture for hours: `C:\HERETIC`
(because a hand-written `X` row in INSTALL.LST removed `C:\GAMES\HERETIC`
first) and `C:\KEEN` (its C:\GAMES counterparts are named KEEN4 and KEENDRMS,
so no title collided).

## COMMAND.COM parses redirection inside `rem`, too (2026-08-13)

The mystery 0-byte files named `43` (in `C:\`, `C:\KEEN`, `C:\DUKE` and
`C:\GAMES\HERETIC`) came from DOSGAME.BAT's own **comment**:

```
rem >=43 means the menu died in a way it does not define
```

COMMAND.COM sees the `>`, skips the `=` (an argument delimiter, and an illegal
FAT character), and takes `43` as an output filename — once per pass through
the menu loop. They landed all over the disk because RUN.BAT `cd`s into the
game's directory and **a batch `cd` persists into its caller**. cmd.exe does
the same thing, so `serve_win.bat` had it too. The documented "no `<>` in an
echo" rule is not enough: it applies to **every line, comments included**.
`tests/run_dos_tests.sh` and `test_dosstage_and_batch.py` now reject any angle
bracket in a `rem`.

## COMMAND.COM's batch line buffer is 128 bytes, and it chops silently (2026-08-13)

Three generated RUN.BAT failure lines ran 139-158 characters — `if errorlevel
1 ` (16) + `echo run:    ` (13) + the message + ` >> C:\DOSGAME\DOSGAME.LOG`
(30). The tail carrying the redirect was cut, so the ONE line explaining why an
install failed never reached the log — on a box in MS-DOS mode with nobody
watching the screen, that log is the only evidence there is. This is the same
126-byte class as the command tail but a *different* limit with a different
victim; budget the whole line, prefix included.

## Staging that compares SIZE cannot ship a batch-file fix (2026-08-13)

`dosstage.c`'s `copy_if_different` skipped any file already present at the same
size. The DOS payload is mostly batch scripts, where the ordinary fix preserves
length exactly — `0x300` to `0x320`, `goto try4` to `goto try5`, `PACKETINT
0x62` to `0x63` — so publishing one left every already-staged box on the broken
file **forever**, while `dosstage_run` logged "staged 0 file(s)" and wrote the
success marker. The agent's own updater carries a `.ver` sidecar because it
learned exactly this once; the DOS path never got the same treatment. Now
compares last-write time too (`CopyFileA` preserves it, so re-runs stay
no-ops), and `DOSSTAGE force` finally means "re-stage" rather than "ignore the
off-switch".

## A generated artifact outlived the fix to its generator (2026-08-13)

`gen_catalog.py`'s `dos83()` and `survey_share.py`'s `coms_shallow` export were
both fixed — and `data/GAMES.CAT` was never regenerated, so the shipped
catalogue (byte-identical to the one on .243) still carries the OLD
`name.upper()[:12]` output: 14 launchers truncated to 12 characters
(`DECATHLON.EX`, `BOULDER DASH`) and **not one .COM launcher in 2,982 rows**,
which is impossible for a pre-1990 DOS library — every COM-launched game is
missing from the Available tab. Both fixes are inert on hardware.
`make catalog` rebuilds it (needs the share mounted) and
`scripts/dosgames/tests/check_catalog.py` reports the staleness from anywhere,
no share required. **Check in a generated artifact and you have to check that
it still matches its generator.**

## The share catalogue is a good TIE-BREAKER and a bad RANKER (2026-08-13)

DOSGAME's launcher picker keeps meeting directories where several programs
could plausibly be "the game". The 2,983-row share catalogue names a main exe
for each title, so it looks like the oracle — but it is only trustworthy when
asked a narrow question.

Where it WORKS (verified on the box's own directories):
- `C:\ROTT` — `ROTT.EXE` (listed) vs `ROTTIPX.EXE` (not listed). Name shape
  alone got this exactly backwards and shipped `ROTTIPX.EXE`, the IPX
  multiplayer launcher, as the game.
- `C:\KEEN` — `KEEN4E.EXE` (listed 2x) vs `KEEN.EXE` (not listed).
- `C:\GAMES\JAGGED~1` → `DOXVIEW.EXE`, `C:\GAMES\KEENDRMS` → `KEENDR.BAT`,
  both of which the catalogue names and both of which "first non-tool .EXE"
  got wrong.

Where it FAILS — the catalogue contains rows that name support programs as the
launcher: **`DEALERS.EXE` (3 rows)**, `RAP-HELP.EXE`, `BS-HELP.EXE`,
`3DRCAT.EXE`, `EXECUTOR.EXE` (5), `COMMIT.EXE` (6), `DM.EXE` (7), `HELP.BAT`,
`README.BAT`, even **`AUTOEXEC.BAT`**. Using it as a general ranker would have
broken `C:\RAPTOR` and `C:\WACKY`, which pick correctly today.

**Rule: consult it only for an UNAMBIGUOUS answer** — exactly one candidate
listed — and fall back to the old heuristic otherwise. Also note the cost:
re-reading the 293K catalogue per ambiguous directory measured 6-7 s EACH in
DOSBox, so it is indexed once into a 4K two-hash bit set (0 false negatives,
1.38% false positives against the real catalogue; a false positive can only
produce "can't tell").

## Every Apogee shareware directory ships an advertising bundle (2026-08-13)

`C:\ROTT` has **10 runnable programs** and only one is the game. Every
Apogee/3D Realms title ships `APOGEE.BAT`, `CATALOG.EXE`, `DEALERS.EXE`,
`SWCBBS.EXE`, `3DRCAT.EXE`, `VENDOR.EXE` and a per-game help viewer whose name
varies (`RAP-HELP.EXE`, `WW-HELP.EXE`, `DN2-HELP.EXE`, `BS-HELP.EXE`,
`DN3DHELP.EXE` — matched by the `*HELP.EXE` shape, since they cannot be listed
by name). id titles add `SERSETUP.EXE` / `IPXSETUP.EXE`. Any "pick the first
.EXE" rule picks an advert.

## Two different "installed but it re-runs the installer" bugs on .243 (2026-08-13)

Both showed the same symptom in DOSGAME's menu — a game listed as installed
that, on Enter, ran something installer-shaped instead of playing. They have
**nothing in common** beyond the symptom, so fixing one left the other alive:

**1. Series shell picked over the episode binary.** `C:\KEEN` held
`KEEN.EXE` (642,659 bytes — an Apogee front-end/ad shell) beside
`KEEN4E.EXE` (105,108 — Commander Keen 4, the actual game). The picker's
strongest rule is "an .EXE named after its directory", and that is exactly
the wrapper. Fix: an .EXE whose name **extends** the directory name
(`KEEN` → `KEEN4E`) now wins over the bare one, unless the extra suffix looks
like a support tool (`ROTT.EXE` vs `ROTTSND.EXE` — `is_util_suffix()`). A
`.BAT` named for its directory is a deliberate launcher (`TYRIAN.BAT`) and
still wins outright.

**2. An unextracted self-extracting download registered as a game.**
`C:\HERETIC` contained **one** file: `HTIC_V10.EXE`, 1,439,232 bytes. That
name is in no installer table, so it fell through to "first non-tool .EXE" and
was recorded as ready-to-play. Fix: one lone program with **zero** data files
beside it and a size over 256K is classified as needs-setup, so launching it
extracts and the post-install pass then re-picks the real binary. The size
floor is load-bearing — without it a small lone exe (a tiny complete game, and
a legitimate game one level below its unpack dir) got misread the same way.

**A stored registry row is trusted, so neither fix reaches an already-wrong
row.** `C:\DOSGAME\INSTALL.LST` had to have its `G|` rows dropped by hand for
the box's three installed games to be re-derived; the `X|` rows (spent unpack
dirs) were kept.

## mTCP DHCP "Aborting" on attempt 1 is a STRAY KEYSTROKE, not a dead NIC (2026-08-13)

`.243`'s DOS log showed `DHCP request sent, attempt 1: Aborting` on both
attempts, which reads exactly like a network fault — the card is fine and works
in Windows. mTCP's DHCP advertises **"Press [ESC] to abort"** and takes **any**
pending keystroke as that ESC, so a key still sitting in the BIOS type-ahead
buffer from the menu (or from a "press a key" prompt) kills the lease request
the instant it starts. Fix: `DOSGAME.EXE /kflush`, a headless mode that only
drains the buffer, called on the line immediately before **both** `DHCP.EXE`
call sites in `NETUP.BAT`.

## Do NOT run DOSGAME.EXE through the agent's EXEC/EXECW on the box (2026-08-13)

`EXECW 180 C:\DOSGAME\DOSGAME.EXE /selftest` on **.243** took the agent off the
network (connection *refused* afterwards, not timed out — it was gone, not
busy). `/selftest` is a no-video mode built for the **DOSBox CI loop on the dev
host**; it is not a way to check a real box's scan remotely. Verify launcher
selection in DOSBox against a fixture that mirrors the box's directories, and
read the box's own `DOSGAME.LOG` for what actually happened there.

## Apogee shareware is a MULTI-DISK install — `INSTALL.EXE` + `<NAME>._1`/`._2` (2026-08-12)

A lot of the share's DOS catalogue is the floppy-era BBS distribution:

```
C:\GAMES\BLAKE   BS_1BBS._1 (739K)   BS_1BBS._2 (739K)   INSTALL.EXE
C:\GAMES\KEEN4   K4_1_BBS._1 (643K)                      INSTALL.EXE
```

`INSTALL.EXE` expects each `._N` on its **own floppy**. Run from a hard-disk
directory it still prompts **"insert disk 2"** even when every disk file is
already sitting right there — so the natural reading is "a disk is missing"
and people stop. Both installs on **.243** ran under ten seconds and wrote
nothing, which is what "installed some games but they were not playable" was.

**Answer the prompt with the unpack directory, and press ENTER at a disk
prompt** — the data is already on disk. DOSGAME now prints that *before*
launching an installer.

**The tooling was making it worse**: reconciliation reported "bad download, or
an installer that was cancelled", which sends the search in exactly the wrong
direction. It now lists the unpack directory into the log, counts leftover
`._N` files, and distinguishes *"multi-disk installer that did not finish —
run setup again and answer `<dir>`"* from *"nothing was written at all"*.
The directory listing in the log means the next case is diagnosable from the
log alone.

**What DOES work unattended** (verified on .243, same session): single-archive
installers that write to their own directory — `HERETIC` → `C:\HERETIC\HTIC_V10.EXE`,
`KEEN1` → `C:\KEEN\KEEN.EXE`, both found and registered automatically by the
snapshot/diff pass.

## `CreateThread(..., lpThreadId=NULL)` IS ILLEGAL ON WIN9x — it silently disabled half the agent for years (2026-08-12)

**On Windows 95/98 `CreateThread` requires a non-NULL `lpThreadId`. Only NT
accepts NULL.** Passing NULL returns **`ERROR_INVALID_PARAMETER` (87)** and no
thread. Every fire-and-forget helper in `agent/src/main.c` passed NULL, so on
**.243 (N5R5L9, Win98 SE)** all of these silently never ran:

`automap` · `autoupdate` · `retrowall` · `watchdog` · `ai_status` ·
`sharelog` · `dosstage` · **`discovery`**

Only `dosstage` checked its return value, so only `dosstage` ever reported —
and its message guessed **"(low memory?)"**, which sent us auditing RAM on a
box with **87 MB free**. A wrong guess in an error string hid the real cause
behind it for as long as the message existed.

That one line explains a whole string of symptoms previously blamed elsewhere:

- **auto-update "quietly doing nothing"** on that box across four versions —
  the thread never started. (The Win9x running-exe lock is a *second*,
  independent reason it could not have worked; both are now handled.)
- **the agent log never appearing on the share** — `sharelog` never ran.
- **the share needing `MAPSHARE.BAT` to map it** — `automap` never ran.
- **no wallpaper/theme** on that box — `retrowall` never ran.
- **UDP :9899 refusing every probe all session** — `discovery` never ran.

Fixed in agent **1.27.0/1.27.1**: one `spawn_helper()` passes `&tid`, checks
the result, closes the handle, and the failure message now names
`ERROR_INVALID_PARAMETER` and what it means instead of blaming memory.
`tests/python/test_agent_resources.py::test_createthread_passes_a_thread_id_pointer`
rejects any call passing NULL — it is what caught the last two (`discovery`
and the per-connection `client_thread`).

**Verified on hardware.** Under 1.26.x the log had only
`dosstage thread FAILED to start: 87 (low memory?)`; under 1.27.0 it has
`[DOSSTAGE] staging DOS programs from \\192.168.1.122\...` /
`staged 3 file(s)` and `retrowall:` reporting in.

**Consequence to remember:** now that `dosstage` actually runs, it **reverts
hand-deployed DOS files** to whatever is in the share's `dosgame\` payload on
the next boot. Publish to the share — pushing straight to a box no longer
sticks.

## Two agents at once was the cause of the locked exe and the unreachable box (2026-08-12)

Nothing stopped two `retro_agent.exe` running simultaneously. Each start logs
`Listening on TCP :9898+:9897`, but the second only gets whichever port the
first did not take — so **the fleet's port answers nothing while an agent is
plainly running**, and killing "the" agent leaves **the exe locked by the copy
still alive** (hit by hand on .243 trying to replace it). It is also why
`9898 refused / 9897 open` recurred all session and why a binary swap needed a
reboot.

Fixed in **1.27.0** with a named mutex claimed before anything else; a second
copy logs why and exits. The OS releases it however the holder dies, so it
cannot lock you out of your own box.

Two other resource bugs found with it:
- **`CreateThread`'s handle was discarded at every call site** — including once
  **per connection** in threaded mode, leaking a kernel object for every client
  the agent ever served. Closing the handle does not stop the thread.
- **Client slots were held until the peer disconnected**, which a half-open TCP
  connection never does. Ten stale slots and the agent is unreachable while
  looking healthy. Slots now track activity and are reaped after 5 min —
  longer than the longest legitimate silence (a 30 s long-poll, 1 s on 9x).

## DOS lane: DOSGAME "installs a game but never offers to play it" — five separate causes (2026-08-11)

Reported from a real Win98 box in **MS-DOS mode**: install a game from the LAN
catalogue, and it never shows up as runnable. Not one bug — five, each of which
alone is enough to produce that symptom. All fixed in `DOSGAME` **v0.2**
(`retro-agent/scripts/dosgames/`), all pinned by tests
(`scripts/dosgames/tests/run_dos_tests.sh` in DOSBox +
`tests/python/test_dosgame_{stem,stability,install_detect}.py`).

1. **The installer's own target directory was never discovered.** Nearly half
   the catalogue (1,434 of 2,982) is `INSTALL.EXE` + payload, and a classic
   Apogee/id installer copies the game to a directory *it* chooses
   (`C:\WOLF3D`), not the `C:\GAMES\<stem>` we unzipped into. The menu listed
   only the leftover unpack dir — whose one executable is `INSTALL.EXE` — as
   "run setup", so Enter re-ran the installer **forever**. Fixed by recording
   what an install produced in a registry (`INSTALL.LST`), populated by a
   post-install pass that diffs the directory listing against a snapshot taken
   before the installer ran.
2. **DOS silently truncates a command tail at 126 bytes.** Measured: batch
   lines of 160/200/215 chars all delivered exactly 126 bytes of arguments, no
   error. The install line pasted the URL-encoded zip name (mean 61 chars, max
   137) onto the server URL, so **845 of 2,982 entries** fetched a chopped-off
   URL, 404'd, and reported *"Download failed — check the network"* — sending
   everyone after a networking problem that did not exist. Fixed with a
   fixed-length `/z/<STEM>` endpoint.
3. **The 8-char install stem is not unique: 1,268 of 2,982 rows collided.** All
   eleven Duke Nukem titles installed into `C:\GAMES\DUKE_NUK`, unzipping over
   each other, and installing any one marked the other ten as installed. Stem
   is now 5 readable chars + 3 base-36 chars of a 16-bit FNV-1a hash — zero
   collisions. `serve_dosgames.py` computes the identical stem; a test diffs
   the two implementations.
4. **The scan was depth-1 only**, but ~24% of the share's archives are not
   flat-root, so the game lands one directory further down and was listed
   **nowhere at all**.
5. **A receipt could never make a game playable.** The old `INSTLD.LST` only
   set `installed=2`, which stars the *catalog* row; `rebuild_view()` puts only
   `installed==1` on the Installed tab. The star said "installed" and there was
   still no way to launch it.

**"9898 refused + 9897 open" means TWO agent instances, not a dead agent.**
On **.243** (N5R5L9, Win98, agent 1.22.0) the box looked dead: 9898 refused
connections while the alt listener on 9897 accepted and (apparently) never
answered. `PROCLIST` showed **two `RETRO_AGENT.EXE` processes** — one from the
boot-time `Run` key and one from a manual restart. Each logs
`Listening on TCP :9898+:9897`, but the second only gets whichever port the
first did not hold, so the fleet's normal port is gone while the box is
actually fine. `PROCKILL` the duplicate; the survivor does **not** re-bind
9898, so a clean single instance needs a restart (or the next Windows boot).

**A 10-second timeout produces a FALSE "dead agent" diagnosis on this box.**
9897 was answering all along — the machine doc records auth handshakes taking
**40–150 s** on this 31 MB Pentium 1, and a 10 s connect timeout reported it
as hung. Always use 90–180 s connect timeouts here before concluding anything
is wrong. (A raw TCP *refusal* is still meaningful and immediate; a *timeout*
on this box is not.)

**Do not `EXEC`/`EXECW` a real-mode DOS TUI through the Windows agent anyway.**
The agent's own rule is "EXEC = CLI only", and the 9x agent is single-threaded;
a program that takes over the screen and keyboard is not a CLI tool. (In the
event `DOSGAME.EXE /selftest` did complete — it rewrote `DGSELF.TXT` — so it
was survivable here, but it is not a safe habit.) The DOSBox harness
(`scripts/dosgames/tests/run_dos_tests.sh`) is what DOS-binary testing is for:
the same `/selftest` runs there in ~2 s against a fixture that reproduces a
real Win98 `C:\` root. Deploy the binary over the agent; run it from the box.

**`CWSDPMI.EXE` was NOT staged on .243** — hardware-confirmed, so the DPMI
suspicion above is real: `C:\DOSGAME` held `UNZIP.EXE` (197,120 bytes, a DJGPP
build) with no DPMI host beside it. In MS-DOS mode every install would die on
`no DPMI` and leave an empty directory. Now deployed from
`csdpmi7b.zip` (delorie.com); `write_install()` also puts `C:\DOSGAME` on the
PATH, because the go32 stub searches PATH rather than the exe's own directory.

**The share had no `dosgame\` payload at all** (`\\192.168.1.122\files\Utility\
Retro Automation\` held only the agent/chat binaries), so `dosstage` had
nothing to stage and every DOS box was running whatever was hand-copied onto
it. The 0.2 build is now published there.

**Other DOS gotchas measured while fixing this** (all now in
`scripts/dosgames/README.md`):
- **`if exist DIR\*.*` is TRUE for an empty directory** (`.`/`..` match), so
  batch cannot tell "unzipped nothing" from "unzipped fine". `if not exist
  DIR\nul` *does* correctly detect a missing directory. Content judgements
  belong in the exe, reported back via `errorlevel`.
- **A `.BAT` launcher must be `CALL`ed.** Chaining to one abandons the rest of
  `RUN.BAT` silently (the outer menu loop still resumes, so it looks harmless
  until you add post-launch bookkeeping).
- **Watcom's default DOS stack is 2K** — too close for the scan call chain plus
  `sprintf`. Build with `-k8192`; a blown stack on a real box is a hang.
- **Suspected, not yet confirmed on hardware:** the staged `UNZIP.EXE` is a
  DJGPP build needing a **DPMI host**, which "Restart in MS-DOS mode" does not
  provide (EMM386 gives VCPI, not DPMI). If `CWSDPMI.EXE` is not staged beside
  it, every install dies with `no DPMI` and leaves an empty directory. v0.2 puts
  `C:\DOSGAME` on the PATH (the go32 stub searches PATH) and names CWSDPMI in
  the failure message. **Verify what is actually in the share's `dosgame\` dir.**
