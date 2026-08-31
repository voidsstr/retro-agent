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

## CHECK ACTIVATION BEFORE YOU REBOOT A BOX (REQUIRED)

**An unactivated XP box is fine while it is logged in and unreachable the moment
it restarts.** When the activation grace expires, Windows blocks logon entirely
— so the console session never starts, the `HKLM\...\Run\RetroAgent` value
never fires, and the machine comes back with networking up (445/139/135 open)
and **the agent dead** (9898/9899/9897 refused). It looks like a failed boot; it
is a locked activation screen.

This happened to **.171** on 2026-08-29 while finishing a Daemon Tools install:
auto-login was verified correct *before* the reboot, `safe-reboot.py` armed the
PXE hold so the box was not re-imaged, and it still came back unreachable. The
box had been flagged weeks earlier as "not activated, `wpabaln.exe` runs at
logon, not blocking yet" — those two facts were never connected.

**So, before rebooting any box:**
1. **Check activation** — `LICSTATUS` (read-only, like `slmgr /xpr`). If it is
   unactivated or in grace, resolve that FIRST or accept you may need a keyboard.
2. Be especially careful when the reboot is to finish a **driver-class install**
   (a virtual SCSI bus, a display driver) — that is a reboot you chose, and it
   can be deferred.
3. Note that a network logon failure tells you **nothing** about the console
   password: XP Pro in a workgroup defaults to ForceGuest, so
   `NT_STATUS_LOGON_FAILURE` over SMB is expected even when the password is
   right. Do not start guessing credentials.

**Recovering a locked box** (needs someone at the screen — there is no remote
path): the activation lockout itself offers **Yes → "telephone a customer
service representative" → any country**, which displays the 54-digit
Installation ID. Microsoft retired the XP activation servers, internet *and*
phone, so the Confirmation ID is generated offline with the **`xp-activation`
skill** in the private `retro-agent-private` repo
(`scripts/xp-activation/xpcid`). It computes the same value the phone system
used to read back, patches nothing, and only works for IDs the hardware itself
produces. Safe Mode (F8) also allows a login to run `msoobe.exe /a`.

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

### A staged DOS title declares its real-DOS launcher — `DOSGAME.TXT` (2026-08-30)

**DOSBox needs roughly a gigahertz of host CPU to emulate a 486, so on the
fleet's genuine Pentium 1 (`.243`) every DOSBox shortcut is correctly refused —
while the DOS binaries those emulators are running are NATIVE to that machine.**
Five staged titles carry one: `DESCENTR.EXE`, `DESCENT2.EXE`, `QUAKE.EXE`,
`MAINPROG.EXE`, `RR.EXE`.

Two things had to change, and the first is the one worth remembering:

- **A cost a WRAPPER pays must be stated on the shortcut that pays it.** Those
  four titles each declared `min_cpu_mhz` 350–400 at the **title** level, with a
  note reading *"the floor is the emulator's host cost"* — and the title-level
  floor decides whether the tree is **copied at all**, so the whole DOS half of
  the library never reached the one box that runs it natively. The same shape
  bit `requires_capabilities`: Descent 2's title-level `disc_mount` suppressed
  **both** its shortcuts, so on `.123`/`.246` the title had no icon at all.
  Rules 6 and 7 in [`scripts/gamegate/SCHEMA.md`](scripts/gamegate/SCHEMA.md).
- **The DOS menu could not pick the DOS build.** `DOSGAME.EXE` already scans
  `C:\GAMES` — where GAMESYNC deploys — but a staged tree is built for Windows,
  so its 8.3 guess lands on `GLQUAKE.EXE` (a Win32 PE) for Quake and on
  `DESCENT1.BAT` (a cmd.exe batch) for Descent. **Measured in DOSBox**, not
  assumed: `scripts/dosgames/tests/test_pick_outcomes.sh`. The tree now says it
  in `DOSGAME.TXT` (`<8.3 launcher><TAB><title>`), staged by
  `python3 scripts/fleet/stage-dosnative.py`. **The file's own name must be
  8.3** — `dosnative.txt` reaches real DOS as `DOSNAT~1.TXT`.

Carmageddon 1 and Redneck Rampage are deliberately **withheld**: both
`imgmount` a CD image at launch and real DOS here has no image mounter staged.
That is recorded in the stager's `WITHHELD` table, because "we looked and it
cannot work yet" and "we never looked" are different facts.

> **⚠️ Every staged `Play *.bat` is cmd.exe dialect and Win9x is COMMAND.COM.**
> They all open `call "%~dp0FLEETRES.BAT"` / `cd /d "%~dp0"`, and `%~dp0`,
> `cd /d` and `start "<title>"` are NT extensions. That includes
> `Play Quake - Software.bat`, the one Windows shortcut the gate approves for
> `.243`. **Unverified on hardware** — test it with one `EXEC` before believing
> any staged Windows shortcut works on a Win9x box.

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
| `ollama` | system | the 5090 inference engine the capability gate calls |

### After a host reboot, ask ONE question

```bash
python3 scripts/fleet/host-duties.py            # full report
python3 scripts/fleet/host-duties.py --quiet    # only problems
```

It answers the two questions that actually matter, because they have different
answers:

* **Is it running NOW** — and not merely `active`. A unit can be active while
  the thing it supervises is wedged, so where a duty has an observable output
  the check probes *that*: the game servers' own per-engine query replies, the
  freshness of `state.json`, ollama's API, the brain's heartbeat. Verify the
  post-condition, not the return value.
* **Will it come back after the NEXT boot** — which means `enabled`, and for
  `--user` units it also means **linger**. A service started by hand is
  invisible until the reboot that loses it, and that is the one failure this
  cannot be eyeballed.

> **⚠️ LINGER IS THE SINGLE POINT OF FAILURE FOR SEVEN OF THESE DUTIES.**
> Without `loginctl enable-linger voidsstr`, **no `systemctl --user` unit starts
> until somebody logs in** — every unit still reads `enabled`, and the host
> comes up with the fleet bridge, the brain, the favourites agent and all nine
> game servers dead. Verified on 2026-08-30: `Linger=yes`, and all 12 servers
> answered after the reboot.

The check reports **three** states, never two — `absent` (never installed here),
`unknown` (could not ask) and `down` — because only the last is a fault. Its
tests are `tests/python/test_host_duties.py`, and most of them assert the
NEGATIVE path: a checker that can only say OK is the exact failure this project
keeps paying for.

Narrower tools, still useful on their own:

