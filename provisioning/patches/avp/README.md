# Aliens versus Predator Gold: undo the InstallShield obfuscation in the staged tree

## The defect

`Games-Library/AliensVsPredator` is the raw payload of the Gold disc 1. 154 of
its files still carry the InstallShield 5 file obfuscation. The retail
installer removes it as it copies each file, using the transform unshield names
`unshield_deobfuscate`:

    plain[i] = ror8(stored[i] ^ 0xD5, 2) - (i % 0x47)        (seed 0 for every file)

Because of this, avp.exe never loads a fast-file. On .123, .145, .195 and .240,
the last `LOGFILE.TXT` ends with:

- `Menus\IntroFont.rim ... file not found`
- `AwCreateGraphic(): ERROR: No data medium is specified`
- `ASSERTION FAILED! pSurface alt_tab.cpp 198`

No log contains a `Loaded FastFile:` line. The game has never reached its menu on
this fleet. The DB rows `runs, d3d 640x480` recorded this hang.

## Files in this directory

| file | what it is |
|---|---|
| `apply.py` | the tool: `--check`, `--build`, `--publish` (future use), `--install-server` (not applicable), `--video-cfg` |
| `files.tsv` | 154 `deobfuscate` rows and 56 `keep` rows. Each row has the staged md5, the patched md5 and the reference md5. `make_table.py` derives it; do not edit it by hand |
| `nakedavp-gold-md5.txt` | the independent oracle: the 201-file Gold-install md5 list from github.com/atsb/NakedAVP |
| `make_table.py` | re-derives the table from the share's bytes, prints any difference, and refuses counts other than 154/145/56 |

| staged files | count | proof |
|---|---|---|
| `fastfile/Tex1..58.FFL`, `fastfile/ffinfo.txt` | 59 | md5 equals NakedAVP; each FFL also passes the RFFL index check, and every packed file is a whole IFF chunk |
| `avp_huds/*.rif` | 23 | md5 equals NakedAVP |
| `avp_rifs/*.rif` | 63 | md5 equals NakedAVP |
| `FMVs/*.bik` (8), `FMVs/IntroSound.smk` | 9 | no published md5. Checked by structure: the Bink frame index plus each frame's audio packet sizes, and the Smacker size table plus each frame's chunk sizes. ffmpeg 7.0.2 also decoded every frame of all 9 without error (build phase) |

**Leave these alone.** They already match NakedAVP as staged: `Snd*.FFL`,
`common.ffl`, `*.dat` and `language.txt`. `shape_rifs` (REBINFF2), the 53
`message*.smk`, `HSound`, `LSound`, `RSound`, `English`, `MPConfig` and
`MPlayer` are also plain. A whole-tree scan (`--check --full-scan`) finds no
other obfuscated file. The same scan flags exactly these 154 once they are
removed from the table.

## Run it

    python3 apply.py --check [--full-scan]     # read-only; every input md5; exit 0 = OK
    python3 apply.py --build                   # -> ~/.retro-fleet/patch-out/avp + manifest.json
    python3 apply.py --publish --dry-run       # FUTURE: prints the plan
    python3 apply.py --publish                 # FUTURE: back up, then put, one file at a time

`--publish` handles each file in this order:

1. It reads the share copy through `/mnt`. If the copy is already patched, it
   skips the file. If the copy is neither the staged original nor the patched
   file, it stops.
2. It backs up the original to
   `Files/Games-Library/_patches/AliensVsPredator/originals-2026-09-29/<path>`.
   An identical backup is reused. A different backup stops the run.
3. It puts the patched file with `scripts/fleet/sharewrite.py`, which checks the
   md5 through `/mnt` and a fresh mtime.

The first failure stops the run. `ffinfo.txt` goes last, so a publish that stops
part-way leaves the game failing exactly as it does today. It never creates a
half-working fast-file set.

Every file keeps its size, so its write time must be fresh: GAMESYNC resumes on
size and mtime together.

**If a put fails part-way**, the share copy of that one file can be truncated or
missing (gvfs has done both on this NAS). A re-run then STOPs on it, because it
is neither the staged original nor the patched file, and it writes nothing. Its
original is already safe: the backup put ran first and sharewrite verified it
through `/mnt`. To recover, put the built file by hand, then re-run `--publish`,
which skips everything already patched:

    python3 scripts/fleet/sharewrite.py put ~/.retro-fleet/patch-out/avp/<path> "Files/Games-Library/AliensVsPredator/<path>"

The publish is 308 puts (154 backups and 154 files). Each put opens its own
smbclient session and reads the NAS credentials from the vault, so expect it to
run for a long time. Run it detached (`setsid`), not in a shell that can be
closed.

Then follow the staged-game loop:

