# gamegate — only deploy a game to a machine that can run it

GAMESYNC used to copy the whole staged library onto every box. The fleet spans a
1999 Pentium III with a Voodoo and a 2011 Sandy Bridge quad, so a machine that
cannot run a title still spent an hour of SMB1 bandwidth on it and then wore a
desktop icon that launches into a black screen or an illegal instruction.

## The split, and why it is where it is

A Pentium III cannot call a language model, and the RTX 5090 in the dev host
cannot see inside a fleet box. So the work divides strictly by where it can run.

| | where | what |
|---|---|---|
| **collect** | agent, C | `HWPROFILE` — CPUID, real clock, real RAM, the **active** display adapter, OS, DirectX, disc-mounter capability, and a reboot-stable `profile_hash` |
| **decide (rules)** | both | `agent/shared/gamegate.h` and `scripts/gamegate/rules.py` — the same arithmetic, pinned together by a mirror test |
| **decide (borderline)** | host, Python | `llm.py` — ollama, **only** for the marginal band |
| **remember** | host | `cache.py` — SQLite keyed on the hardware profile, not on an IP |
| **enforce** | agent, C | GAMESYNC skips a gated title and logs why |

The agent carries the deterministic rules **as well as** the host, and that is
not redundancy: a freshly PXE-imaged box syncs its games before any host tool has
ever seen it, and it must still not put Doom 3 on a Pentium III.

## Deterministic rules decide alone wherever they can

```
hard NO    OS floor unmet, a required CPU instruction absent, a GPU two whole
           feature levels short, NO 3D AT ALL against any GPU floor, or the
           tree cannot fit in the free space.
                                            -> arithmetic, no model call
MARGINAL   within 25% of a published minimum, or a GPU exactly one level short.
           -> the ONLY thing that reaches ollama
RUN        everything met.                  -> arithmetic, no model call
```

`none` (an S3 Trio64, a Matrox Millennium, a Tseng ET4000 - chips with no 3D
pipeline whatsoever) is the one exception to the marginal band, even though it
sits only one level below `fixed`. The band exists because a title of that era
usually ships a *lower-detail* path for a weaker rasteriser; there is no
lower-detail path from "has a rasteriser" to "has none". See SCHEMA.md, which
also explains why stating `gpu_feature_level` on a title that ships a software
renderer is a bug rather than a harmless extra.

A gate that phones an LLM to conclude "a Pentium III cannot run Doom 3" is a bad
gate. `tests/native/test_gamegate.c` asserts the obvious cases stay obvious —
if they ever start coming back MARGINAL, every one of them becomes a model call.

## FAIL-OPEN, by explicit decision

Absent data never blocks a title: no `requires.json`, an unparsable one, a field
omitted, an unclassifiable GPU, a clock that could not be measured — all deploy.
The gate blocks only on **positive evidence**.

This was chosen, not defaulted to. Fail-closed would produce a box that silently
receives no games and says nothing about why — the exact failure shape CLAUDE.md's
*"Make Failure VISIBLE"* section is about, and much worse than a box that
receives one game too many. Every skip is logged with its limiting factor and
both numbers behind it:

```
[GAMESYNC] GATED Doom3 - gpu_feature_level: GPU too old for this title's renderer
[GAMESYNC] done: 31/34 title(s) copied, 0 skipped (no room), 3 gated (machine cannot run), 0 file error(s)
```

**Kill switch:** `HKLM\Software\RetroAgent\GameGate` = `0` restores
copy-everything.

## Capabilities are not verdicts

A GeForce2 will never grow a pixel shader; a box with no virtual disc mounter is
one installer away from having one. Calling both "cannot run" tells the operator
to give up rather than to fix the box. So a missing **capability**
(`disc_mount`, today) never changes `run`/`marginal`/`no`: the title still
deploys, only the shortcut that needs it is suppressed, and the log names the
remedy. GAMESYNC re-runs every boot, so it comes back by itself once the box is
fixed.