```bash
systemctl --user status retro-gameindex retro-gameservers-watch
python3 scripts/game-servers/healthcheck.py          # per-engine query, 12 servers
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

## Fixing a Staged Game — FIX THE LIBRARY, REDEPLOY TO TEST, THEN PUSH TO THE WHOLE FLEET (REQUIRED)

**User directive, 2026-08-29:** *"when you are fixing issues with staged games
you should make the fix on the staged game and redeploy it to test so we know
the staged game is working correctly for future installs"* and *"always make
sure all retro agents get the fix as well as the staged game"*.

A fix applied to one machine is not a fix. It dies at the next re-image, it
never reaches the other boxes, and the next machine imaged from PXE pulls down
the same broken title. **Every game fix follows this loop, in this order:**

1. **Diagnose on hardware.** Reproduce it on a real box and find the actual
   cause. Do not guess from the tree alone.
2. **Fix it in the STAGED TREE** — `\\192.168.1.122\files\Files\Games-Library\<Title>\`
   — never only on the box. The fix belongs in `launch.txt`, `install.reg`, the
   game's own config, or the `Play <Game>.bat`, so it deploys with the title.
3. **PURGE the title from the test box** (`rd /s /q C:\Games\<Title>` plus its
   desktop `.lnk`, and any per-user state the game keeps — an engine that
   persists an accepted CD key or a resolution into `%USERPROFILE%` will mask a
   broken tree).
4. **Redeploy from the library** via the agent's `GAMESYNC`, and confirm it
   really landed: `state=done` **AND** `failed_files == 0` **AND**
   `titles_done == titles_total`. `state` alone hides partial failures, and
   `gs_write_marker` is skipped when `failed_files != 0`, so `gamesync.done`
   goes stale after a bad run.
5. **Retest and SCREENSHOT it.** A process list is not evidence. The title must
   render, and it must render **fullscreen** (see below).
6. **THEN PUSH IT TO EVERY OTHER CONNECTED BOX.** Purge + `GAMESYNC` the fixed
   title on all of them, so the whole fleet carries the fix — not just the
   machine that happened to test it. This is the step most easily forgotten and
   the user has called it out explicitly.

**Why the purge matters:** a test that passes because you hand-placed a file
earlier proves nothing about the library. This has really happened — a Quake 3
pass was built on a hand-copied `q3key` and had to be redone. If you edited
anything on the box while diagnosing, purge it before you claim a verification.

**All staged games must run FULLSCREEN** (user requirement). Set it in the
staged tree, not in an in-game menu on one box: an id engine rewrites
`config.cfg` on exit, so fullscreen and key binds go in **`autoexec.cfg`**;
Unreal-engine titles use `[WinDrv.WindowsClient] StartupFullscreen=True`; DOSBox
titles need `fullscreen=true` in the title's own `dosbox*.conf`.

**Multiplayer is part of "working"** — see the LAN/IPX rules below.

## Never Put Parentheses in a Generated Filename (REQUIRED)

**A `.bat` whose filename contains `(` or `)` cannot be launched through the
agent.** `EXEC cmd /c start "" /D "<dir>" "Host Redneck Rampage (LAN).bat"`
loses its quoting by the time `cmd` parses it and fails on `'...\Host'`.
Desktop shortcuts are unaffected, so the file looks perfectly good to a person
double-clicking it — **it only bites automation**, which is exactly why it
survives review.

**This is the SECOND time this character has cost us time.** The generated
`onboard.cmd` (since removed with ONBOARD in v1.71.0) had the same problem with
game NAMEs containing parentheses — `(BC Romania)`, `(fleet build)` — where the
`)` in an expanded variable closed a `( ... )` block early and cmd aborted with
`- was unexpected at this time.`, leaving onboarding silently unfinished. The
file is gone; the rule is not.

So the rule is now general, not per-script:

- **Any filename this project GENERATES — a launcher `.bat`, a staged shortcut
  target, a `launch.txt` entry — must avoid `(` and `)` entirely.** Use a dash:
  `Host Redneck Rampage - LAN.bat`, not `Host Redneck Rampage (LAN).bat`.
- **Display names in `launch.txt` may still contain parentheses** — that column
  is a label, not a path. It is the *filename* that must stay clean.
- Where a name is not ours to choose (a vendor's own exe, an existing game
  directory), quote defensively and **test the launch through the agent**, not
  just from a shortcut.

The same applies to `taskkill`: `taskkill /f /im "Descent 3.exe"` needs the
quotes, and **without them it silently kills nothing** — after which the
previous game is still on screen and the next screenshot is attributed to the
wrong title.

## Search Windows Trees CASE-INSENSITIVELY (REQUIRED)

**We are a Linux host reasoning about Windows filesystems, where filenames are
case-insensitive. A case-sensitive `grep`/`find` will tell you a file is absent
when it is sitting right there**, and a confident "0 hits" is far more damaging
than no answer, because it redirects everyone to hunt for something that was
never missing.

This has cost real time more than once:
- **Shogo** appeared to be a half-applied patch — the engine wanted `cshell.dll`
  version 3 and resolved version 1. A case-sensitive grep for `cshell` returned
  **zero hits in every patch archive**, which read as "the patch did not ship
  it". LithTech names the file **`CShell.dll`**, and it was staged all along in
  `SHOGOP3.REZ`. The real fault was simply that the command line never named
  that archive.
- The PXE **nicdb** lookups failed three separate times because its keys are
  uppercase hex, and each time the conclusion was "we have no driver for this
  NIC" — for drivers we already shipped.

**So:**
- Use `grep -i`, `find -iname`, and `_stricmp`-equivalent comparisons whenever
  the subject is a Windows path, filename, registry key or hardware ID.
- **A negative result about a Windows file is not reportable until it has been
  re-run case-insensitively.** Say "not found (case-insensitive)" so the reader
  knows which was done.
- Prefer asking the binary what it wants over guessing: `Shogo.exe`'s own string
  table contains the archive chain it builds (`ShogoL, ShogoT, ShogoP9 … P2, P`
  — highest patch first, first-match-wins), which settles load order without
  experiment.

## Make Failure VISIBLE — every serious defect here reported success (REQUIRED)

Across a full day of fleet work, **every serious defect had the same shape: the
tool reported success and the operator believed it.**

| the tool said | what was actually true |
|---|---|
| `GAMESYNC` → `state=done, failed_files: 0` | it had **skipped every same-size file**, half-applying a patch — Deus Ex took the new `Core.u` and kept the retail `Core.dll` |
| `rd /s /q` → returns, no error surfaced | it hit **per-file access-denied on a running game and carried on**, leaving a partial tree that then synced into a mix |
| a mount launcher → printed reassurance and started the game | `batchmnt64.exe` had **exited 216 on 32-bit**, and it launched against a **completely different game's disc** |
| `state=done` after an abort | the aborted run recorded its in-flight file as `failed_files: 1` — a real failure and an abort look identical |

**In none of these cases was the remedy better tooling. It was making the
failure visible.** Quote `failed_files` rather than `state`. Verify the
directory is actually gone rather than trusting `rd`. Check the exit code rather
than assuming a mounter ran. Print a banner instead of a soothing sentence.

So, when you build or use anything in this repo:

- **A tolerated failure must SAY it is a failure.** The mount launchers now
  print a boxed `MOUNT FAILED` banner, state that the disc in the drive may be
  another game's entirely, and write a `mount-error.txt` — while still
  proceeding, because these checks often do accept any disc and a launcher that
  refuses where the game would have run is its own bug. Tolerating a failure is
  fine; **concealing it is not.**
- **Check the exit code of the thing that actually did the work**, not of the
  wrapper that started it. `start ""` discards it entirely; a launcher stub
  (`SUN.EXE`, `Ra2.exe`) always returns 0 regardless of the game.
- **Verify the post-condition, not the return value.** Did the directory
  disappear? Did the drive letter appear? Did the file's size change on the box?
- **A negative result needs the same rigour as a positive one.** "Not found" is
  reportable only once it has been re-run case-insensitively; "no such file" is
  reportable only once you have listed the parent.

The corollary for reporting: **a phantom bug report is worse than no report**,
because it sends everyone chasing something that was never there. Before
escalating "this affects the whole fleet", check whether your *measurement* is
the broken thing — especially when the claim rests on timing rather than on a
screenshot or a log line.

## GAMESYNC Never Deletes — and what changed in v1.62.0 (REQUIRED)

`gs_copy_file()` decides what to copy cheaply, because hashing 30 GB over SMB1
on a Pentium III costs more than it saves. That leaves **one** live blind spot,
and one that has been **fixed** — the distinction matters, because the old
workaround is now obsolete advice.

### STILL TRUE: it never DELETES
A file the library **stops shipping stays on every box forever**. A stale
`System\Running.ini` survived a full redeploy and went on raising UT's "Recovery
Mode" dialog; `SiNGold` currently carries ~963 stale files on `.143`. Removal is
not something GAMESYNC can express, so **put the deletion in the title's
`Play <Game>.bat`**, or clear it on each box by hand.

### FIXED in v1.62.0: it no longer skips a same-size file
Until then the skip test was **size only**, so a staged file edited to the same
byte count never reached a box that already had it — silently, with
`state=done, failed_files: 0`. That half-applied every patch we shipped: the
Deus Ex 1.112fm payload changed 38 files and **seventeen kept their exact byte
size**, including `Core.dll` (790,528) and `DeusEx.exe` (253,952), so boxes took
the new `Core.u` and kept the retail `Core.dll`.

**v1.62.0 requires size AND last-write time**, and stamps the destination with
the source's mtime so resume still costs nothing. Verified: the one-off repair
re-copy took 1,446 s, and the **next incremental sync took 182 s**.

> **⚠️ OBSOLETE ADVICE — do not follow it.** An earlier version of this section
> told agents to *"deliberately change a staged file's size when you edit it"*
> and to pad configs with a trailing comment. That was the correct workaround
> **before v1.62.0 and is now cargo-cult** — it clutters staged configs to
> defeat a check that no longer exists. Edit staged files normally.

### What has NOT changed: verify the post-condition
A sync reporting success is still not proof a specific fix arrived. **Check the
file on the box** — its size, or `type` it — after any small staged change, and
quote `titles_skipped` **and** `failed_files`, not `state`. Note an **aborted**
run records its in-flight file as `failed_files: 1`, so an abort and a real
failure look identical from the status alone.

Two ways to manufacture the same mixed install by hand, both seen here:
- **`rd /s /q` hits per-file access-denied on a running game and carries on**,
  leaving a partial tree. Kill the game first (quoting spaced image names),
  `rd`, then **verify the directory is gone** before syncing.
- **GAMESYNC cannot overwrite a running `.exe`** and will skip the marker — one
  box hit `failed_files: 5` on `SiNGold\sin.exe` because a test had it running.

## LAN / TCP Multiplayer Is Part of a Staged Game (REQUIRED)

**User directive, 2026-08-29:** games that only offer IPX out of the box must be
set up so LAN/TCP play works, **and it must be proven** — *"ra 2 might need you
to test out creating a game on one computer and joining from another"*. The CS
1.6 server must also be **visible in the client's LAN browser** on every box.

**The proof standard: two machines, screenshots of BOTH.** Host on box A, join
from box B, and capture the host hosting, the joiner seeing A's game in its
browser, and both players in the game together. A Network menu is not proof; a
`netstat` line is not proof.

- **IPX-only Win32 titles → IPXWrapper** (tunnels IPX over UDP; drop-in DLLs
  beside the exe, no driver, no protocol install). `Games-Library/Carmageddon2/`
  already carries a working one — **copy that pattern, do not invent a second**.
  Beware: `RedAlert2/wsock32.dll` is a *different* file and is NOT IPXWrapper.
- **DOSBox titles: `ipx=true` ALONE DOES NOTHING.** It only enables the emulated
  adapter; a tunnel is still required — `ipxnet startserver` on one machine and
  `ipxnet connect <ip>` on the rest. Pick ONE hosting pattern and use it for
  every DOSBox title.
- **Check whether a TCP/IP-native engine already exists** before wrapping IPX —
  e.g. D2X-Rebirth speaks UDP/IP natively, which removes the problem entirely.
- The fleet is one flat subnet (192.168.1.0/24) and the **firewall is off
  fleet-wide and in the image**, so neither is your obstacle.

Fleet servers live on **192.168.1.132** — CS 1.6 `:27015` (no-blood `:27016`),
Specialists `:27017`, Quake III `:27961`, OpenArena `:27960`, Quake 2 `:27910`,
QuakeWorld `:27502`, UT99 `:7797`, UT2004 `:7777`. If a client's LAN tab is
empty, try `connect 192.168.1.132:27015` from the console — that distinguishes
"server unreachable" from "broadcast discovery failing", which are different
faults with different fixes.

## Adding a NEW Staged Title — the checklist (REQUIRED)

**User directive: every deployed game must have a real icon, neatly placed. That
is part of staging a title, not a follow-up pass.** A title is not staged until
it satisfies all of this:

1. **The installed tree**, not the installer — the state *after* setup ran.
2. **`launch.txt`**, one line per shortcut:
   `<target><TAB><display name><TAB><icon path>`
   - The **display name becomes the `.lnk` filename**, so it must be a legal
     Windows filename: **no** `\ / : * ? " < > |`. Redneck Rampage shipped
     `"... Setup / Network Config"` and that shortcut **never existed on any
     box** — the agent logs a failure and carries on.
   - **No parentheses in a target filename** — unlaunchable through the agent,
     yet fine from a desktop double-click, so it survives review.
   - **Keep every data line inside the agent's 1023-byte read** — put data
     lines above any comments.
