# Retro Agent — Claude Code Instructions

## Work in a Worktree, Land on `master` at Every Checkpoint (REQUIRED)

**Do the work in a worktree; when it is stable and tested, commit and push it to
`master`. Do not ask first.** Several sessions run against this repo at once
(the DOS game-launcher session lives in `.claude/worktrees/dosgame-stability`),
so the worktree is what keeps you from fighting over one working tree — and the
push to `master` is what stops verified work rotting on a branch nobody merges.

**1. Take a worktree before you start.**
```bash
git worktree list                                        # who else is live?
git worktree add .claude/worktrees/<topic> -b worktree-<topic> origin/master
```
Do all editing, building and testing there. It leaves the main tree alone, lets
another session keep working, and makes a bad experiment trivial to throw away.
*Exception:* a trivial one-file edit (a doc line, a typo) can go straight in the
main tree if it is free — don't build ceremony around a one-liner.

**2. Test in the worktree before you call it a checkpoint.** `bash tests/run_all.sh`
green, or the fix verified on the hardware, or at minimum the thing you changed
exercised once doing what it should. A perfect suite isn't required; an untested
guess is not a checkpoint.

**3. At each checkpoint, land it on `master` and push:**
```bash
git fetch origin
git rebase origin/master        # keep it a fast-forward
bash tests/run_all.sh           # re-verify AFTER the rebase
git push origin HEAD:master     # land it - do not leave it on the topic branch
```
Then when the topic is finished: `git worktree remove .claude/worktrees/<topic>`
and `git branch -d worktree-<topic>`. Batch trivial follow-ups into the next
checkpoint rather than committing every keystroke, but **never end a session with
verified work uncommitted or unpushed** — a commit that isn't pushed is still
only on one machine.

**Guardrails, all of which have bitten here:**
- **Stage explicit paths.** `git add <paths>`, never `git add -A` — it sweeps up
  another session's in-progress work.
- **Never commit a whole-file line-ending (CRLF) churn.** Check
  `git diff --ignore-cr-at-eol --stat` before staging a file you didn't rewrite.
- **Never switch a branch another session has checked out** to satisfy this rule.
- **Shared index files still collide even across worktrees** — `tests/README.md`,
  `tests/run_all.sh` and this file are the usual casualties. Before editing one,
  `git -C <their-worktree> status --short` to see if it's already modified there;
  if it is, append in a distinct region and say so in the commit message.
- **If the push is rejected** as non-fast-forward, `git fetch && git rebase
  origin/master` and re-run the tests. If the *main* tree is too dirty to rebase,
  build the commit in a throwaway worktree off `origin/master` and push from
  there — never stash hundreds of someone else's files to land a small change.

The sibling repos (`retro-3dfx`, `nsc-assistant`) carry the same rule.

## Don't Stop With Unfinished Work (REQUIRED)

**Do not stop or hand back while there is unfinished work in scope.** If a task is
blocked, find another route to complete it (a different mechanism, a workaround, a
prerequisite fix) rather than declaring it blocked and stopping. Keep working
through the task list until everything the user asked for is actually done and
verified — only pause when genuinely blocked on something that requires the user
(a decision only they can make, a physical action, missing credentials). When a box
reboot or long operation is in flight, wait for it and continue; don't end the turn
early. Persist through multi-step, multi-reboot efforts to completion.

## Fleet Auto-Login (agent must survive reboots) (REQUIRED)

Every connected fleet box is configured for **Windows XP auto-login** so that after
any reboot it logs straight into the console session and the retro agent (an
`HKLM\...\Run` value `RetroAgent`) restarts automatically — no keyboard needed.
Keep this intact on every box; when you add or re-image a machine, apply the same.
Full doc: [`docs/fleet-auto-login.md`](docs/fleet-auto-login.md).

**Config (per box), in `HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon`:**
- `AutoAdminLogon` = `1`
- `ForceAutoLogon` = `1` (re-applies auto-login even after a manual logoff — the "fully enabled" bit)
- `DefaultUserName` = the box's **real console account** (MUST match an existing local account)
- `DefaultDomainName` = the box **hostname** (local account)
- `DefaultPassword` = `password` (fleet convention; plaintext, as XP auto-login requires)
- plus `HKLM\Software\Microsoft\Windows\CurrentVersion\Run\RetroAgent` = the agent exe.

**Convention:** the console account's Windows password is **`password`** on every box.
(Re)apply with `net user <account> password` **and** the five Winlogon values together
so the account password and `DefaultPassword` always match — that's what prevents a lockout.

**Per-box console accounts (2026-07):** .124/ADMIN=voidsstr (Voodoo3 ref, leave as-is) ·
.143/1GHZ (Voodoo5, untouched during driver session) · .123/2004-XP=Administrator ·
.240/USER-41EA3B3330=User · .145/DELL=voidsstr.

**Gotchas (hard-won):**
- `DefaultUserName` MUST be a real local account — set it from `echo %USERNAME%` (the
  actual console user), never an assumed name. Several boxes had a stale
  `DefaultUserName=admin` that doesn't exist → auto-login silently fails and the box
  won't come back after reboot.