1. Purge `C:\Games\AliensVsPredator` and its `.lnk`.
2. Run `GAMESYNC RESET` then `GAMESYNC START`.
3. Check that `failed_files` is 0 and `titles_done` equals `titles_total`.
4. Launch, and look in `LOGFILE.TXT` for `Loaded FastFile:` lines with no
   `ASSERTION FAILED`.
5. Push to every box.

## Independent verification (2026-09-29 review)

The reviewer re-checked the build without relying on this directory's code:

- **The transform** was re-run through unshield's own `unshield_deobfuscate()`,
  called from `libunshield.so.1` (unshield 1.6.2) via ctypes, over all 154
  staged originals. Every output is byte-identical to this build.
  `tests/python/test_patch_avp.py` now pins apply.py against that library.
- **The reference list** was fetched again from
  `raw.githubusercontent.com/atsb/NakedAVP/master/README.md`. Its 201 MD5 lines
  are identical to `nakedavp-gold-md5.txt`, so the list is external, not
  derived from our own output. 145 de-obfuscated files and 56 kept files match
  it.
- **The FMVs**: ffmpeg 7.0.2 with `-xerror -map 0` decodes the video and
  audio of all nine without error, and the frame counts match their headers.
  The staged originals cannot be opened.
- **The whole tree** was scanned a second way: longest byte run, printable
  ratio and a broad magic table, raw against de-obfuscated. That scan also
  finds nothing beyond the 154. Its two hits were checked by hand and are false
  positives: `avpgolded.mpi` is zero-padded, and `MPlayer/System/install.log`
  is plain text. Run over the table instead, the same scan flags all 154.
- **Limit of `--full-scan`**: it recognises formats by magic, plus text that
  de-obfuscates to printable characters only. An obfuscated file in a format it
  does not know would not be flagged. That is why the second scan was run. Re-run
  both if the library copy of this title ever changes.

Evidence: `.claude/evidence-1080p/build-avp/review/`.

## What else 1080p needs (not done here)

From the avp.exe disassembly (`.claude/evidence-1080p/build-avp/avp-exe-video-disasm.txt`):

- **The menus always run at 640x480** on the primary display. `SelectMenuDisplayMode`
  (0x491e90) hard-codes 0x280 x 0x1e0. The chosen mode applies only to
  gameplay (`SetGameVideoMode`, 0x4fada0).
- **Options > Video lists what the driver offers.** It shows every mode from
  `EnumDisplayModes` (flags 0) with bpp > 8, w >= 512 and h >= 384, in a depth
  the HAL device renders. The list caps at 100 modes. So once the data fix
  lands, 1920x1080 should be selectable in game with no further change. Nobody
  has seen this list on the fleet yet.
- **`AvP_Video.cfg` is 32 bytes**: a 16-byte DirectDraw GUID, le32
  DDGUIDIsSet, then le32 W, H and BPP. The loader (0x559810) matches only the
  GUID bytes. The primary display's GUID is 16 zero bytes. A cfg that names the
  right device but a mode the game did not list leaves the mode index at -1.
  The game then reads W/H/BPP from the 12 bytes before its mode table. A writer
  must therefore only write a mode it has confirmed through DirectDraw.
  `apply.py --video-cfg W H [BPP]` produces a hand-test file.
- avp.exe calls `IDirectDraw::SetDisplayMode(w, h, bpp)` once (0x537511), using
  the DirectDraw 1 signature with no refresh argument.
- A device only enters that list if its DirectDraw caps include `DDCAPS_3D` and
  `IDirect3D` (v1) enumerates a hardware device for it (`dcmColorModel != 0`).
  The depth check reads that device's `dwDeviceRenderBitDepth`. Nobody has
  measured that field on the fleet.

## Records to correct after the hardware test

These records were written while every box was stuck at the pre-menu assertion.
None of them describes the game as it is:

- `Games-Library/AliensVsPredator/README-FLEET.txt` says "THE STAGED TREE IS
  BYTE-COMPLETE" and "THE FAULT IS ON THE BOX". That was a size-only comparison.
- `Games-Library/_patches/README.txt` says "ALIENS VERSUS PREDATOR -> NO PATCH
  NEEDED" and "AliensVsPredator already CLOSED as BLOCKED. Do not touch it."
  Replace both with a record of this de-obfuscation: what it changed,
  `apply.py`, and the backup location.
- `provisioning/fleetres/PER-TITLE-STATUS.md` says "not staged, CLOSED as
  BLOCKED".
- Compat DB rows `runs, d3d 640x480`. The game never reached its menu.
- `docs/lan-multiplayer-status.md` says AvP "screenshots come back black" and
  lists it among the relative-mouse menus. Neither could have been measured on a
  game that never reached a menu. The black frame on `.133` was most likely the
  game dead at its assertion. Take `SCREENSHOT 0` before repeating either claim.