3. **THE ICON, and ship it INSIDE THE TREE.** Auto-resolution is a fallback, not
   a plan: it cannot separate a title's several launchers (Red Alert 2 from
   Yuri's Revenge; Half-Life's five mods all reaching `hl.exe`), and it picks
   wrongly on its own — Counter-Strike resolved to `hl.exe` and wore the
   Half-Life lambda; System Shock 2 resolved to `clokspl.exe`, a CD-Cops loader.
   **Give the third field explicitly for every shortcut**, pointing at an `.ico`
   or an exe **inside the staged tree**, so the artwork deploys with the game
   and a fresh box gets it with no extra step. If a title genuinely ships no
   artwork (Carmageddon 1 has none — no `.ico`, and a DOS4GW binary carries no
   PE resources), **say so in the tree's notes**; a dull icon beats a wrong one.
4. **`install.reg`** for every registry key the game needs — CD-check
   satisfaction, install paths, video config. **`REGEDIT4`** merges everywhere;
   `Windows Registry Editor Version 5.00` is XP+ only and does nothing at all,
   silently, on Win9x.
   **A per-INSTALLATION value cannot live here** — install.reg is copied
   byte-identically to every box, so a network serial or machine GUID must be
   generated on the box by the launcher (see Red Alert 2).
5. **Fullscreen**, set in the tree — and a resolution the box's monitor
   actually supports.
6. **Relocatable** — no absolute paths assuming the machine it was built on.
7. **Multiplayer patched to the version our servers run**, and LAN proven on two
   machines if the title has it.
8. **Check the PE subsystem of every binary**: `SubsystemVersion >= 6.0` is
   Vista-only and **XP's loader refuses it before a single instruction runs**.
   GOG and re-release repacks are the usual offenders — SiN Gold shipped one and
   was unloadable on every XP box. `pescan.py` in the ops scratchpad sweeps a
   whole tree.
9. **Run the validator**: `python3 scripts/validate-staged-library.py` must
   report 0 failures.
10. **Deploy it and look at the desktop.** The icon must be real and the icons
    must sit tidily in the wallpaper's bay. A staged title whose shortcut is a
    generic white page is not finished.

**Then push it to every connected box** — a title staged but not deployed is
half done.

## Prove the Library Is Deployable — run the validator (REQUIRED)

**A staged library is only worth the promise it keeps: that the agent can move a
title onto a brand-new machine and it simply works.** Every way we have broken
that promise was **silent** — the defect appears on a box, hours later, with
nothing pointing back at the library.

```bash
python3 scripts/validate-staged-library.py          # full report
python3 scripts/validate-staged-library.py --quiet  # only problems
python3 scripts/validate-staged-library.py --json   # for tooling
```

Exit 0 means every title satisfies the contract. **Run it before any imaging
run, after any change to `Games-Library/`, and as the last step of any staging
work.** `tests/test_staged_library.py` runs it in the suite (suite [6]) and
SKIPS loudly when the share is not mounted, because a silent skip would let the
library rot unnoticed.

What it catches, each of which really reached a box:

| check | the silent failure it prevents |
|---|---|
| `launch.txt` names a file in the tree | no shortcut is made and nothing says why |
| every data line inside the **1023-byte** read | comments above the data push a line past the agent's read and that game silently loses its shortcut |
| no `(` or `)` in a launcher filename | unlaunchable through the agent, fine from a desktop double-click — so it survives human review |
| an explicit icon path resolves | a typo degrades quietly to the auto-resolved icon, i.e. exactly the wrong artwork the field exists to prevent |
| `install.reg` dialect | `REGEDIT4` merges everywhere; `Windows Registry Editor Version 5.00` is XP+ only and does nothing at all on Win9x |
| DOSBox conf sets `fullscreen=true` | all staged games must run fullscreen |

**Deploy-blocking problems are `FAIL`; everything else is `warn`.** That line is
deliberate — a validator that cries wolf trains people to ignore it, and the
REGEDIT5 check was demoted to a warning for exactly that reason once it was
confirmed valid on the whole current fleet.

## "Staged Games" — the term, and what it guarantees (REQUIRED)

**A game is STAGED when it can be moved onto a retro PC by the agent and simply
work — no installer, no wizard, no operator at the keyboard.** When the user says
"staged games" they mean exactly this, so do not read it as "copied" or
"available on the share".

Staging lives in `\\192.168.1.122\files\Files\Games-Library\<Title>\`. A title is
staged only when ALL of the following are true:

1. **The whole tree is there**, already installed — the state the game is in
   *after* its installer has run, not the installer itself.
2. **`launch.txt`** names what to run: `<relative exe or .bat><TAB><display name>`.
   ONE SHORTCUT PER LINE — Red Alert 2 lists both the game and Yuri's Revenge;
   `#` comments and blank lines are ignored. The agent makes a desktop shortcut
   from each line, so a title whose second entry is missing loses half itself
   silently.
3. **`install.reg`** carries every registry key the game needs (install paths,
   CD-check satisfaction, video config). The agent merges it after copying.
   A game that only works because a registry key happens to exist on the box
   that built it is NOT staged.
4. **It runs from any path** — no absolute paths baked into config that assume
   the machine it was installed on.
5. **DOS titles carry their own DOSBox** and a `Play <Game>.bat` that `cd`s into
   `DOSBOX\` first, so the conf's relative `mount C ".."` resolves wherever the
   tree lands. Carmageddon, System Shock, Descent and Redneck Rampage all follow
   this one pattern - do not invent a second.
6. **Multiplayer titles are patched to the version our servers run.** UT99 must
   be OldUnreal 469e because `ut99-server` is 469e and a 436 client cannot join
   at all. See `Games-Library/_patches/README.txt` for what is applied and what
   still needs a Windows box.

**Support directories in the library root start with `_`** and are NOT games:
`_desktop/` (fleet wallpapers), `_patches/` (the patch record), `_priority.txt`
(copy order). The agent skips `_`-prefixed directories — before it did, every
machine copied 26 MB of wallpaper as if it were a title.

**The goal is to keep growing this set.** Every game added to the library should
be staged to this standard, because the whole point is that a freshly imaged
machine gets its games with nobody touching it. A title that needs a manual step
is not finished — record what it needs in `_patches/README.txt` rather than
leaving it looking done. The two that currently need their disc mounted (System
Shock 2, Diablo II) say so in their trees.

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

#### ⚠️ WRITE THE `.ver` SIDECAR **LAST** (ordering invariant, not style)

Publish in this order, whichever route you use: **versioned archive → latest
pointer → `.ver` sidecar.** Auto-update compares the agent's compiled version
against the sidecar and pulls on **inequality**, so while `.ver` still names the
OLD version every box compares equal and correctly declines to pull. There is
then no instant at which a box can fetch a binary whose version disagrees with
the sidecar. Writing `.ver` first opens exactly that window.

#### ⚠️ THE DEV HOST HAS **TWO** MOUNTS OF THE SAME SHARE — one is read-only

"the share is read-only from the dev host" is half true, and the half that is
false costs an hour. Both of these are the same SMB share:

| path | how | access |
|---|---|---|
| `/mnt/retro-share` | CIFS from `/etc/fstab`, with an explicit **`ro`** flag | **read-only** — a `cp` here fails with *Read-only file system* |
| `/run/user/1000/gvfs/smb-share:server=192.168.1.122,share=files,user=voidsstr` | gvfs (the GNOME "mapped network drive") | **read-write** (verified by write → read back → delete) |

So a host-side `cp` **does** work — through the gvfs path, not `/mnt`. Read
through `/mnt/retro-share` (it is fine for that and always present); write
through gvfs. **The gvfs mount is per-login-session**: it exists because the
user is logged into a desktop, and it is absent in a headless, `systemd`-run or
freshly-rebooted context. Do not build anything that depends on it silently.

`/etc/cifs-retro-share.creds` is root-only, and sudo needs an interactive
password on this host, so `smbclient -A` is **not** an available route.

Three ways to publish, in order of preference:
1. **Host-side `cp` via the gvfs mount** (above) when that mount is present —
   simplest, and lets you `md5sum` the published file directly afterwards.
2. **`make release`** (in `agent/tools/` for the chat client, `agent/` for the
   agent) — bumps the version tag, builds, and uploads both. Needs `SMB_CREDS`
   set in the Makefile.
3. **Via an online fleet agent** — the fallback that always works, and the one
   to use when gvfs is absent: build, then `UPLOAD` the binary to a machine
   (e.g. `C:\RETRO_AGENT\retro_chat.exe`) and `EXEC cmd /c copy /Y` it to the
   share's latest pointer **and** the versioned path.
   **`NETMAP` first — do not assume the box has the share mapped.** On
   2026-08-30 `.171` answered `net use` with "There are no entries in the list"
   and `Z:\Utility\Retro Automation` did not exist; one
   `NETMAP \\192.168.1.122\files Z:` fixed it. The `Z:` drive is a
   convention, not a guarantee.

**Verify the post-condition from the host**, never from "1 file(s) copied":
`md5sum` the local build against BOTH published copies, `cat` the `.ver`, and
`strings <published exe> | grep -x <version>` so the binary itself confirms the
version you think you shipped.

**Publish through gvfs, then verify through `/mnt`.** That is not just faster
than the fleet-box route — it is a *better* check, because you read the bytes
that actually landed back over a **different mount than you wrote them through**.
The fleet-box route can only tell you a copy command returned.

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

## Hardware Capability Gate — only deploy a game a machine can RUN (agent v1.71.0+)

**GAMESYNC now decides, per title, whether this machine can actually run it, and
skips the ones it cannot.** The fleet spans a 1999 Pentium III with a Voodoo and
a 2011 Sandy Bridge quad; copying the whole library onto every box spent an hour
of SMB1 bandwidth on titles that then launched into a black screen.

Full docs: [`scripts/gamegate/README.md`](scripts/gamegate/README.md).
Authoring a title's requirements: [`scripts/gamegate/SCHEMA.md`](scripts/gamegate/SCHEMA.md).

### The split — by where the work can physically run

A Pentium III cannot call a language model; the dev host's RTX 5090 cannot see
inside a fleet box.

- **Agent (C):** `HWPROFILE` collects the machine (below), and
  `agent/shared/gamegate.h` carries the **deterministic rules**, so a freshly
  PXE-imaged box gates its own GAMESYNC before any host tool has seen it.
- **Host (Python, `scripts/gamegate/`):** plans the whole fleet, escalates
  **only** the borderline band to ollama, caches every verdict in
  `~/.retro-fleet/gamegate.db`, and publishes
  `<library>\_gamegate\<profile_hash>.txt` which the agent prefers when present.
- The two rule copies are pinned together by `tests/python/test_gamegate_mirror.py`,
  which **compiles the C header** and compares every answer against the Python one.

### Rules decide alone wherever they can; only MARGINAL reaches the LLM

| | |
|---|---|
| **hard NO** | OS floor unmet, a required CPU instruction absent (a missing SSE2 is `#UD`, not "slow"), a GPU **two** whole feature levels short |
| **MARGINAL** | within **25%** of a published minimum, or a GPU **exactly one** level short — the only band an LLM ever sees |
| **RUN** | everything met |

