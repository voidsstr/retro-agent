# Staged titles: patch level and stability register

**User directive, 2026-09-30:** every staged title must be patched to the
highest level available, so LAN and online play work and the game is stable -
and **the agent needs to install patches for the staged games**. A box must
never be left on an older build than the library carries, and the library must
never carry an older build than exists.

## How a patch reaches a box today

1. Install the patch **in the build VM** (CLAUDE.md "INSTALL IN THE BUILD VM"),
   never on a fleet box.
2. Capture the patched tree into the staged library (`Games-Library/<Title>`),
   verified file by file.
3. Bump `Games-Library/_deploy_generation.txt`. `retro-autodeploy` then runs
   `GAMESYNC RESET` + `START` on every box as it comes online, and GAMESYNC
   copies the changed files (size + write-time resume test).

So a patch staged in the library *does* reach every box - but only because
someone staged it and bumped the generation.

## What the agent still needs to do (open requirement)

- **Know each box's installed build per title** and report it - e.g. read
  `System\Build.ini` (UE2), the exe's version resource, or a per-title marker
  the stager writes - in `GAMESYNC STATUS` / `GAMERES VERIFY`-style JSON.
- **Flag drift**: a box whose installed build is older than the library's
  (a title synced before the patch landed, a sync that failed part-way) must
  say so loudly, and a sync must bring it up to the library build.
- **Flag a library that is behind**: this register is the record of the
  highest known build per title; a title below it is a finding, not a pass.

None of that is implemented yet. Until it is, the register below is checked by
hand when a title is staged or patched.

## Register

| title | staged build | highest known | notes |
|---|---|---|---|
| UT2004 | **3369** (`Build.ini` `UT2004_Build_[2005-11-23_16.22]`, `Version: 3369 (128.29)`) | 3369 official final; OldUnreal's community **3374** is a release candidate (seen on internet servers as `v3374-RC1`) and is **not** staged - not on the share, and the fleet server runs 3369.3 | joins the fleet server and OpenSpy-listed internet servers |
| UT2003 | **2225** (`UT2003_Build_[2003-04-07_17.42]`) | 2225 official final | no CD check after the patch |

### UT2003 / UT2004 stability settings (2026-09-30)

Reported on `.123` (Athlon 64 4000+, Radeon HD 3850): both games stutter about
every 30 s and input hesitates. Measured there: nothing else on the box takes
CPU (`typeperf`, 150 s of a bot match - UT2004 held 87-100 %); both games were
already on their final patch. The staged inis carried settings known to cause
exactly this, now changed in the library (and warned on by
`validate-staged-library.py`, check `ue2-config`):

| setting | was | now | why |
|---|---|---|---|
| `[WinDrv.WindowsClient] UseSpeechRecognition` (UT2004) | True | False | Windows speech recognition on the mic all game |
| `[ALAudio.ALAudioSubsystem] UseVoIP` (UT2004) | True | False | voice-chat capture open; the fleet has no mics |
| `ReduceMouseLag` (render devices) | True | False | CPU waits on the GPU every frame |
| `[Engine.GameEngine] CacheSizeMegs` | 32 | 128 | cache thrash on large maps |
| UT2004 `MasterServerList` | `...,Group=N)` | no `Group` | 3369 logs `Unknown member Group` 12x per start |
| UT2003 `MasterServerAddress[0/1]` | `ut2003master1/2.epicgames.com` (dead) | `utmaster.openspy.net` | browser said `Connection Failed - Retrying`; now `Welcome to OpenSpy!`, `Ready` |

Whether the stutter is gone needs a person playing on `.123` - the agent
cannot measure frame pacing, and the counters showed no competing process.