This matters right now: ten already-staged titles mount a disc image at launch.
**Re-measure which boxes have a mounter before quoting a number here** — as of
2026-08-31 exactly one box lacks one (`.123`, which has no optical drive at all),
not two: `.246` has WinCDEmu and five boxes run DAEMON Tools 3.47. An unchecked
"only .240 has a mounter" in a sibling document blocked four titles for a day.
See SCHEMA.md, which also has the table of what a virtual drive does and does not
satisfy — `disc_mount` does not mean SafeDisc 2.80 will pass.

## Use

```bash
# What is that box, really?
python3 scripts/gamegate/gamegate.py profile 192.168.1.124

# Decide every title for one or more boxes (no writes)
python3 scripts/gamegate/gamegate.py plan 192.168.1.124 192.168.1.145

# Same, and publish <library>/_gamegate/<profile_hash>.txt for the agent
python3 scripts/gamegate/gamegate.py publish 192.168.1.124

# Check the library's requires.json files
python3 scripts/gamegate/gamegate.py lint

# Cache
python3 scripts/gamegate/gamegate.py cache
python3 scripts/gamegate/gamegate.py cache --forget-llm   # keep rule verdicts
```

Useful flags: `--no-llm` (rules only), `--refresh` (ignore the cache),
`--refresh-llm` (drop model opinions, keep arithmetic), `--model`, `--library`.

## ONE PUBLISHER OWNS THE WHOLE FILE (read this before writing `_gamegate/`)

`<library>/_gamegate/<profile_hash>.txt` is **per-profile and whole-library**.
It is written by `gamegate.py publish`, in `rules.py:format_verdict_file`, and
by nothing else.

**A per-title pass must NOT write it.** If you stage a new title and want its
verdict published, **re-run the full publish** for the affected boxes:

```bash
python3 scripts/gamegate/publish_all.py          # every live box, whole library
```

**Why this rule exists, in one incident.** On 2026-08-30 a per-title publisher
wrote a **one-row** verdict file over the complete **38-row** file on **seven of
eight** boxes. The survivor was immaculate — same `# gamegate v1` header, same
columns, one valid verdict line — so every reader, human and machine, saw a
healthy file. Nothing errored, nothing warned.

What was actually lost were the **nine ollama adjudications**: `.124`/UT2004 and
`.171`/BF1942, JediAcademy, MaxPayne, SoF2, UT2004. Those are the marginal-band
calls a Pentium III **cannot recompute for itself** — they are the entire reason
a host publishes a file at all. Every box carried on gating correctly from its
local rules, which is precisely what made the loss invisible.

So:

* **Never write a partial file.** Merge into the existing one or regenerate the
  library; a "just my title" write is a clobber wearing the right header.
* **The file declares its own scope** (`# titles=N`) and the agent logs how many
  verdicts it loaded, warning when the file covers fewer titles than the library
  holds. Neither refuses the sync — the gate is unharmed — but the shrinkage is
  now *sayable* instead of silent.
* **Verify the post-condition after publishing**, always:

```bash
for f in /mnt/retro-share/Files/Games-Library/_gamegate/*.txt; do
  printf '%s  %s rows\n' "$(basename "$f")" "$(grep -vc '^#' "$f")"
done
```
  Every box should report the library's title count. A `1` is this bug.

## `--library /mnt/retro-share/...` cannot write — the mount is read-only

`/mnt/retro-share` is mounted **`ro`** in `/etc/fstab`, so pointing `--library`
at it and publishing dies with a bare traceback:

```
OSError: [Errno 30] Read-only file system
```

That is the mount, not a bug, and nothing about the message says so. Publish
through a fleet agent instead (`publish_all.py` does this), or through a
writable gvfs mount of the same share.

## `--title` MERGES; it never replaces

A narrowed publish reads the existing file and overlays only the titles it
re-decided. Before 2026-08-30 it wrote just those rows, which is how a one-title
pass left a one-row file on seven boxes. If you change this code path, keep the
merge: the verdicts it would otherwise drop are the **ollama adjudications**,
the only ones a fleet box cannot recompute for itself.

## Getting the file onto the share without corrupting its timestamp

The host mount is read-only, so publishing goes through a fleet agent — and
**`copy` propagates the SOURCE file's timestamp**. Writing a local temp on a box
and `copy`-ing it to the share stamps the file with **that box's clock**:
measured, `.124` is two hours fast and its files landed two hours in the future.