A gate that phones an LLM to conclude "a Pentium III cannot run Doom 3" is a bad
gate. `tests/native/test_gamegate.c` asserts the obvious cases stay obvious.

**Model: `qwen3:14b`** — 5/5 strict JSON, 4/5 agreement, 0.4 s median on the
5090. `qwen3.6:27b` returns an **empty response** under schema `format` (0/5) —
do not re-try it. `gemma4:26b` matches accuracy at 45 s and emits nonsense
confidences. `qwen2.5-coder:7b` is fast and confidently wrong. A malformed reply
is retried and then **the deterministic verdict stands** — it is never allowed to
become "run", which would make a broken model the most permissive gate on the
fleet, invisibly.

### FAIL-OPEN, by explicit decision

**Absent data never blocks a title** — no `requires.json`, an unparsable one, a
field omitted, an unclassifiable GPU, an unmeasurable clock: all deploy. The gate
blocks only on **positive evidence**. Fail-closed would produce a box that
silently receives no games and says nothing about why. Every skip is logged with
its limiting factor and both numbers, and `GAMESYNC` status gained
**`titles_gated`**, counted separately from `titles_skipped` (which means "did
not fit on the disk" — different fact, different follow-up).

**Kill switch:** `HKLM\Software\RetroAgent\GameGate` = `0` restores
copy-everything.

### HWPROFILE — because SYSINFO cannot answer the question

`SYSINFO` reports no clock, no CPU vendor, no instruction set and no GPU at all,
and its RAM saturates at 2047 MB. `HWPROFILE` (`agent/src/hwprofile.c`) reports
CPUID vendor/family/model/stepping, the real clock (`~MHz`, or a timed TSC loop
on 9x), `GlobalMemoryStatusEx` RAM, the instruction-set bits, the **active**
display adapter's PCI ids + video RAM + driver version, OS level, DirectX, free
disk, a `disc_mount` capability, and a **`profile_hash`** that is stable across
reboots (hardware fields only, clock bucketed to 25 MHz and RAM to 16 MB) — that
hash is the cache key and the published verdict file's name.

> **⚠️ `VIDEODIAG.adapters[0]` IS NOT THE LIVE ADAPTER.** It enumerates every
> `Class\{4D36E968-...}\NNNN` subkey, so on any box that has ever had a card
> swapped its first entry can be a **stale registry key for hardware that is no
> longer fitted** — it has already caused one wrong report that a box had no
> video driver. `HWPROFILE` asks `EnumDisplayDevices` for the adapter
> `ATTACHED_TO_DESKTOP` and follows **its own `DeviceKey`**. Use `HWPROFILE` when
> you want to know what card is really driving the screen.

**Hardware T&L is the axis that separates this fleet, not VRAM megabytes.**
`.171`'s Intel 865G has 3D and no hardware T&L at all (`fixed`), `.124`'s
GeForce2 GTS has T&L and no shaders (`tnl`), and GeForce4 **MX** is `tnl` while
GeForce4 **Ti** is `sm1.x` — a DX7 part wearing a DX8 part's name.

### Capabilities are reported, never folded into the verdict

A GeForce2 will never grow a pixel shader; a box with no virtual disc mounter is
one installer away. Calling both "cannot run" tells the operator to give up
rather than to fix the box. So a missing **capability** (`disc_mount` today)
leaves the verdict alone: **the title still deploys**, only the shortcut that
needs it is suppressed, and the log names the remedy. GAMESYNC re-runs every
boot, so it returns by itself once the box is fixed. This is live right now —
seven staged titles mount a disc image at launch and `.123`/`.246` have no
mounter, so on those boxes they have never worked and nothing said so.

**Requirements can be per-shortcut**, keyed on the `launch.txt` first column,
because a title's halves need different machines: BF1942's single player wants a
mounted disc while its LAN launchers check neither disc nor CD key.

### ONBOARD was REMOVED in v1.71.0

`ONBOARD`, `agent/src/onboard.c`, `provisioning/onboard.json` /
`gen_onboard.py` / `onboard.cmd` / `onboard_9x.bat` / `push_onboard.py` and the
`onboard-machine` skill are all gone. **GAMESYNC does the same work from the
staged library, which is the source of truth for what a game is** — the
onboarding list was a second hand-maintained inventory, so a properly staged
title still did not reach a box until someone remembered to add it, zip it and
push control files.

The hardware gating that made onboarding worth having did not go away, it got
much better: `set_capability_env()` exported four coarse booleans (`ONB_GPU3D`,
`ONB_CPUFAST`, `ONB_RAM64`, `ONB_RAM128`) from `wProcessorLevel >= 6` and a
substring search of the adapter name — it could not tell an 845 MHz Pentium III
from a 3.1 GHz Core i5, could not see a clock or video RAM at all, and treated a
Voodoo 2 and a GeForce 8400 GS as the same "has 3D" fact. That role is now
`HWPROFILE` + `requires.json`.

**`provisioning/retro_unzip.js` STAYS** — `provisioning/ddk/*.py` and the
`game-install` skill both stage and drive it. The `Onboarded` registry value is
simply ignored now; leaving it set does nothing.

### Desktop theme + icons are (re)applied on EVERY startup

The dark "hacker" system-color theme, the dossier wallpaper, and the parked-icon
layout are applied by the agent's **`retrowall` thread on every startup** (v1.8.0+,
`agent/src/retrowall.c`) — so a box keeps the
fleet look across reboots. It applies whatever the **retro-wallpaper skill** has
staged into `C:\retro-wall\`:
- `wall00..NN.bmp` + `rotate_wall.exe` → wallpaper rotation
- `retro_theme.reg` + `setsyscolors.exe` → dark green-on-black system colors
  (regedit writes `HKCU\Control Panel\Colors`, then `setsyscolors.exe` pushes
  them live via `SetSysColors` so it takes effect without a re-logon)
- ~~`arrange_icons.exe`~~ → **superseded, and must NOT be run** (see below)

Each step is a **no-op if its asset isn't staged** — but the icon layout and the
theme are applied **regardless**, because neither needs a staged asset. Note the
apply call has to sit **above** `retrowall_apply_startup()`'s early returns:
both of those returns are the NORMAL path on a fleet box, so anything placed
after them runs on almost no machine while looking installed. The theme and the
screensaver were already caught by exactly that once.

### Desktop icons: AUTO ARRANGE is the fleet default (agent v1.73.0+)

**User requirement: "the icons are always auto arranged".** Only Windows' own
Auto Arrange delivers that, because the shell re-packs the desktop **itself**
on every event that scatters icons — a resolution change, a fullscreen game
exiting, a new shortcut, an Explorer restart. No agent pass, however frequent,
can win that race; it can only tidy up afterwards, which *is* the "the icons
keep moving" complaint. `gs_desktop_icons_apply()` (`agent/src/gamesync.c`)
sets it on every agent startup, after a `GAMESYNC`, and on demand.

**This SUPERSEDES the icon bay.** With Auto Arrange on, the shell packs icons
into its own grid from the top-left and **ignores `LVM_SETITEMPOSITION`
outright**, so the wallpaper's drawn bay cells are no longer used. The two are
mutually exclusive; exactly one runs, chosen explicitly:

| `HKLM\Software\RetroAgent\IconAutoArrange` | layout |
|---|---|
| **absent, or 1** | **Auto Arrange — the shell owns the layout (DEFAULT)** |
| `0` | legacy icon bay — the agent places each icon in a drawn cell |

**It is a SET, never a TOGGLE.** `FCIDM_SHVIEW_AUTOARRANGE` is a WM_COMMAND
**toggle**. Fired blindly it turns Auto Arrange *off* on a box that already had
it on — the exact inverse of the bug that once left icons in rows across the
top of the screen. So the toggle is posted **only when `LVS_AUTOARRANGE` is
clear**, the bit is read back afterwards, and `SetWindowLongA` is the fallback.

> **The fallback is load-bearing, not belt-and-braces.** Measured 2026-08-30:
> the shell toggle **silently failed on both `.171` and `.143`**. A
> WM_COMMAND-only implementation would have logged "auto-arrange turned on" and
> changed nothing on a quarter of the fleet — this project's recurring
> "reported success and was believed" shape. Always read the bit back.

**Persistence** is `HKCU\Software\Microsoft\Windows\Shell\Bags\1\Desktop`
→ `FFlags`, a FOLDERFLAGS word: **bit 0 = `FWF_AUTOARRANGE`**, bit 2 =
`FWF_SNAPTOGRID` ("align to grid"). It must be **read-modify-written**, never
stamped: the fleet is not uniform — `.143` read `0x220` and `.171` read `0x224`
— so a constant would silently change align-to-grid on some boxes and not
others.

> **THE REGISTRY ALONE IS NOT ENOUGH — the every-startup re-apply is what
> actually delivers the guarantee.** Measured on `.133` across a real power
> cycle: `FFlags` came back **`0x221`, bit 0 intact**, and the live listview
> style was nonetheless **OFF** — XP's shell did not honour the persisted bag —
> so the agent had to set it again at startup (`shell toggle did not take - set
> LVS_AUTOARRANGE directly`). Treat the bag write as a **backstop**, not the
> mechanism. It does survive an Explorer restart (measured separately on
> `.171`: `FFlags` stayed `0x225` and the live bit stayed on), which is the case
> it genuinely covers.

**Align-to-grid is left alone** in auto mode. The agent used to clear
`LVS_EX_SNAPTOGRID` only because its 103px row pitch walked icons out of the
bay's 80px cells; with the shell doing the packing that no longer applies.

**`scripts/retro-wallpaper/arrange_icons.exe` must never be staged or run.** It
parks icons bottom-right **and explicitly clears `LVS_AUTOARRANGE`**, so one
run turns the fleet-wide setting back off. `deploy_rotation.py` renames any
stale copy aside; the source carries a `SUPERSEDED` banner.

On demand: **`ICONARRANGE [auto|bay]`** — applies the layout now and returns the
**post-condition** (live style bit, persisted `FFlags`, icon count, screen mode)
rather than `OK`, because a log line saying we set auto-arrange is not evidence
that auto-arrange is set.

#### The icon layout is rebuilt ONLY when the desktop changed (v1.73.0)

`gs_run()` used to end with an **unconditional** arrange. GAMESYNC runs at
startup and the usual case is a fully provisioned box — every title skipped,
nothing copied, no shortcut created — so **every boot of every machine rebuilt
the layout for nothing.** That is what "the retro agent is rebuilding icons all
the time" was.

| what happened | rebuild? |
|---|---|
| a file was really **written** (not skipped by the size+mtime resume test) | **yes** |
| a `.lnk` **appeared** that was not there before, or was **swept away** | **yes** |
| explicit **`ICONARRANGE`** | **yes, always** — a manual request is deliberate |
| a sync that copied nothing and created no shortcut (the every-boot case) | **no** — logs `nothing changed - icons left alone` |

**Do not over-correct this.** A title genuinely redeployed *must* still arrange
— that is the staged-game fix loop, and a freshly deployed game whose icon never
gets placed is a worse bug than the churn.

Two counters that look obvious and are **wrong**, both guarded by tests:
- counting "`gs_copy_file` succeeded" — it returns success for a **skipped**
  file, so it is true on every run and measures nothing. The counter sits past
  the resume early-out.
- counting **shortcut writes** — `gs_make_game_shortcut()` rewrites a title's
  `.lnk` every pass. Only a link that was **not there before** changes the set
  of icons.

The second churn source was in the auto-arrange code itself: `LVM_ARRANGE` was
sent on every startup even when auto-arrange was **already on**. With the shell
maintaining the layout that achieves nothing and is visible churn, so it is now
sent only when the setting was just changed, or when forced.

**The gate REPORTS what it decided on (v1.75.0)** — a silent gate is an
untestable gate. Both the `done:` log line and `GAMESYNC STATUS` now carry
`files_written` and `shortcuts_changed`:

```
done: 35/37 title(s) copied, 0 skipped, 2 gated, 0 file error(s),
      0 file(s) written, 0 new/removed shortcut(s)