- **Never set `DefaultPassword` without also `net user`-ing the account to the same
  value**, or the next reboot locks the box out (agent won't start → unreachable).
- The loopback test (`net use \\127.0.0.1\IPC$ /user:host\user password`) gives **false
  negatives** on XP (network-access policy) — a failure there does NOT mean the console
  password is wrong.

## Findings Log & Documentation Upkeep (REQUIRED)

**Keep a running findings log and keep docs current.** As you uncover any important,
hard-won finding (a gotcha, a root cause, a working method, a dead end that cost
time), immediately append it to the findings log
`/home/voidsstr/development/retro-3dfx/FINDINGS.md` (newest-first within its section;
this is the quick "don't forget this" index — detailed narratives go in the relevant
plan/design doc). **After every key milestone**, also update the affected
documentation (e.g. `retro-3dfx/D3D-DRIVER-PLAN.md`, skill `SKILL.md` files, the
memory files) so they reflect reality. Do this proactively, not only when asked.

## Driver Regression Tests (REQUIRED for every driver fix and deploy)

The 3dfx driver stack has a regression suite at
`/home/voidsstr/development/retro-3dfx/tests/` that encodes every
hardware-verified fix (source assertions, built-artifact/stale-obj checks, and
an on-target D3D matrix via the windowed `d3dlab.exe` lab).

- **Before deploying any driver binary** to a box, run
  `retro-3dfx/tests/predeploy.sh` — non-zero exit means do NOT deploy.
- **After deploy + reboot**, run `retro-3dfx/tests/run_target_tests.py` plus
  the OpenGL golden gate.
- **When a fix is verified on hardware, update the test suite in the same
  commit as the fix**: add a source assertion to `test_source_invariants.sh`,
  a binary marker to `test_built_artifact.sh` if applicable, and a target test
  (new `d3dlab` mode + golden in `tests/golden/d3dlab_golden.json`, or
  equivalent) so the fix can never silently regress. A fix without a
  regression test is not done.

## Regression Tests — Add One When a Fix Is Verified (REQUIRED)

**Our stack has a regression suite at `tests/` — keep it green and grow it as
fixes are verified.** Run it with `bash tests/run_all.sh`. It covers OUR stack:
Python client protocol/discovery tests, agent-C true-source tests, and MesaFX ICD
logic tests in `tests/native/`. It also invokes the sibling `retro-3dfx/tests/`
harness (the VINTAGE H5/SGL lane — see the Driver Stack Map) for a whole-machine
view, but the tests we own live under `tests/`. Everything runs natively on the
dev host in under a second — no hardware, no Wine. **Test the ICD your fix is
actually in** (MesaFX 0.1.x vs vintage SGL 0.3.x — see the Stack Map).

**When you verify a fix anywhere in the stack** (ICD, glide3x, display/D3D HAL,
agent, client, provisioning), immediately:
1. Add a test that encodes the fix's invariant — a `native/test_<fix>.c`
   (pure-logic/arithmetic invariant, citing the source file:function + fix
   version, asserting BOTH the fixed and the old-buggy value) or a
   `tests/python/test_*.py` case. See `tests/README.md` for the pattern.
2. Confirm `bash tests/run_all.sh` stays green.
3. Update `tests/README.md`'s fix→test table, and add a line here in CLAUDE.md
   if it's a milestone fix.

This is how we show reliability and catch a fix breaking under a later change.
Do this proactively, the same way you keep `FINDINGS.md` current.

## Driver Stack Map — KNOW WHICH STACK YOU'RE TOUCHING (READ FIRST)

> ### ⚠️ `.124` NO LONGER HAS A VOODOO 3 (hardware change 2026-08-11)
> The user pulled the Voodoo 3 out of **.124** and fitted an **NVIDIA GeForce2
> GTS** (`PCI\VEN_10DE&DEV_0150`, NV15), now running **ForceWare 71.89**. The
> entire 3dfx stack was purged from that box — display-class instances,
> `3dfxvs`/`3Dfx` services, the `OpenGLdrivers\3dfx` ICD registration, the OEM
> INFs, every `system32` 3dfx/glide DLL, the ghost `VEN_121A` devnodes, and 35
> game-local ICD copies. **Our clean-room stack (`voodoo-cleanroom/`) and the
> `voodoo3-driver-dev` skill therefore have no hardware behind them** until a
> Voodoo card goes back into a box. Everything below still describes the code
> lanes correctly; only the "`.124` = Voodoo 3" hardware claim is stale.
> Details + the driver-choice trap: `retro-3dfx/FINDINGS.md` (top entry).

There are **two different 3dfx codebases** in play. Do not conflate them — the
fixes, files, build tools, versions, and even the OpenGL renderer string differ.

### NEVER EDIT THE `retro-3dfx/` DRIVER CODE (REQUIRED, user directive 2026-08-04)

**`retro-3dfx/` is the Voodoo 5 lane (`.143` V5 5500 and `.133` V5 6000) and is NOT ours to modify.** In
this repo's work — and on **.124 (Voodoo 3)** specifically — write and ship
**only our own drivers in `retro-agent/voodoo-cleanroom/`** (MesaFX ICD, our
Glide, `vcr-disp`).

- **Do not edit** anything under `retro-3dfx/3dfx Driver Code/**` (H5 display
  driver, D3D/DDraw HAL, miniport, vintage SGL ICD, vintage Glide), do not
  build it, and do not deploy artifacts built from it.
- Treat that tree as **read-only reference** only: reading it to understand
  hardware behaviour or to port a *concept* into our clean-room code is fine;
  copying/patching its source is not.
- **A missing capability is not a licence to patch the vintage tree.** If our
  stack lacks something (e.g. `vcr-disp` has no full D3D HAL yet), the answer
  is to build it in `voodoo-cleanroom/`, or to tell the user it's missing —
  never to "just fix it in retro-3dfx".
- **Known outstanding conflict:** `.124` currently boots the vintage
  `3dfxv3d.dll` (H5 display driver) because our `vcr-disp` cannot yet drive 2D
  + D3D, and a 2026-08-03 D3D flip-present optimization was (wrongly) made in
  that tree and deployed there. Do not extend that work. Migrating `.124` onto
  an all-ours display driver is the open task; ask the user before touching the
  deployed vintage binary either way.

### 1. OUR open-source stack = `retro-agent/voodoo-cleanroom/` (this is "the driver we build")

A complete, self-built Voodoo stack from genuinely open source (3dfx's 2000 Glide
GPL release + MIT Mesa). Three components, all our forks/code — provenance in
`voodoo-cleanroom/FORKS.md`:

| Component | Fork / repo | Upstream | Source (local, gitignored clone) | Builds to |
|---|---|---|---|---|
| **OpenGL ICD (MesaFX)** | `voidsstr/retro3dfx-gl` | `sezero/MesaFX-6.2` (Brian Paul) | `voodoo-cleanroom/build/retro3dfx-gl/src/mesa/drivers/glide/fx*.c` | `voodoo-cleanroom/out/opengl32_retail.dll` (~2.7 MB) |
| **Glide** | `voidsstr/retro3dfx-glide` | `sezero/glide` | `voodoo-cleanroom/build/retro3dfx-glide/` | `voodoo-cleanroom/out/glide3x.dll` |
| **Display driver** | `voodoo-cleanroom/vcr-disp/` (OUR original code, GDI_DRIVER) | modeled on Device3Dfx/RISCyVoodoo/vmdisp9x | `voodoo-cleanroom/vcr-disp/*.c` | `vcr-disp.dll` |

- **Build:** `bash voodoo-cleanroom/build-stack.sh` once (builds glide + the gl SDK/headers),
  then `bash voodoo-cleanroom/build-mesafx-retail.sh` → `out/opengl32_retail.dll`.
  Toolchain: **mingw `i686-w64-mingw32-gcc` (gcc-13)**; flags
  `-O2 -ffast-math -march=pentium3 -mtune=pentium3 -mfpmath=sse`. ("retail" =
  links the retail AmigaMerlin glide import lib; the non-retail path links our
  `retro3dfx-glide`.)
- **Version:** `voodoo-cleanroom/VERSION` (0.1) + `.buildnum` → **0.1.N**;
  `GL_RENDERER = "Mesa Glide v0.62 ... [voodoo-cleanroom 0.1.N]"`.
- **Deploy:** to **.124** as game-local `opengl32.dll` / system32 `retrogl.dll`
  (**game-local shadows system32** — deploy to both or neutralize game-local when
  A/B-ing). See the `deploy-3dfx-driver` skill.
- **Tests:** `bash tests/run_all.sh` (Python client + agent-C + MesaFX ICD logic).
- **Fix versions live in `voodoo-cleanroom/CHANGELOG.md`** (0.1.x): fx_pack_ub SSE clamp
  (0.1.2), vertex cache (0.1.3), swap-interval (0.1.6), LOD-bias (0.1.11), Q2
  glide3x (0.1.19), gamma/dither/alpha-PFD (0.1.30), etc.

### 2. Vintage 3dfx source = the `retro-3dfx/` repo — VOODOO 5 (`.143` 5500 / `.133` 6000) ONLY, READ-ONLY HERE

3dfx's own leaked/released **H5/Napalm** driver source. It is a *different*
codebase from our open stack and, per the directive above, **off-limits for
editing/building/deploying from this repo** — listed here so you can recognise
its files and stay out of them:

- **Display driver + full D3D/DDraw HAL:** `retro-3dfx/3dfx Driver Code/H5/W2K/Src/Video/Displays/H5/`
  → `3dfxvs.dll` (Wine/**MSVC DDK**), WFP-renamed `3dfxv3d.dll` (.124) / `3dfxv5d.dll` (.143 and .133).
  **This is what currently provides the D3D HAL + 2D on .124** (our `vcr-disp`
  is the minimal cooperative driver, no full D3D HAL yet).
- **Vintage SGI/3dfx SGL OpenGL ICD:** `retro-3dfx/3dfx Driver Code/SWLIBS/OPENGL/GLIDE3X/`
  ("Copyright 1991-1997, Silicon Graphics, Inc.", `__glSST*` naming) → `opengl.dll`
  (MSVC, ~704 KB) / `3dfxogl.dll`. **Versions 0.2.x+** (0.4.x on .143, 0.5.x on
  .133); this is the **Voodoo5 "pure-3dfx" lane (the OTHER agent)** — .143 is the
  V5 5500, .133 "P3-DUAL" is the **V5 6000** (4-chip, 256MB mode verified;
  see `retro-3dfx/V56K-SLI-FINDINGS.md`) — NOT our MesaFX.
- **Tests:** `retro-3dfx/tests/` (source-invariants, `d3dlab` pixel goldens,
  predeploy gate) — for the H5 HAL + SGL ICD.

### Telling the two OpenGL ICDs apart (this is what tripped us up)

| | OUR MesaFX (retro3dfx-gl) | Vintage SGL (SWLIBS) |
|---|---|---|
| origin | Mesa 6.2.2 (Brian Paul) | SGI 1991-97 / 3dfx |
| files | `src/mesa/drivers/glide/fx*.c` | `SST_*.c`, `sst_export.c`, `__glSST*` |
| build | mingw gcc-13 | Wine/MSVC |
| size / version | ~2.7 MB / **0.1.x** | ~704 KB / **0.2.x–0.3.x** |
| renderer | `Mesa Glide v0.62 [voodoo-cleanroom 0.1.N]` | `[retro3dfx 0.2.x]` (the vintage lane's own brand) |
| lane | **.124 (ours)** | .143 (other agent) |

**Current .124 deployment is a HYBRID:** OUR MesaFX ICD (open) + retail AmigaMerlin
glide + vintage H5 display/D3D driver — converging toward the all-open stack
(retro3dfx-gl + retro3dfx-glide + vcr-disp). The vintage display driver is there
only because `vcr-disp` can't drive the box yet; **all NEW driver work goes into
`voodoo-cleanroom/`** (see the never-edit rule above).

**Before writing a driver test or "fixing" an ICD bug, confirm which ICD it's in**
(renderer string / file path / version number above). A 0.3.x fix in `SWLIBS`
is the vintage SGL lane, not our MesaFX.

## Session Startup — Chat Proxy Status Check (REQUIRED)

**On every Claude Code session start in this directory, IMMEDIATELY run the chat status check and echo a status message to the user.** Do this as your first action, before responding to any user message:

```bash
bash /home/voidsstr/development/retro-agent/scripts/chat_status.sh
```

The output will tell you and the user:
- Whether the `retro_chat_daemon.py` is running (✓ or ✗)
- How many retro agents are claimed
- Whether there are pending prompts in the inbox
- Whether the chat **brain** (processor) is alive (based on `processor.heartbeat`)

Echo a friendly status block to the user, e.g.:

```
Retro chat status:
  ✓ daemon running (pid 12345, claimed: .124, .143)
  ✗ chat brain: NOT RUNNING (or stale, last heartbeat 12 minutes ago)
  ✓ inbox: 0 pending prompts

To start the chat brain:  systemctl --user start retro-chat-brain
To restart the daemon:    bash /home/voidsstr/development/retro-agent/scripts/restart_daemon.sh
```

If the daemon is NOT running, offer to start it. If the brain is down, offer to start the service.

### The chat processor is now a standalone service (the "brain")

The processor that answers chat prompts is **`scripts/retro_chat_brain.py`** — a
long-running service built on the **Claude Agent SDK** (the same engine Claude
Code runs on). It replaces the old "spawn a Claude Code subagent" approach, so
the retro chat works **even when no Claude Code session is open**. It has the full
Claude Code tool suite on this host plus `mcp__retro__*` tools that operate the
fleet. Full docs: [`scripts/README-chat-brain.md`](scripts/README-chat-brain.md).

When the user asks "start the chat processor"/"start the chat brain":
```bash
systemctl --user start retro-chat-brain     # if the unit is installed
# or, without systemd:
nohup bash /home/voidsstr/development/retro-agent/scripts/retro_chat_brain_supervisor.sh \
  > /tmp/retro-chat/brain-sup.log 2>&1 &
```
Do **not** spawn an Agent-tool subagent for this anymore — the service is the processor.

### Fleetbook — the fleet's persistent memory of solved problems (USE IT)

**`scripts/retro_fleetbook.py`** is a SQLite knowledge base
(`~/.retro-fleet/fleetbook.db`) holding **recipes** (reusable fixes: problem →
symptoms → exact steps, with tags and usage counts) and **changes** (a
per-machine log of what changed on which computer, when, linked to the recipe
applied). The chat brain is prompted to search it before diagnosing and to
record every completed change; do the same in Claude Code sessions:

- `search <keywords>` → `show <id|slug>` — "have we solved this before?"
- `log --host <ip> --summary "..." [--recipe <slug>]` — after any fleet change
  (`--recipe` bumps that recipe's usage counters).
- `add --title ... --problem ... --recipe "steps" --tags a,b` — when a new fix
  is verified and reusable. `history --host <ip>` — what changed on a box.

Seeded 2026-08-03 with the classic fixes (vcache, ghost PCI, auto-login
lockout, WFP rename deploy, NETUP.BAT, manual agent-update swap, WASD binds,
bench quiesce, ...). Tests: `tests/python/test_fleetbook.py`.

### Deferred task queue (run-on-next-connect)

The daemon drains a **per-host task queue** whenever it (re)connects to a machine
(and on each idle cycle), so you can queue agent commands for a box that's
**offline now** and they run automatically when it next comes online. Plain file
storage under the daemon runtime dir: `/tmp/retro-chat/tasks/<ip>/*.json`
(pending) → `done/` (with captured output) or `failed/` (after 3 unreachable
tries). Queue with `scripts/retro_enqueue.py <ip> "<agent cmd>" [--label ...]`
(or the daemon's `--enqueue`/`--list-tasks` CLI); `retro_enqueue.py --list` shows
what's pending. A command that reaches the agent but errors still completes the
task; only network failures are retried. Full docs:
[`scripts/README-chat-brain.md`](scripts/README-chat-brain.md).

## Fleet AI engine (retro-infer) — OPT-IN ONLY (agent v1.17.0+)

**The AI engine `retro-infer.exe` must NOT run by default.** On the single-core
vintage fleet boxes it steals CPU from games and skews benchmarks. As of agent
**v1.17.0** it is gated behind a persisted registry flag and is **off unless
explicitly enabled through the retro chat interaction**:

- **Flag:** `HKLM\Software\RetroAgent\AIEngine` (REG_DWORD). Absent/0 = disabled
  (default). The agent's boot `ai_status_thread` and `infer_ensure()` both refuse
  to spawn the engine when it's 0 (`agent/src/ai.c:ai_engine_enabled()`).
- **Enable:** agent command **`AI_ENABLE`** (sets the flag + starts the engine).
  Over chat that's `mcp__retro__retro_command` with `command=AI_ENABLE` — this is
  the intended "enable via retro chat" path.
- **Disable:** **`AI_DISABLE`** (clears the flag + taskkills `retro-infer.exe`).
- Any AI command (`AI_HELLO`, `INFER_RUN`, …) still returns an error while the
  engine is disabled — enable it first.

**Benchmarking quiesces background CPU thieves.** The `driver-bench` skill's
`preflight()` now issues `AI_DISABLE` and taskkills `retro-infer.exe`,
`rotate_wall.exe`, `wuauclt.exe` (Windows Update), `3dfxMan.exe`, `daemon.exe`,
`wmiprvse.exe`, and stray `dwwin/dumprep` before running — a bench with any of
these live reads several fps low. If you bench by hand, do the same quiesce.

## DOS lane — DOSGAME (game manager) + DOSCHAT (agent+chat in one exe)

There is a **DOS lane** alongside the Windows fleet, for Win98's DOS 7.1, real
MS-DOS boxes, and DOSBox:

- **`scripts/dosgames/`** — `DOSGAME.EXE`, a 16-bit TUI that scans the drive
  for installed games, typeahead-searches the share's ~3,000-title DOS catalog,
  installs games with scripted steps over the LAN (mTCP `HTGET` → `UNZIP` →
  optional `INSTALL.EXE`), and shows VGA mode-13h gameplay preview tiles.
  Host side: share survey, catalog generator, tile renderer, and an HTTP bridge
  (`serve_dosgames.py`, systemd user unit `retro-dosgames-http` on :8181)
  because DOS can't read the SMB share's long filenames.
- **`agent/doschat/`** — `DOSCHAT.EXE`, the retro **agent and chat in a single
  real-mode exe**: the same framed protocol on 9898 + discovery on 9899, so
  `retro_chat_daemon.py` claims a DOS box like any other, with the chat UI in
  the same process.

**The agent auto-stages both DOS programs on DOS-capable boxes** (agent
**v1.19.0+**, `agent/src/dosstage.c`). On a Windows 9x/ME machine — the ones
that actually boot DOS 7.x — a startup thread copies `doschat\` and `dosgame\`
from the share into `C:\DOSCHAT` and `C:\DOSGAME` (plus `C:\DOSGAME.BAT`), so
the DOS side is ready without a manual copy. It is a **no-op on the NT family**
(XP has no DOS to boot into, and the tile payload is ~11 MB). It's idempotent
(same-size files are skipped, so a reboot costs one directory scan) and runs at
below-normal priority after a 45s delay. **The ~11MB preview-tile payload is
opt-in** (`DosStageTiles`=1) and staging is **skipped entirely below 6MB free
RAM**: on the Pentium-1 Deskpro (31MB, 0MB free) copying the tiles killed the
agent outright, ~45s after every start, which looked for hours like a startup
crash. A cosmetic feature must never cost a box its agent. On demand: **`DOSSTAGE`**
(`DOSSTAGE force` ignores the off-switch, never the OS gate).
Registry (`HKLM\Software\RetroAgent`): `DosStage` DWORD 0 disables,
`DosStageTiles` DWORD 1 opts into the preview tiles, `DosStagePath` overrides
the source share, `DosStaged` records the last run.

**A box running the chat client locally needs client slots for BOTH.**
`retro_chat` holds three connections (command + log poll + status poll) and the
daemon needs two (wait + send). `MAX_CLIENTS` was 4, so on the Win98 box the
daemon could not attach — nobody polled its prompts and typing into its chat
produced no reply, while any operator got `ERR max connections reached`. Raised
to 10 in agent **v1.22.0**; a slot is ~32 bytes.

**Discovery must tolerate a slow box.** The daemon probed each host with
1.5–2s timeouts, fine for XP but not for the single-threaded Pentium-1, which
therefore never got claimed. Now 8s (the whole /24 is probed concurrently, so
it costs wall-clock only on the slowest host).

**Queued tasks expire after 24h.** A `QUIT` enqueued on 2026-07-29 fired the
moment that box was re-claimed on 08-03 and took its agent straight down.
Queued work means "run when the box next appears", not "run forever" — and use
`RESTART`, never `QUIT`.

**Win9x agents are single-threaded (multiplex mode)** — one thread serves every
client. Long-polls are therefore clamped to 1s there (`g_longpoll_max_ms`);
without that, the local chat client's 30s `LOG_WAIT`/`STATUS_WAIT` starved every
other client and the box was unreachable from the network while happily serving
localhost. If a 9x box "accepts but never answers", suspect starvation, and get
its log via `retro_agent.exe -l <path on the share>` rather than inferring from
port behaviour.

**Shared code lives in `agent/shared/`** — `frameproto.h` (wire constants, also
included by `src/protocol.h`), `chatcore.[ch]` (prompt slot / log ring / status
sequence; `src/chatproxy.c` wraps this same engine in its NT locks and events),
`chattext.h` (sanitize + word wrap, shared with `tools/retro_chat.c`). **Change
the shared module, not one copy** — `tests/native/test_chatcore.c` and
`tests/python/test_doschat_shared.py` enforce this.

Toolchain (Open Watcom + mTCP + DOSBox-X under Wine) lives outside the repo in
`~/development/toolchain-dos/`; the traps that cost real time (case-sensitive
quoted includes, the 64K DGROUP/socket-malloc ceilings, the mandatory
`$(TCPOBJS): doschat.cfg` dependency) are documented in
[`scripts/dosgames/README.md`](scripts/dosgames/README.md) and
[`agent/doschat/README.md`](agent/doschat/README.md). Read those before
touching a DOS build.

## Host services — what must be running on the dev host (192.168.1.132)

The fleet depends on a handful of long-running services on this host. All of
them are now reported on the **GDM login-screen status wall**
([`dashboard/README.md`](dashboard/README.md)), which is the fastest way to see
whether anything is down — walk past the monitor.

| service | scope | what it does |
|---|---|---|
| `retro-chat-daemon` | `--user` | LAN bridge that claims retro agents |
| `retro-chat-brain` | `--user` | the Claude Agent SDK processor answering chat prompts |
| `retro-gameindex` | `--user` | **the favourites agent** — keeps every box's in-game server list full of live servers |
| `retro-gameservers-watch` | `--user` | **game-server watchdog** — probes all 10 servers every 20s and restarts what dies |
| `retro-dosgames-http` | `--user` | HTTP bridge for the DOS game catalog |
| `retro-pxe` | system | proxyDHCP + TFTP for network-installing the fleet |
| `retro-dashboard-collector` | system | gathers everything into `/run/retro-dashboard/state.json` |

```bash
systemctl --user status retro-gameindex retro-gameservers-watch
python3 scripts/game-servers/gameservers.py          # every game server, right now
python3 scripts/gameindex/sync.py --status           # what the favourites DB knows
```

**`systemctl --user` as root is the wrong manager.** Anything running as root
(the collector, a sudo shell) that wants to inspect these must drop to
`voidsstr` with `XDG_RUNTIME_DIR=/run/user/1000` set. A bare `systemctl --user`
under root queries *root's* manager, which holds none of them, so every service
reads "not found" — indistinguishable from every service having died.

**"Not installed" and "crashed" must never render the same.** This bit us
repeatedly while building the wall: `LoadState=not-found` (never installed),
`unknown` (could not ask the manager) and `failed` (it died) are three
different calls to action, and only the last is a fault.

### The favourites agent is a service, not a timer

`retro-gameindex` was a `oneshot` behind a 5-minute `.timer`, which meant its
unit read `inactive (dead)` for 297 of every 300 seconds. It is now a
long-running `Type=simple` daemon doing the same 5-minute pass. **The timer is
deleted** — leaving it enabled starts a second pass that fights the daemon over
the same SQLite file.

It publishes a per-pass report to
`$XDG_RUNTIME_DIR/retro-gameindex/status.json`, because **the fleet is powered
on demand**: a healthy pass across zero live boxes writes nothing and logs
almost nothing, so judged by output volume a healthy agent looks dead every
time the retro machines are switched off.

### The game servers have two process managers

Nine are `systemd --user` units; **Tribes 2 is a docker container**
(`tribes2-server`), because it needs a 2001 userland. Anything enumerating the
game servers must ask the right manager — `systemctl show tribes2-server`
returns `not-found` and silently drops a running server off the board.
`scripts/game-servers/gameservers.py` declares each row's `manager` and
dispatches; use it rather than assuming systemd.

**`rtcw-server` and `mohaa-server` have never existed on this host** — they are
in the game-servers *skill's* table as a wish list. Do not treat their absence
as a regression, and do not add them to the status table until they are really
installed, or the wall shows a permanent outage.

**Bots are not players.** The Q3 server runs `bot_minplayers 4`, so any player
count that does not separate bots claims someone is playing 24/7. GoldSrc's
A2S reply carries a bot count; on the Quake family a player line with **ping 0**
is a bot. Tribes 2 reports no count at all (TribesNext encrypts the info
response) — that is `—`, never `0`.

## Repository Context

This repo was extracted from the `nsc-assistant` monorepo. The dashboard, MCP server, and OpenClaw agents remain in `nsc-assistant`. This repo contains only the agent binaries, Python client library, provisioning scripts, and documentation.

The `nsc-assistant` dashboard still imports the Python client (`shared/retro_protocol.py` and `shared/retro_discovery.py`). Those files are duplicated here as `client/`. When modifying the protocol, update both repos.

## Build & Deploy

### Versioning (REQUIRED)

**ALWAYS bump the version when you build a new `retro_agent.exe` or
`retro_chat.exe`.** The version comes from the **highest git tag matching `v*`**;
the Makefile injects it into the binary via `-DAGENT_VERSION`. A build that
carries the *previous* version's tag is a broken build — never ship one.

- **Preferred:** `make release` — bumps the tag, builds, and uploads (binary +
  `.ver` sidecar) in one step. Patch bump by default; `make release BUMP=minor|major`.
- **Manual build:** `git tag vX.Y.Z` **before** `make` (the tag must exist at
  build time so it's compiled in). Verify with `SYSINFO` (`agent_version`) or by
  reading the sidecar `.ver` after publish.

Bump rule: **new command or feature = minor bump; bug fix = patch bump.** Major
is reserved for protocol-breaking changes.

> **Check `git tag -l 'v*'` before you build.** The Makefile takes the *highest
> existing tag*, so a clone with missing tags compiles an **older** version
> number onto newer source. On 2026-08-11 this clone's tags stopped at `v1.9.2`
> while the source was `v1.25.1` — a bare `make` would have stamped 1.9.2, and
> because auto-update pulls on version **inequality** (not "remote is newer"),
> publishing it would have downgraded every box on the fleet. Guarded by
> `tests/python/test_agent_version.py`; if that test fails, create the missing
> tag rather than editing the test.

### Publishing builds to the share (REQUIRED)

**Any time you build a new `retro_chat.exe` or `retro_agent.exe`, immediately
publish it to the SMB share so the fleet auto-updates.** A build that isn't on
the share reaches no machine.

**Canonical deploy location** — the **latest pointer**
`\\192.168.1.122\files\Utility\Retro Automation\retro_agent.exe` is exactly what
every agent's auto-update reads. (Chat client: `…\retro_chat.exe`.)

> **This path is hardcoded in the agent (`agent/src/autoupdate.c:39`) and a share
> rebuild WILL silently kill fleet-wide auto-update.** It happened on 2026-08-11:
> the rebuilt share had no `Utility\Retro Automation\` at all, so every box was
> stranded on whatever binary it already had, with no error anywhere. **After any
> share rebuild, recreate this directory first** (latest pointer + `.ver` sidecar
> + `retro_agent/` archive + the chat pair) and confirm with a `dir`. A box with
> the share mapped can restore it: `copy` from `C:\RETRO_AGENT\retro_agent.exe`
> and `UPLOAD` the `.ver` files.

Publish **three** things for each build:
- the **latest pointer** — `…/Retro Automation/retro_agent.exe` (what auto-update
  reads),
- a **versioned archive** copy — `…/Retro Automation/retro_agent/retro_agent_vX.Y.Z.exe`
  (for rollback), and
- a **`.ver` sidecar** — `…/Retro Automation/retro_agent.exe.ver`, a plain-text
  file containing just the **bare** version string that matches `AGENT_VERSION`
  (e.g. `1.6.0`, no `v` prefix). Auto-update reads this to decide whether to pull
  (see below), so it **must** be updated in lockstep with the latest pointer.
  (Chat client sidecar: `…\retro_chat.exe.ver`.)

**Auto-update decides by VERSION, not size.** On startup the agent compares its
compiled `AGENT_VERSION` against the share's `retro_agent.exe.ver`; a mismatch
triggers a pull. It falls back to the old **file-size** comparison only when no
`.ver` sidecar is present. So a version bump **always** propagates — even when the
rebuilt binary is byte-for-byte the same size. (Previously the agent compared size
only, so a same-size bump silently failed to propagate — this is why the `.ver`
sidecar and the "always bump" rule are mandatory.) A distinct remote version is
attempted at most once (`HKLM\Software\RetroAgent\LastUpdateVer` guard), so a
mispublished `.ver`/`.exe` mismatch can't loop the fleet — but keep them in sync.

Two ways to publish:
1. **`make release`** (in `agent/tools/` for the chat client, `agent/` for the
   agent) — bumps the version tag, builds, and uploads both. Needs `SMB_CREDS`
   set in the Makefile.
2. **Via an online fleet agent** when SMB creds aren't handy locally: build, then
   `UPLOAD` the binary to a machine (e.g. `C:\RETRO_AGENT\retro_chat.exe`) and
   `EXEC cmd /c copy /Y` it to the share's latest pointer **and** the versioned
   path. The fleet machines have the share mapped with write access (the `Z:`
   drive), so the copy succeeds from there.

Also commit the rebuilt binary to git (`agent/tools/retro_chat.exe` is tracked).
The fleet picks up the new build on each machine's next agent restart/reboot.

> The Retro Chat **brain/daemon** (`scripts/retro_chat_brain.py`,
> `retro_chat_daemon.py`) are server-side Python — they do **not** ship to the
> fleet, so changing them needs no share publish (just restart the systemd
> services: `systemctl --user restart retro-chat-brain retro-chat-daemon`).

### Windows Agent

```bash
cd agent && make clean && make
# Output: agent/retro_agent.exe

# Upload to SMB share (distribution point for all retro machines)
curl --upload-file agent/retro_agent.exe -u YOUR-CREDS \
  "smb://YOUR-SERVER/files/Utility/Retro%20Automation/retro_agent.exe"
```

`make release` — bumps patch version, tags, builds, uploads to share in one step. `make release BUMP=minor|major` for minor/major bumps.

After building the agent, also rebuild the `nsc-assistant` dashboard (it embeds the agent binary for the "Update Agent" button):
```bash
cd /home/voidsstr/development/nsc-assistant && docker compose up -d --build dashboard
```

#### Dual-boot swap gotcha (discovered on .124)

Some fleet boxes are **Win98/XP dual-boot**, and the running XP agent binary can
live on the **C: (Win98) volume** even though `%SystemDrive%` is **D:**. Do not
assume the exe is under `%SystemDrive%`.

- Confirm the running exe path before swapping:
  `EXEC wmic process where "name='retro_agent.exe'" get ExecutablePath`.
- **You cannot overwrite a running exe on Windows.** Either move-aside then copy
  (`EXEC cmd /c move /Y retro_agent.exe retro_agent.exe.old & copy /Y new.exe retro_agent.exe`),
  or just let auto-update do the swap on next restart.
- **Use `RESTART` (agent v1.20.0+) for a remote agent restart, never `QUIT`.**
  Nothing supervises the agent on Win9x — the `RetroAgent` Run key only fires
  at logon — so a bare `QUIT` takes the box off the network until someone
  walks over to it. `RESTART` writes and spawns a detached relaunch batch
  *before* stopping. (Pre-1.20.0 shutdown also left the alt listener `:9897`
  bound, so a quit agent kept ACCEPTING connections while answering nothing —
  the box looked reachable but was unusable. Both fixed in 1.20.0.)
- **To restart the running agent, use `EXEC` + a detached batch, not `LAUNCH`**
  (on .124 `LAUNCH` returns a PID but does not actually execute the child): EXEC a
  `.bat` that does `taskkill /f /im retro_agent.exe` then `start "" …\retro_agent.exe`
  — the orphaned batch survives the agent's death and relaunches it. Same trick
  runs GUI games (`EXEC cmd /c cd /d "<dir>" ^&^& start "" game.exe …`).

### EXECW — bounded long-running commands

`EXEC` has a fixed 60s timeout. **`EXECW <seconds> <command>`** runs the same
hidden-capture exec with a caller-chosen timeout (clamped to 15 min) and
tree-kills the child on timeout (marker: `[EXECW: timed out, process tree killed]`).
Use it for slow steps (game/benchmark launches, installers) instead of the
`LAUNCH`+sleep+reconnect+kill dance. Added in v1.6.0.

### Linux Agent

```bash
cd agent-linux && make clean && make
curl --upload-file agent-linux/retro_agent_linux -u YOUR-CREDS \
  "smb://YOUR-SERVER/files/Utility/Retro%20Automation/retro_agent_linux"
```

## Fleet Onboarding (on demand, via chat/skill — NOT at startup)

Onboarding maps the share, installs a **hardware-appropriate** game set (games
the box can't run are skipped), applies the desktop/wallpaper, and marks
`HKLM\Software\RetroAgent\Onboarded`. As of **agent v1.16.0 it is triggered on
demand, not at agent startup** — on old, slow hardware (a Pentium-1 Compaq
Deskpro 2000) the first-boot SMB copy/extract saturated the box for minutes and
made the agent look hung, so the boot path is now kept lightweight. Trigger it
with the **`ONBOARD`** agent command (`ONBOARD force` to re-run an
already-onboarded box); over chat that's `mcp__retro__retro_command` with
`command=ONBOARD`. Full workflow: the **`onboard-machine` skill**
(`.claude/skills/onboard-machine/`). `agent/src/onboard.c:onboard_run()` does
the work in a background thread; it's a no-op until the payload is published.

- **Dual dialect + hardware gating:** `provisioning/gen_onboard.py` emits BOTH
  `onboard.cmd` (NT/XP cmd.exe) and `onboard_9x.bat` (Win98 COMMAND.COM — no
  cmd.exe on 98); the agent picks the right one per OS and runs it with the
  right shell (`command.com /c`, no `2>&1`, on 9x). Each game in
  `onboard.json` declares `requires` capability flags (gpu3d/cpufast/ram64/
  ram128); the agent detects the box's hardware (`onboard.c:set_capability_env`,
  via GetSystemInfo wProcessorLevel / EnumDisplayDevices / GlobalMemoryStatus)
  and sets `ONB_*` env vars the batch gates on. A P1+2D box (Deskpro 2000) gets
  no games (all `[HWSKIP]`), just wallpaper.
- Edit the game list in `provisioning/onboard.json`, regenerate with
  `python3 provisioning/gen_onboard.py`, publish control files via
  `provisioning/push_onboard.py <online-agent-ip>`, and drop per-game ZIPs into
  the share's `…\Games\` dir. Full docs: [`provisioning/README.md`](provisioning/README.md).
- Uses the `copy /Y` + JScript `retro_unzip.js` extract pattern (NOT `xcopy` -
  it hangs on NETMAP'd SMB on XP); sets the marker via `regedit /s` (no `reg.exe`
  on Win98). Idempotent - reruns skip already-installed games.
- **`onboard.cmd` gotcha (fixed):** game NAMEs contain parentheses (e.g.
  `(BC Romania)`, `(fleet build)`), so the `:game` routine must NOT echo `%NAME%`
  inside a `( ... )` block — the `)` in the expanded name closes the block early
  and cmd aborts with `- was unexpected at this time.` (onboarding then never
  completes: no theme, no Onboarded flag). `gen_onboard.py` emits goto-based flow
  with plain-line echoes instead. If you edit the generator, keep it paren-safe.

### Desktop theme + icons are (re)applied on EVERY startup (not just onboarding)

The dark "hacker" system-color theme, the dossier wallpaper, and the parked-icon
layout are applied by the agent's **`retrowall` thread on every startup** (v1.8.0+,
`agent/src/retrowall.c`) — not only on first-run onboarding — so a box keeps the
fleet look across reboots. It applies whatever the **retro-wallpaper skill** has
staged into `C:\retro-wall\`:
- `wall00..NN.bmp` + `rotate_wall.exe` → wallpaper rotation
- `arrange_icons.exe` → icons parked in the bottom-right well
- `retro_theme.reg` + `setsyscolors.exe` → dark green-on-black system colors
  (regedit writes `HKCU\Control Panel\Colors`, then `setsyscolors.exe` pushes
  them live via `SetSysColors` so it takes effect without a re-logon)

Each step is a **no-op if its asset isn't staged**. Stage/refresh all of them
with `python3 scripts/retro-wallpaper/deploy_rotation.py <ip>` (it now also stages
`retro_theme.reg` + `setsyscolors.exe`). To fix a single box's theme immediately
without a restart: `python3 scripts/retro-wallpaper/apply_hacker_theme.py <ip>`
(`--revert` restores Luna).

## Using the Agent from an LLM

The retro agent is operated by LLMs through the Python client library in `client/`. An LLM connects to agents over TCP, sends commands, and processes responses — enabling autonomous diagnostics, software installation, hardware configuration, and GUI automation on retro PCs.

### Connecting

```python
import asyncio
from client.retro_protocol import RetroConnection

async def run():
    conn = RetroConnection('10.0.0.50', 9898)
    await conn.connect('retro-agent-secret', timeout=15.0)
    # ... send commands ...
    await conn.close()

asyncio.run(run())
```

When running from the `nsc-assistant` repo, use `from shared.retro_protocol import RetroConnection` instead.

### Command Patterns

```python
# Text command — returns string, raises RetroProtocolError on error
text = await conn.command_text('SYSINFO')

# Binary command — returns bytes (screenshots, file downloads)
bmp_data = await conn.command_binary('SCREENSHOT 0')

# Raw command — returns (status_byte, data_bytes)
status, data = await conn.send_command('EXEC dir C:\\WINDOWS')

# Upload — two-frame protocol (command + binary payload)
await conn.send_command('UPLOAD C:\\path\\file.reg', binary_payload=reg_bytes)
```

### LLM Diagnostic Workflow

A typical LLM-driven diagnostic session:

1. **Connect**: `RetroConnection(host, 9898)` → `connect(secret)`
2. **Assess**: `SYSINFO`, `VIDEODIAG`, `AUDIOINFO`, `PCISCAN` for hardware inventory
3. **Investigate**: `REGREAD` for driver config, `PROCLIST` for running processes, `DIRLIST` for files
4. **Fix**: Upload `.reg` files via `UPLOAD` + `EXEC regedit /s`, copy drivers from share via `EXEC copy`, apply `SYSFIX`
5. **Verify**: Re-run diagnostic commands, take `SCREENSHOT` to confirm
6. **Reboot** if needed (only with user approval — machines may require physical access)

### LLM GUI Automation Workflow

For installing drivers/software with a GUI:

1. Upload or copy installer to machine
2. `LAUNCH installer.exe` (never EXEC for GUI apps — it runs them hidden)
3. **Screenshot-click loop**: `SCREENSHOT 0` → analyze with vision → `UICLICK x y` or `UIKEY keyname`
4. Post-install cleanup: upload `.reg` to delete broken Run keys, shell extensions
5. `REBOOT` and verify with `VIDEODIAG`/`AUDIOINFO`

### LLM Fleet Management

```python
from client.retro_discovery import discover_retro_pcs

# Find all agents on the LAN
pcs = await discover_retro_pcs(timeout=3.0)
for pc in pcs:
    conn = RetroConnection(pc.ip, pc.port)
    await conn.connect(secret)
    # Run inventory, apply updates, check health
    await conn.close()
```

### Critical Rules for LLM Usage

- **One connection at a time** per agent. The agent is single-threaded. Close connections promptly.
- **EXEC = CLI only** (hidden, blocks, captures output). **LAUNCH = GUI only** (visible, returns PID). Mixing them hangs the agent.
- **Win98 shell escaping**: `<>` in `echo` commands are interpreted as redirects by `command.com`. Use `UPLOAD binary_payload` to write files with special characters instead.
- **FILECOPY delimiter**: `FILECOPY src|dst` (pipe, not space).
- **REGREAD format**: `REGREAD HKLM Path\\To\\Key` (root and path space-separated).
- **Screenshots are raw BMP**: Convert to PNG with Pillow before passing to a vision model or saving.
- **REBOOT/SHUTDOWN require confirmation**: These machines need physical access to recover. Never issue without explicit user approval.
- **Win98 RST crash**: Abrupt TCP disconnects crash Win98 Winsock. Always `await conn.close()` gracefully. Never kill connections to Win98 agents.

## Agent Command Reference

### System Info
- **PING** — returns "PONG"
- **SYSINFO** — JSON: CPU, memory, OS, drives, uptime
- **VIDEODIAG** — video card, driver, PCI IDs, resolution, DirectX
- **AUDIOINFO** — audio device enumeration
- **SMARTINFO** — S.M.A.R.T. disk health
- **DISPLAYCFG** — display config and refresh rate
- **PCISCAN** — PCI device enumeration with vendor/device IDs

### Execution
- **EXEC cmd** — run hidden, capture output, block until exit (60s timeout)
- **LAUNCH cmd** — run visible, return immediately with `{pid, command}`

### Process Control
- **PROCLIST** — JSON list of running processes
- **PROCKILL pid** — terminate process by PID
- **QUIT** — stop agent gracefully (for updates)
- **SHUTDOWN** — power off machine
- **REBOOT** — restart machine

### File Operations
- **DIRLIST path** — JSON directory listing
- **UPLOAD path** — upload file (two-frame: command + binary payload)
- **DOWNLOAD path** — download file (binary response)
- **MKDIR path** — create directory (recursive)
- **DELETE path** — delete file
- **FILECOPY src|dst** — copy file (pipe-delimited)

### UI Automation (Windows)
- **SCREENSHOT quality** — raw 24-bit BMP (0=full, 1=half, 2=quarter)
- **SCREENDIFF [FULL]** — dirty-tile delta vs the agent's previous frame (only
  changed 64×64 tiles; `FULL` forces a baseline). Real-time screenshots.
- **CLICKSHOT x y [right|dbl] [settle_ms]** — click, settle, and return a
  `SCREENDIFF` delta in ONE round trip (agent **v1.18.0+**). The real-time
  click→result primitive.
- **UICLICK x y [button]** — click at coordinates (left/right/middle)
- **UIKEY keyname** — send keystroke (uses MapVirtualKey scan codes)
- **WINLIST** — JSON list of visible windows

For fast, real-time button-clicking installs (single box or many in parallel),
use the **`gui-install` skill** (`.claude/skills/gui-install/`): `FastUI` holds one
persistent connection and drives `CLICKSHOT`/`SCREENDIFF` deltas.

### Registry (Windows)
- **REGREAD root path** — read value or enumerate keys
- **REGWRITE root path name type data** — write value. **Five tokens.** The
  value name is its own argument, *not* part of the path, and the data comes
  last: `REGWRITE HKLM SYSTEM\CurrentControlSet\Services\fxgpio Start REG_DWORD 1`.
  Folding the name into the path (`...\fxgpio\Start 1 REG_DWORD`) makes the
  agent **create a subkey** called `Start`, write a value named `1` into it,
  and still answer `OK` while the real value is untouched
  (`agent/src/registry.c:284`). **Always read the value back — never trust the
  `OK`.** Cost an hour on .171 (2026-08-28).
- **REGDELETE root path** — delete value or key

### Network (Windows)
- **NETMAP unc [drive] [user] [password]** — map network share
- **NETUNMAP drive** — disconnect mapped drive

### Hardware (Windows)
- **DRVSNAPSHOT** — capture driver configuration state
- **SYSFIX [check|apply]** — check/apply Win98 system fixes

### Linux-Only
- **PKGINSTALL name** — install package (auto-detects apt/yum/pacman)
- **PKGLIST** — list installed packages
- **SVCINSTALL** — manage systemd services

## Taking Screenshots

Agent returns raw 24-bit BMP. Always convert to PNG:

```python
import os
from PIL import Image

data = await conn.command_binary('SCREENSHOT 0')
os.makedirs('/tmp/retro-screenshots', exist_ok=True)
with open('/tmp/retro-screenshots/screen.bmp', 'wb') as f:
    f.write(data)
img = Image.open('/tmp/retro-screenshots/screen.bmp')
img.save('/tmp/retro-screenshots/screen.png', optimize=True)
# Now read screen.png to view
```

Zoom into regions for detail: crop with Pillow and resize with `Image.NEAREST`.

## Win98 Known Issues & Fixes

### SYSFIX Command

Built into the agent. Always run `SYSFIX apply` on any new Win98 machine.

- `SYSFIX check` — report issues (read-only)
- `SYSFIX apply` — fix all issues

### vcache (Critical)

Win98 with >512MB RAM and no `MaxFileCache` limit: disk cache exhausts VxD address space → "Windows Protection Error" on boot. Looks like a driver problem but it's a memory bug. `SYSFIX apply` adds `MaxFileCache=262144` to SYSTEM.INI.

### Ghost PCI Entries

Removed hardware leaves registry entries that block PnP. `PCISCAN` shows ghosts. Delete via `.reg` file upload + `EXEC regedit /s`.

### Registry .reg Files on Win98

Win98 has no `reg.exe`. Write `.reg` files and apply with `EXEC regedit /s`:

```python
# Delete a registry key
reg = b'REGEDIT4\r\n\r\n[-HKEY_LOCAL_MACHINE\\Path\\To\\Key]\r\n'
await conn.send_command('UPLOAD C:\\WINDOWS\\TEMP\\fix.reg', binary_payload=reg)
await conn.command_text('EXEC regedit /s C:\\WINDOWS\\TEMP\\fix.reg')
```

### Win98 RST Crash

Abrupt TCP disconnects crash Win98 Winsock, taking down the whole machine. Always close connections gracefully. Never stop a dashboard/proxy while Win98 agents are connected.

## Debugging & Direct Agent Access

Connect directly over TCP for raw protocol access:

```python
import asyncio
from client.retro_protocol import RetroConnection

async def run():
    c = RetroConnection('AGENT_IP', 9898)
    await c.connect('retro-agent-secret', timeout=15.0)
    status, data = await c.send_command('COMMAND_HERE')
    print(data.decode('ascii', errors='replace'))
    await c.close()

asyncio.run(run())
```

### Finding Agents on the LAN

```python
async def scan_subnet():
    tasks = []
    for i in range(1, 255):
        tasks.append(try_host(f'10.0.0.{i}'))
    await asyncio.gather(*tasks)

async def try_host(ip):
    try:
        c = RetroConnection(ip, 9898)
        await c.connect('retro-agent-secret', timeout=2.0)
        status, data = await c.send_command('SYSINFO')
        print(f'{ip}: {data.decode("ascii", errors="replace")[:100]}')
        await c.close()
    except Exception:
        pass
```

## Remote Driver Installation

See the case studies in `docs/case-studies/` for detailed real-world examples of:
- Ghost PCI device cleanup and driver installation (Voodoo3)
- vcache diagnosis and NVIDIA driver installation (GeForce2 GTS)

General pattern:
1. `SYSFIX apply` (always first on Win98)
2. Clean ghost PCI entries via `PCISCAN` + `.reg` file
3. Stage driver files (UPLOAD or copy from SMB share)
4. `LAUNCH` installer, walk GUI via screenshot-click loop
5. Post-install registry cleanup (broken Run keys, CPL extensions)
6. `REBOOT` and verify with `VIDEODIAG`

### 3dfx Driver Installation (Voodoo 3/4/5) — Critical Notes

**DO NOT manually create Display class registry entries for 3dfx drivers.** Unlike NVIDIA, the 3dfx VxD (`3dfxvs.vxd`) requires full Win98 PnP context to initialize the hardware. Manually creating `Display\0000` entries results in VIDEODIAG showing `status: OK` but the display stays at 640x480 4-bit VGA — the VxD never actually initializes and the driver silently falls back.

**DO NOT add `device=3dfxvs.vxd` to `[386Enh]` in SYSTEM.INI.** It's a minivdd (miniport) loaded by `*vdd` during PnP, not a standalone VxD. Loading it via `device=` causes a Windows Protection Error.

**The Amigamerlin `Driver Install.exe` only copies files and INFs** — it does NOT create registry entries or configure PnP.

**What actually works:**
1. `SYSFIX apply`, clean ghost PCI entries from old cards
2. Copy Amigamerlin package to machine from SMB share
3. Run `Driver Install.exe` via `LAUNCH DRIVER~1.EXE` (use short name — spaces in filenames break LAUNCH on Win98)
4. Delete competing old INFs from `C:\WINDOWS\INF\OTHER\` (keep only the Amigamerlin `Voodoo.inf`)
5. Reboot → Win98 PnP detects card → prompts "Insert disk labeled 3dfx Voodoo driver installer disk"
6. Point the disk prompt to the `driver9x\` directory (e.g., `C:\WINDOWS\Desktop\am29win9x\driver9x`) which has `Voodoo.inf` and all driver files
7. Let PnP complete → reboot again → driver activates at correct resolution

**Amigamerlin 2.9 — the Win9x reference baseline.** Covers Banshee/Voodoo3/4/5;
INF section `Driver.InstallV3` for Voodoo3, `Driver.InstallV5` for Voodoo4/5.
**It is Win9x-only** (`am29win9x.exe`) — on Win2000/XP use **Amigamerlin 2.5 SE**,
which ships the `driver2K\` set.

Treat Amigamerlin as the **stable third-party yardstick to measure our two
in-house stacks against**, not as "the best driver": both source stacks (the
vintage H5 tree in `retro-3dfx` and the clean-room stack here) are under active
improvement, so any ranking goes stale. Note also that we have **never
benchmarked Amigamerlin as a complete stack** — every Amigamerlin row in the
benchmark DB is a *hybrid* running under our own ICD, so its own OpenGL path is
unmeasured and "Amigamerlin is slower/faster" is not a supported claim.
Comparison write-up: `retro-3dfx/DRIVER-STACK-ASSESSMENT.md` (commit `1768c53`).

## Known Machines

> ### The fleet is powered ON DEMAND — an empty sweep is NOT an outage
>
> The retro machines are **deliberately kept powered off**. They are switched on
> only when we are **making configuration changes to them** or when we are
> **gaming**. Most of the time a discovery sweep of `192.168.1.1-254:9898`
> correctly finds **zero agents**.
>
> - **Never report the fleet as down without first asking whether it is simply
>   off.** On 2026-08-18 eight agents answered; on 2026-08-23 none did, and
>   nothing had broken. (They also drop ICMP — `ping` fails on a box whose agent
>   is answering fine, so probe **TCP 9898**, never ping.)
> - **This makes the chat daemon's "cold-boot crash loop" the steady state, not
>   an edge case.** `retro_chat_daemon.py` returns from `main_async()` when
>   discovery finds no agents, and its unit is `Restart=always` / `RestartSec=3`
>   — so on a host with the fleet off it rescans all 254 IPs every 3 seconds
>   forever. Do **not** enable `retro-chat-daemon` as a boot-time unit on the
>   fleet host; start it alongside the retro machines, or give it a zero-agent
>   backoff first.
> - Judge host health by the **brain's heartbeat** and the daemon's *ability* to
>   claim — never by a live agent count.

Current fleet is on **192.168.1.0/24** (see "Per-box console accounts" above for
the full list). Verified boxes:

| IP | Hostname | OS | Hardware Notes |
|----|----------|----|----|
| 192.168.1.124 | ADMIN | Windows XP SP3 | **NVIDIA GeForce2 GTS** (`10DE:0150`, NV15) on **ForceWare 71.89** — the Voodoo 3 was removed 2026-08-11 and the whole 3dfx stack purged. 383MB RAM, single CPU (440BX/PIII class), dual-boot: **XP on D:**, Win98 on C: (games live on both volumes). |

Legacy rows below are from an older 10.0.0.0/24 network and are **not** current:

| IP | Hostname | OS | Hardware Notes |
|----|----------|----|----|
| 10.0.0.50 | Q0Q1G8 | Win98 4.10 | Voodoo5 5500 AGP, AWE64 PnP |
| 10.0.0.51 | VOIDSSTR-YOR7S5 | Win2K 5.0 | GeForce 2 GTS, SB Live |
| 10.0.0.52 | 2004-XP | Windows XP | 2047MB RAM |

## Windows activation / license status

To read a machine's Windows activation/license status (read-only, like
`slmgr /xpr`), use the agent's `LICSTATUS` command. It only *reports* status; it
does not modify activation state. Restoring activation on a licensed machine is
done through Microsoft's normal path (product-key entry / telephone activation)
by the operator — the offline confirmation-ID helper for that lives in the
private `retro-agent-private` repo, not here.

## SMB File Share

`smb://YOUR-SERVER/files/Utility/Retro Automation/` — distribution point for agent binaries.

Upload: `curl --upload-file file -u YOUR-CREDS "smb://YOUR-SERVER/files/Utility/Retro%20Automation/file"`

## Game/dedicated-server tooling

**The fleet's game servers now run on `whitebeast` (192.168.1.82, Windows 11)**,
which has taken over from the old server box. Configs, the no-blood CS mod and
the host notes live here in [`scripts/game-servers/`](scripts/game-servers/).

Two hard-won rules from standing that up (full detail in
[`scripts/game-servers/README.md`](scripts/game-servers/README.md)):

- **Game servers run natively on Windows, never in WSL.** WSL2 here is in NAT
  mode, so a server bound inside WSL is unreachable from the 192.168.1.0/24
  fleet, and `netsh portproxy` cannot bridge it because portproxy is **TCP-only**
  while GoldSrc is UDP.
- **Never redirect `hlds.exe -console`'s stdout.** It needs a real console;
  `hlds.exe ... > log 2>&1` aborts it into a "Microsoft Visual C++ Runtime
  Library" dialog at `BreakpadMiniDumpSystemInit` and hangs it there — which
  looks exactly like a corrupt install and is a long detour to diagnose. The
  hung process also keeps its UDP port bound after exiting.

The older *Linux* dedicated-server installers and the `cs16-servers` ops skill
remain in the private **retro-agent-private** repo
(`.claude/skills/cs16-servers/`, `docs/game-compat-and-servers.md`).