**Write straight to the share instead**, so the file server does the stamping:

```
UPLOAD Z:\Files\Games-Library\_gamegate\<hash>.txt      <- correct
UPLOAD C:\temp\x.txt  +  EXEC copy /Y ... Z:\...          <- box's clock
```

The agent's own file write is already `CreateFile`+`WriteFile`, so no agent
change is needed for the host-driven case. (Where the *agent itself* publishes
at startup with no host in the loop, there is no `UPLOAD` available and a direct
`CreateFile` on the share is the fix — see `agent/shared/hwpub.h`.)

Nothing currently reads these mtimes — the agent slurps the file's *content*,
and `_`-prefixed directories never enter GAMESYNC's size+mtime resume test
(`gamesync.c:2943`) — so the wrong stamp was harmless. It is worth getting right
anyway: the moment anything judges freshness by mtime, a two-hour-future file
is permanently "newest".

## The cache, and why it is keyed on hardware

`~/.retro-fleet/gamegate.db`, keyed
`(profile_hash, title, shortcut, requirements_version, model)`.

* **`profile_hash`, not an IP.** Two boxes built the same share one entry, a
  re-imaged box keeps its verdicts, and a box that gets a new graphics card
  correctly loses them. The agent computes it (`gg_profile_hash`) from hardware
  fields only, with the clock bucketed to 25 MHz and RAM to 16 MB — a measured
  clock wobbles a few MHz between polls, and a hash that moved on that would
  miss on every lookup, which is the same as having no cache.
* **`requirements_version`** so correcting a title's numbers invalidates its
  cached verdicts instead of letting them outlive the correction.
* **`model`** because a verdict is only as good as who gave it. A rule verdict
  is stored under the empty model, so swapping models throws away opinions and
  keeps arithmetic.
* **`decided_by`** (`rule` / `llm`) is stored so the cache is auditable at all —
  without it, "why is Doom 3 not on that box" has no answer.

## Model choice: `qwen3:14b`

Measured on this host's RTX 5090 against five real fleet/title pairs, with
ollama's JSON-schema `format` enforcement:

| model | strict JSON | agreed with a human | median latency |
|---|---|---|---|
| **qwen3:14b** | **5/5** | **4/5** | **0.4 s** |
| gemma3:12b | 5/5 | 4/5 | 0.7 s |
| gemma4:26b | 5/5 | 4/5 | 45.6 s |
| qwen3.6:27b | **0/5** | 0/5 | 1.0 s |
| qwen2.5-coder:7b-instruct | 5/5 | 1/5 | 0.2 s |

`qwen3.6:27b` returned an **empty response every time** under `format` — record
that so nobody re-tries it. `gemma4:26b` matched qwen3:14b's accuracy at a
hundred times the latency, emitted nonsense confidences (0.0, once −1.0) and one
word-salad reason. `qwen2.5-coder` is fast and confidently wrong — it called
511 MB "insufficient RAM" against a 128 MB minimum. `qwen3:14b`'s single miss
(UT2004 on an 845 MHz Pentium III: "no" where a human said "marginal") is a
defensible reading rather than a misunderstanding of the inputs.

**A malformed reply is retried, and then the deterministic verdict stands.** It
is never allowed to become "run" — that would make a broken model the most
permissive gate on the fleet, invisibly.

## Files

| file | |
|---|---|
| `SCHEMA.md` | the `requires.json` contract — read this before authoring one |
| `rules.py` | deterministic rules, mirror of `agent/shared/gamegate.h` |
| `llm.py` | ollama escalation, strict schema, retries |
| `cache.py` | SQLite verdict cache |
| `library.py` | reading the staged library |
| `gamegate.py` | the CLI |

## Tests

* `tests/native/test_gamegate.c` — the C rules (fail-open, the marginal band's
  both edges, hash stability, the non-monotonic NVIDIA ids, per-shortcut
  overlay, capabilities staying out of the verdict).
* `tests/python/test_gamegate_mirror.py` — compiles the C header and compares
  every answer against `rules.py`, so the two copies cannot drift.
* `tests/python/test_gamegate_host.py` — the cache's key behaviour, the LLM
  reply validator, and the escalation gate.