```

> **This instrumentation immediately found that the gate was defeated.**
> `gs_run()` begins with `gs_sweep_desktop()`, which moves **every** `.lnk` off
> the desktop — so by the time each title's shortcut is written nothing is ever
> "already there", every shortcut counted as new, and the gate was true on every
> box on every sync while reporting itself as working. Fixed in **v1.76.0**: the
> icon **set** is sampled before the sweep (`gs_desk_snapshot()`) and the net
> difference resolved at the end (`gs_desk_settle_lnks()`), so sweeping 81
> shortcuts and rewriting the same 81 is correctly *no change*.

**VERIFIED ON HARDWARE (.171, agent 1.77.0, quiet library):**

```
nothing changed - icons left alone (0 file(s) written, 0 new/removed shortcut(s))
done: 37/38 title(s) copied, ... 0 file(s) written, 0 new/removed shortcut(s)
```

> **To see the gate suppress, the library must be QUIET.** It could not be
> observed doing so for a whole afternoon — every sync read `files_written=1`,
> which looks exactly like the defect below. It was not: four agents were
> editing `Games-Library` throughout, so a *different* file legitimately changed
> each pass. **You cannot measure "did anything change?" while something is
> changing.** Establish that a no-op was actually available before concluding
> the no-op path is broken.

**A steady-state box must report `0 file(s) written`.** A box reporting the
*same small non-zero count on consecutive no-change syncs* is announcing the one
realistic way this gate fails: a file whose mtime never stamps (SetFileTime
failing on an odd-attributed file, or coarser destination time granularity)
fails the size+mtime resume test **forever**, so every sync writes it, the gate
is always true, and the every-boot rebuild returns with nothing saying so. That
is the project's signature failure mode, and this line is what makes it
visible. Stage/refresh all of them
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
- **⚠️ NEVER issue a bare `REBOOT` — use `scripts/fleet/safe-reboot.py <ip>`.** The
  fleet boxes **PXE boot FIRST**: that is how they get imaged. A plain reboot can
  be handed a fresh install offer and the machine **repartitions itself**, wiping
  the disk. `safe-reboot.py` arms the PXE boot hold first and refuses to reboot if
  it cannot. (`--reinstall` inverts it, to reimage deliberately.) This has really
  happened twice: a Gateway 550 lost an hour of provisioning on 2026-08-28, and a
  parallel session caused six reinstalls in an hour on 2026-08-29. The agent's
  `REBOOT` cannot know any of this — it just reboots.
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
- **HWPROFILE** — the machine's stable hardware fingerprint as JSON: CPUID
  vendor/family/model/stepping, real clock, real RAM, instruction-set bits, the
  **ACTIVE** display adapter (PCI ids, video RAM, driver version — not
  `VIDEODIAG`'s possibly-stale `adapters[0]`), OS level, DirectX, free disk, a
  `disc_mount` capability, and a reboot-stable `profile_hash`. This is what the
  capability gate runs on; see the Hardware Capability Gate section.

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
- **UIKEY keyname** — send keystroke (uses MapVirtualKey scan codes).
  Named keys include the function keys, the navigation cluster, `TILDE`
  (the game console key) and `PRINTSCREEN` — the last is how you get a frame
  out of a game whose fullscreen surface `SCREENSHOT` cannot capture.
  Modifier combos work: `UIKEY CTRL+SHIFT+A`.
- **UIKEY TEXT:&lt;string&gt;** — **type a whole string**, character by character,
  via `VkKeyScanA` (so it handles shifted characters). This mode is easy to
  miss — it was in `input.c` for a long time before anyone found it, and was
  rediscovered only by reading the source while fighting a CD-key dialog.
  Use it for text fields instead of a chain of single `UIKEY` calls.

  > **⚠️ SYNTHETIC KEYBOARD INPUT DOES NOT REACH AN id TECH 3 MENU IN
  > EXCLUSIVE FULLSCREEN.** Measured on SoF2: at fullscreen 640x480 both
  > `UIKEY` per-character and `UIKEY TEXT:` landed *nothing*; relaunched with
  > `+set r_fullscreen 0`, the identical `UIKEY TEXT:` filled the field
  > immediately. **If a game ignores your keystrokes, run it windowed to do
  > the typing, then restore fullscreen** — do not conclude the key is wrong.
  >
  > Two further limits in that same menu, so you know when to stop trying:
  > a field may cap its length and not auto-advance; `TAB` can move a visible
  > highlight without moving text-entry focus; and **absolute `UICLICK` cannot
  > reach a menu whose cursor is driven by relative mouse deltas** rather than
  > the OS pointer (`+set in_mouse 0` does not change this). At that point the
  > honest answer is a physical keyboard, not more automation.
- **WINLIST** — JSON list of visible windows
- **ICONARRANGE [auto|bay]** — apply the desktop icon layout now. Defaults to
  the box's `HKLM\Software\RetroAgent\IconAutoArrange` setting (absent = auto).
  Returns the **post-condition** as JSON — `autoarrange` (the live
  `LVS_AUTOARRANGE` bit), `fflags`/`fflags_autoarrange` (what is persisted),
  `icons` and `screen` — not `OK`. Use it to verify a box rather than trusting
  the agent log. See "Desktop icons: AUTO ARRANGE is the fleet default".

> ### ⚠️ TRIAGE FIRST: IS THE MENU KEYBOARD-NAVIGABLE?
> **A menu that moves its own cursor by RELATIVE MOUSE DELTAS cannot be driven
> by the agent at all.** `UICLICK` sets an *absolute* pointer position, which
> such a menu simply does not follow — on Deus Ex an absolute click at
> `(513,508)` moved the in-game cursor to roughly `(10,230)`. `+set in_mouse 0`
> does not change this; it is not a setting, it is how the menu reads input.
>
> This one question predicts the outcome before you spend an hour on it:
>
> | | outcome |
> |---|---|
> | **Keyboard-navigable menu** | works first time — Descent 1, RA2/Yuri, Quake III, UT99, CS 1.6 |
> | **Relative-mouse menu** | **not automatable** — SoF2 (menu *and* CDKEY dialog), Descent 3, Deus Ex |
>
> So when a title stalls at a menu, ask whether it is keyboard-navigable. If it
> is not, **the honest answer is that it needs a human once** — to enter a CD
> key, capture a config, or set an option — after which the resulting file is
> staged and every box inherits it. More clicking will not get there, and
> saying so early is worth more than another hour of attempts.
>
> **Paired fact for id Tech 3 (Quake III engine and its forks):** those menus
> **ignore synthetic keyboard input in exclusive fullscreen and accept it
> windowed.** So for any typing step on that engine, relaunch with
> `+set r_fullscreen 0`, type, then restore fullscreen. Measured on SoF2:
> identical `UIKEY TEXT:` landed nothing fullscreen and filled the field
> immediately windowed.

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

## A STATIC IMPORT WIN9x LACKS KILLS THE EXE AT LOAD — resolve it dynamically (REQUIRED)

**The agent must import nothing Windows 9x cannot resolve. A single unresolved
static import makes the WHOLE PROCESS fail to load — before `main()`, with no
log file, no error dialog and nothing on the box to point at it.** There is no
lazy binding to save you.

Found 2026-08-30 on **.243** (`N5R5L9`, Win98SE, Pentium P54C): the box was
stranded on agent **1.30.0** while the fleet ran 1.78.0, and 1.78.0 would not
start there *at all*. Diffing the two binaries' PE import tables named seven
NT-only entry points that had crept in:

| import | why 9x cannot resolve it |
|---|---|
| `OpenSCManagerA` `OpenServiceA` `ControlService` `QueryServiceStatus` `CloseServiceHandle` `ChangeServiceConfigA` | **Windows 9x has no Service Control Manager** — its `advapi32.dll` exports none of that family (`retrowall.c`, stopping the Themes service) |
| `CM_Get_DevNode_Status` | `setupapi.dll` on NT, **`cfgmgr32.dll` on 9x** (`gamesync.c`) |

Fixed in **1.78.1**: all seven resolve through **`agent/src/ntdyn.c`** with
`GetProcAddress` and degrade gracefully — on 9x `ntdyn_scm_available()` is
false, and the caller logs *"no Service Control Manager on this Windows"* and
skips, rather than failing. `video.c`'s own duplicate loader was folded into it.

- **`service.c` keeps its own, larger dynamic table** (it also needs
  `CreateServiceA`, `StartServiceCtrlDispatcherA`, … for NT service mode). It
  has always been dynamic and was never part of this bug.
- **NOT banned:** the four `SetupDi*`, `AdjustTokenPrivileges`,
  `OpenProcessToken`, `LookupPrivilegeValueA` — 1.30.0 imports them too and runs
  fine on that box. Do not widen the ban list by resemblance.
- **The guard is a PE-import assertion on the BUILT binary**,
  `tests/python/test_agent_win9x_imports.py` — *a source grep would not have
  caught this*: `OpenSCManagerA(...)` is perfectly ordinary C, and it sat in
  files right beside modules that already resolved the same names dynamically.
  ONE direct call anywhere recreates the import.

**Why this test is now a safety mechanism and not a nicety:** `spawn_helper()`
used to pass `lpThreadId = NULL`, which NT accepts and **Win95/98 rejects with
error 87** — so on 9x the `autoupdate`, `retrowall`, `watchdog`, `dosstage` and
`sharelog` threads silently never started. That is the only reason .243 never
pulled the unloadable binary and bricked itself. With that fixed, a 9x box now
auto-updates like any other, so a future NT-only import would take it dark with
**no supervision at all** — the `RetroAgent` Run key fires only at logon, and
recovery needs someone physically at the machine.

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

## The image can ship a driver and still not install it (XP driver ranking)

**On XP, `DriverSigningPolicy=Ignore` suppresses the signature DIALOG, not the
driver RANK.** An untrusted driver node is penalised **+0x8000**
(`#I087 Driver node not trusted, rank changed from 0x00002000 to 0x0000a000`),
so an unsigned INF in `C:\D` can never beat a trusted in-box match. The device
then sits on Windows' own driver at **problem code 0** — it reports itself as
perfect — and nothing anywhere flags it. Found on **.124** 2026-08-29: a
freshly imaged GeForce2 GTS on Microsoft's nv4 6.14.10.5673 at 800x600x16 with
ForceWare 71.89 unused on its own disk. Full narrative: `retro-3dfx/FINDINGS.md`.

Compounding it: **`winnt.sif` carries only the SHORT early driver path (LAN +
chipset)**. Everything else waits for `DevicePath`, which `cmdlines.txt` writes
at **T-12 — after GUI setup has installed the devices**. So graphics, sound,
monitor and mass-storage in `C:\D` are copied, indexed, and never consulted.

**The fix, and the rule going forward:** a driver we must have is declared in
`scripts/pxe/driver-prefs.txt` (`<inf> | <marker in the inf> | why`).
`stage-oem.sh` expands it against the built tree into
`$OEM$\$1\D\PREFER.TXT` (`<hardware id>\t<INF>`), and the agent
**force-installs** those at first logon with
`UpdateDriverForPlugAndPlayDevices`+`INSTALLFLAG_FORCE`, which ignores ranking.
Ad hoc, on a live box: **`DRVUPDATE <hardware-id> [inf-path]`**.

- **Never let a heuristic pick the INF.** Three directories in the image name
  `DEV_0150`; the first one found is `G003\nv4_go.inf` — ForceWare **270.61
  mobile**, a 2011 driver for a 2000 card. Match on the build (`DriverVer
  7.1.8.9` = 71.89).
- **`gs_reclaim_drivers()` must never run before the force-install** (agent
  **1.59.0**). It used to delete the 2.4 GB `C:\D` payload whenever no device
  carried a problem code, which left .124 with neither the right driver nor the
  means to fix itself. It now also refuses while a preference that applies to
  *this* machine is unsatisfied — preferences for absent hardware never block,
  or the list would refill the 6 GB Gateway the reclaim exists for.
- Logic in `agent/shared/drvprefs.h`; tests `tests/native/test_driver_prefs.c`
  and `tests/test_pxe_drivers.py`.
- **A device XP leaves UNCONFIGURED does not need a preference** —
  `gs_install_missing_drivers()` already handles those. This mechanism is only
  for devices Windows configures *badly* and therefore reports as fine.

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
> - **The chat daemon used to exit when discovery found no agents** — with
>   `Restart=always` that made a permanent rescan loop the steady state on a
>   host with the fleet powered down, and it made `daemon: NOT RUNNING` normal,
>   so the status check could not tell "fleet is off" from "the daemon is
>   broken". **Fixed 2026-08-28:** it now stays up with zero hosts and claims
>   machines as they boot (`rediscover()` already did the adding). It is safe
>   to leave enabled. If you see it exiting on an empty fleet again, that fix
>   has regressed — `tests/python/test_chat_daemon_conn_safety.py` guards it.
> - Judge host health by the **brain's heartbeat** and the daemon's *ability* to
>   claim — never by a live agent count.

Current fleet is on **192.168.1.0/24** (see "Per-box console accounts" above for
the full list).

> ### ⚠️ EVERY MEASURED FACT ABOUT A BOX LIVES IN [`docs/fleet-inventory.md`](docs/fleet-inventory.md) — NOT HERE
>
> **That file is generated, and this section deliberately holds no CPU, no
> card, no resolution, no free-space figure.** Each machine publishes its own
> hardware record to the share on every agent startup
> (`agent/src/hwpublish.c` → `…\Utility\Retro Automation\fleet-inventory\<host>.json`),
> and `scripts/fleet/inventory.py` renders those records into that document:
>
> ```bash
> python3 scripts/fleet/inventory.py            # regenerate docs/fleet-inventory.md
> python3 scripts/fleet/inventory.py --check    # exit 1 if a box is stale or missing
> ```
>
> **Why the table that used to be here is gone.** It was hand-maintained and it
> was wrong about most of the fleet. **Twice a box's graphics card was swapped
> without the docs noticing** — `.124`'s Voodoo 3 came out on 2026-08-11 and the
> stale claim survived for weeks, and `.133`'s Voodoo5 6000 is physically gone
> while three documents still named the box by it. Three machines carried a
> DX9-or-better GPU nothing mentioned, which would have wrongly refused
> 2004-era titles on five of eight boxes. A replacement table was *measured*
> on 2026-08-30 and was already drifting within the day.
>
> **Generating a document and keeping the hand-written one solves nothing** —
> it creates a second thing to go stale. So the generated file is the single
> source of truth for every measured field, and what stays here is only the
> prose a probe cannot discover: the traps below, and the per-box notes in
> `scripts/fleet/fleet-roster.txt`. **Do not re-add a specs table here.** If a
> figure is missing from the generated document, fix the probe
> (`agent/src/hwprofile.c`, `agent/src/hwextra.c`), not this file.
>
> **A stale or missing record is not an outage.** The fleet is powered on
> demand, so several boxes legitimately carry old data at any moment;
> `stale`, `never seen` and `unreadable` are three distinct states in that
> document and none of them is by itself a fault.

Per-box traps worth keeping — **prose only; the numbers are in the generated
file** and the roster line for each box repeats the one-liner:

- **.171's Voodoo 2 NEVER shows as a display adapter.** Its INF is `Class=MEDIA`,
  so `VIDEODIAG` and every display-class scan report only the Intel chip and the
  card looks absent. Detect it with `REGREAD HKLM SYSTEM\CurrentControlSet\Enum\PCI`
  → `VEN_121A&DEV_0002` (NB `VEN_1102&DEV_0002` is a Creative SB Live!, not a
  Voodoo). Two identical cards share ONE device key with separate instance
  subkeys, so descend a level to count them. **This box also answers slowly —
  use ≥8s TCP timeouts or sweeps miss it entirely.**
- **.124** had its Voodoo 3 removed 2026-08-11 and the whole 3dfx stack purged;
  it is on ForceWare 71.89. XP is on **D:**, Win98 on C:, and games live on both.
- **.145's `DISPLAYCFG get` reads the INACTIVE Intel HD**, not the discrete card
  driving the panel, so it reports a far smaller mode than the desktop really
  has. Cross-check against a `WINLIST` Program Manager rect before believing a
  suspiciously small mode on a dual-adapter box. (The generated inventory
  sidesteps this: it reports the **persisted** mode from the registry, which is
  also immune to a game exiting without restoring — `.123` and `.240` were both
  found sitting at 640x480 from a DOSBox leftover while driving 1080p panels.)
- **.133 sits just UNDER the 256 MB floor several 2004 titles publish.** That is
  not a rounding artifact — it is the number the capability gate sees. Read the
  exact figure from the generated inventory, not from memory.
- **.240 is the only disk-constrained machine still in service.** Check its free
  space in the generated inventory before staging anything large on it.

> #### ⚠️ THE VOODOO5 6000 IS NO LONGER IN `.133` (measured 2026-08-30)
>
> A draft of the table that used to live here said the V5 6000 was still a
> second adapter in `.133`. It is not, and that draft was written from the
> *previous* table rather than from a probe — which is the whole argument for
> generating this. Four independent reads all say the card is physically
> absent:
>
> - `VIDEODIAG` returns **one** adapter, the GeForce4 Ti 4600;
> - `HKLM\SYSTEM\CurrentControlSet\Enum\PCI` contains **no `VEN_121A` key at
>   all** — and a physically present card enumerates there even with no driver
>   bound, so this is the decisive one;
> - the Display class GUID has a single instance, `\0000`;
> - there is no `3dfx*` service and **no `glide*.dll` anywhere under `%SystemRoot%`**.
>
> **So the vintage Voodoo5 lane is down to ONE box, `.143`** — and even there the
> V5 5500 is the *second* adapter behind a GeForce 6800 that drives the panel.
> The `voodoo5-driver-dev` skill still names ".143 (V5 5500) and .133 (V5 6000,
> 4-chip)"; the `.133` half of that has no hardware behind it, exactly as the
> `voodoo3-driver-dev` skill lost `.124` when its Voodoo 3 came out on
> 2026-08-11. Do not size a Voodoo5 test matrix at two boxes.
>
> **Real Glide silicon on this fleet is now exactly two cards:** `.143`'s V5 5500
> (`121A:0009`, subsys `0002121A`) and `.171`'s Voodoo 2 (`121A:0002`) — and
> this no longer has to be remembered: every box now reports its own
> `accelerators[]` from the PCI enumerator, so `docs/fleet-inventory.md` says
> which machines have 3dfx silicon and states positively where there is none.
> Every
> other box would run a Glide title through a wrapper or not at all — which is
> what makes the staged game-local `glide2x.dll` question a real one.

### A game-local nGlide in the LIBRARY hides a real Voodoo from every box that has one

**Game-local wins at load time.** Two staged titles ship a 1,310,720-byte nGlide
`glide2x.dll` beside their exe — `UnrealGold/System/` and `Carmageddon2/` — and
that wrapper shadows the real 3dfx `glide2x.dll` in `system32` (226,304 B on
`.171`, 258,048 B on `.143`). So on **exactly the two boxes that still have Glide
silicon**, the staged library guarantees the card is never used.

This has already been diagnosed once the expensive way, on `.171` (commit
`7823586`): UnrealGold was reported as *crashing*, had never crashed, and had in
fact spent the whole session on the software rasterizer at 100% CPU — the
wrapper's `grSstOpen` failed `(2, 3)` every time, so the game got neither the
card nor a working wrapper. **That fix was applied to the box, not to the
library**, so the next `GAMESYNC` restores the wrapper. Carmageddon2 carries the
identical wrapper and is still untested.

The staged `Unreal.ini` also still carries the values that stranded that session
(`WindowedRenderDevice=SoftDrv.SoftwareRenderDevice`, and a 1024x768x32 mode no
Voodoo 2 can scan out), so the library reproduces the bug on demand.

**The fix belongs in the per-box launcher, not in a staged constant** — this is
the same shape as the resolution problem: one staged tree, eight boxes, and any
single baked-in answer is wrong somewhere by construction. A box with real Glide
silicon wants the real `system32` Glide and `GlideDrv`; a box without wants
`D3DDrv` and may keep the wrapper. Deleting the staged wrapper outright is *not*
obviously right — it is the only Glide path the six non-3dfx boxes have.


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

## One Staged Tree, Eight Monitors — the resolution is PER BOX (REQUIRED)

**A resolution written into a staged config is wrong somewhere by
construction.** The fleet is four 1920x1080 16:9 LCDs (`.123` `.145` `.240`
`.246`) and four CRTs — `.124` at 1024x768, `.143` with no readable EDID, and
**`.133` and `.171` are 4:3 TUBES** that were being driven at 1280x1024, i.e.
5:4 and visibly squashed. Until 2026-08-30 every title was pinned at 1024x768
and Tiberian Sun at 640x480.

So the fix lives **inside the staged tree and runs at launch**: each affected
title stages `FLEETRES.EXE` (54,784 B, mingw, pure Win32, **no SSE** so it is
safe on `.124`/`.133`/`.143`) plus `FLEETRES.BAT`, and its `Play <Game>.bat`
calls them before starting the game.

```bash
python3 scripts/fleet/stage-fleetres.py            # apply / re-apply, idempotent
python3 scripts/fleet/stage-fleetres.py --check    # is the share current?
```
Source and payload: `provisioning/fleetres/`. Tests:
`tests/python/test_fleetres_staging.py` plus new share-side checks in
`scripts/validate-staged-library.py`.

**Four findings this encodes — do not re-derive them:**

- **`wmic Win32_VideoController.CurrentHorizontalResolution` is WRONG.** It
  reported 640x480 on `.123` while the box was really at 1024x768. Use
  `EnumDisplaySettings`.
- **Never derive a target from the LIVE desktop mode.** A game that exits
  without restoring leaves the desktop at 640x480 — `.123` and `.240` were both
  found sitting there — so a launcher trusting it pins every LATER game to
  640x480 for good. Read the PERSISTED mode (`ENUM_REGISTRY_SETTINGS`).
- **DOSBox `fullresolution=original` changes the WHOLE DESKTOP** to the DOS
  mode (proved with `DISPLAYCFG` on `.145`). On a 16:9 LCD that is a stretched
  640x480 upscale left behind after a crash; `desktop` + `aspect=true`
  pillarboxes correctly. But `original` is right on a CRT — so neither value can
  be a staged constant, and the launcher writes it per box.
- **id Tech 3's `r_mode`/`r_customwidth`/`r_customheight`/`r_fullscreen` are
  `CVAR_LATCH`** — read once at renderer init. A staged `seta r_mode "6"` runs
  after `Com_StartupVariable` and before `R_Init` and therefore **beats the
  command line** (measured on `.123`). Those setas are now deleted from every
  staged `autoexec.cfg`, which ends with `exec fleetres.cfg` instead — a file
  the launcher writes fresh at every start. **Never put `seta r_mode` back**;
  the validator fails the library if you do. Note also that a per-user
  `%APPDATA%\Quake3\baseq3\autoexec.cfg` (retro-gameindex writes one)
  SHADOWS the staged file entirely, which is why the command line is kept as a
  second route. `+vid_restart` also fixes the latch and **exits ioquake3
  outright on `.246`** — not usable fleet-wide.

**Per-box ceiling** for hardware that cannot drive its own monitor:
`HKLM\Software\RetroAgent` `ResCapW`/`ResCapH` (REG_DWORD). Set to 800x600 on
`.171`, whose 3D is a **Voodoo 2** with a hard 800x600 limit hiding behind the
Intel 865G that every display-class scan reports instead.

### ⚠️ id Tech 4 IS THE OPPOSITE OF id Tech 3 HERE — the command line WINS

Everything above about stripping a latched `seta r_mode` is an **id Tech 3**
rule. **DOOM 3 (id Tech 4) re-applies `+set` startup variables a SECOND time,
after it has exec'd `DoomConfig.cfg`** — id's own source comments it
*"re-override anything from the config files with command line args"* — so the
launcher's command line beats the config and the title needs **no
`fleetres.cfg` at all**. Applying the idTech3 recipe there produces a launcher
that looks right and changes nothing.

Two more id Tech 4 specifics, both silent when wrong:
- the cvars are **camel-case**: `r_customWidth` / `r_customHeight`.
  idTech3's lower-case spelling is a *different name* and is ignored.
- **`r_aspectRatio` is a separate cvar** (0=4:3, 1=16:9, 2=16:10) and the engine
  derives horizontal FOV from it, so a 16:9 panel at the right pixel count with
  the default 0 is still stretched. `Doom3`'s launcher computes it per box from
  `%FR_W%`/`%FR_H%` by integer cross-multiply.
- **`DoomConfig.cfg` and `config.spec` must NOT be staged.** Both are per-box
  state the engine writes on exit; a copy captured on one machine carries that
  machine's resolution and detected machine-spec onto all eight.

## Copy protection: read the SafeDisc VERSION before you plan around it (REQUIRED)

**A staged title that says *"Cannot locate the CD-ROM/DVD-ROM"* is not
necessarily missing a CD key, and the disc image is usually not what is
short.** Doom 3 was tracked for a session as "one plain-text file away" when
retail 1.0 in fact raises that modal **before any key prompt**.

- **Identify it:** the PE has sections `stxt774` / `stxt371` and the string
  `BoG_ *90.0&!!  Yy>`.
- **Get the exact version, it is three dwords:** immediately after that marker
  (file offset `0xfd4` on both binaries measured here) sit major/minor/subminor
  — `3 / 0x14 / 0x16` = **SafeDisc 3.20.022** (Doom 3 retail),
  `2 / 0x50 / 0x0a` = **SafeDisc 2.80.010** (C&C Generals).
- **Look for an OFFICIAL patch that drops the wrapper before reaching for a
  mounter.** id's Doom 3 1.3 does — its `Doom3.exe` has six ordinary sections
  including `.reloc`, no `stxt*`, no `BoG_`. Pull it without installing: run the
  InstallShield setup once and `7z x` `%TEMP%\_is2\<name>.msi`. EA's Generals
  ZH 1.04 does **not** — it is an RTPatch binary delta over `game.dat` and never
  touches the wrapper.
- **NEVER compare candidate exes by SIZE.** id's official 1.3 exe and the scene
  crack of it on the same share are **both 5,832,704 bytes**. Compare md5.
- **DAEMON Tools 3.47 — the fleet's only mounter — does NOT satisfy SafeDisc
  2.80**, and the image is not the reason: the `.mdf` pairs here are
  2448-byte-sector dumps (2352 + 96 **subchannel**) with a real weak-sector
  table. Its emulation is set from the tray menu, and the four entries are
  **TOGGLES** — only *"All options ON"* is a SET — so verify the post-condition
  (checkmarks **and** a changed `d347bus\Cfg\khjeh` blob), never the click.

## Keys and Secrets — Azure Key Vault `nsc-secrets-kv` (REQUIRED)

**Every product key, CD key, serial and credential this project depends on
lives in Azure Key Vault `nsc-secrets-kv`. The repo carries the *name* of a
secret and the command that fetches it — never the value.** Full working
detail, including the per-engine table of where each game keeps its key, is in
the **`fleet-keyvault` skill** (`.claude/skills/fleet-keyvault/SKILL.md`);
`scripts/pxe/SECRETS.md` remains the register for the PXE/XP-install secrets.

```bash
python3 scripts/fleet/keyvault.py list           # every fleet-* secret
python3 scripts/fleet/keyvault.py show <name>    # metadata + tags, NOT the value
python3 scripts/fleet/keyvault.py get <name>     # the value, on stdout
```

- **Naming:** `fleet-gamekey-<title>` for game keys, `fleet-winxp-*-key` for
  Windows product keys (named after the media they were verified against),
  `fleet-<service>-*` for host credentials. Currently vaulted game keys:
  `half-life-goty` (which every GoldSrc title on a box shares — HL, CS 1.6,
  Opposing Force, Blue Shift), `half-life-opposing-force`, `halo-pc`,
  `quake3-team-arena`, `sof2`, `ut2004`. **A vaulted key is not a staged
  title**: `fleet-gamekey-halo-pc` is `verified=pending` because the key has
  never validated, and Halo is **not** staged.
- **Every secret carries a `verified=` tag** recording *what proved it works* —
  `pending`, or a date plus the evidence, or `REJECTED by <media> on <date>`. A
  rejected key is **kept**, so nobody re-tries it (that is what
  `fleet-winxp-pro-sp3-product-key` is for). A key that merely exists is not a
  key that works.
- **Never pass a secret on the command line.** `keyvault.py set` takes `--file`
  or `--stdin` only; argv lands in shell history, in `ps`, and in transcripts.
- **`contentType` is capped at 255 chars** and Key Vault rejects a longer one
  with `Property  has invalid value`, naming no property. The helper checks first.

### ⚠️ `aisleprompt-kv` is a DIFFERENT vault and is not ours

There is a second vault, `aisleprompt-kv`, belonging to the AislePrompt
project, which the user has said is not to be changed. **Never read from it,
write to it, or reference it.** `keyvault.py` refuses it in code and
`tests/python/test_keyvault.py` asserts that it does. (`nsc-secrets-kv` also
holds some non-fleet entries — `file-aisleprompt-env`, `amazon-creators-*`.
Stay inside `fleet-*`.)

### THE VAULT IS THE SYSTEM OF RECORD, NOT A RUNTIME DEPENDENCY

**A staged `install.reg` carries the literal key, and that is correct.** A
Windows `.reg` has no syntax for indirection — `regedit /s` merges bytes — and a
retro PC must never need the internet to start a game. `GAMESYNC` copies a tree
byte-for-byte with no hook where a secret could be substituted, and adding one
would mean putting Azure credentials on the fleet, which is far worse than a CD
key sitting in a file on an isolated LAN.

So the vault answers *"what was that key?"* — after a NAS rebuild, when staging
a title on a second library, when a dialog needs it typed by hand. **Do not
"improve" a launcher, an `install.reg` or `GAMESYNC` into fetching a key at run
time.** The right shape is `scripts/pxe/make-xp-source.sh`, which pulls
`PRODUCT_KEY` from the vault **on the Linux host at image-build time** so what
reaches the fleet is a finished artifact.

### THREE categories, not two — the third is the one that breaks multiplayer

When you find a key-shaped value, classify it as one of **three** things. The
expensive mistake is folding the third into the first.

1. **A real per-copy secret** — a product key, a CD key, a credential.
   **Vault it**, tagged, and keep the literal in the staged tree.
2. **A deliberately-public fleet convention** — see below. **Document why**,
   rather than silently leaving it or pretending it is protected.
3. **Per-installation, machine-local state.** **Leave it alone, and say so.**
   It is not a secret; it is a value that must be *different* on every box, and
   centralising it — in `install.reg` or in a vault, identically — is what
   breaks LAN play.

`HKLM\SOFTWARE\Westwood\<game>\Serial` is category 3. It is generated **on
the box** by the launcher `.bat` from the system drive's volume serial. One
value handed to every box makes RA2/Yuri refuse the second machine with *"There
is already a player with your serial# in that game"*; **Tiberian Sun has the
same lineage and the same mechanism**, and a two-box LAN test on 2026-08-30
produced two different eleven-digit serials, exactly as intended. Vaulting
either would be actively wrong. The whole class is audited in
`Games-Library/_patches/PER-BOX-VALUES.txt`.

### Deliberately NOT secret — documented, not hidden

`retro-agent-secret` (the agent's shared secret, compiled in as the default and
assumed by ~20 scripts), `password` (the console account on every box, which XP
auto-login requires in cleartext), and the game servers' `retroadmin` /
`retro-vanilla` / `retro-noblood` rcon passwords are **fleet-wide conventions on
an isolated LAN with no WAN exposure**. Vaulting them would be theatre and would
break every script that assumes the default. Rotating the agent secret is a
fleet-wide operation — see the `security-posture` skill, not this one.
`SMB_CREDS = user:password` in `agent/Makefile` is a placeholder with `# EDIT:`
beside it.

### Never commit a literal

`tests/python/test_no_committed_secrets.py` greps every tracked text file for
key-shaped literals, for private-key/connection-string markers, and — the
catch-all — for the **actual values** of every `fleet-gamekey-*` secret pulled
live from the vault. It skips **loudly** when `az` is unavailable. **If you find
a secret in git history, report it and ask the user before rewriting history** —
many worktrees track this branch. (Swept 2026-08-30: history is clean.)
