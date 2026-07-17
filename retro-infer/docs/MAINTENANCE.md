# Fleet AI maintenance — deploy story, gotchas, extension recipes

How to keep the Fleet AI stack alive and how to extend it without breaking the
parity contracts. Companions: [ARCHITECTURE.md](ARCHITECTURE.md),
[ALGORITHMS.md](ALGORITHMS.md),
[TRAINING-AND-INFERENCE.md](TRAINING-AND-INFERENCE.md).

## Versioning + deploy story (agent ↔ engine)

Two independently versioned binaries per box:

- **Agent** (`retro_agent.exe`, currently **v1.10.0** — the first with the
  full AI capability surface): versioned by `v*` git tags, auto-updates from
  the SMB share by comparing its compiled version against the published
  `retro_agent.exe.ver` sidecar
  ([`agent/src/autoupdate.c:345-358`](../../agent/src/autoupdate.c#L345);
  the `HKLM\…\LastUpdateVer` stamp guards against update loops,
  [`agent/src/autoupdate.c:86`](../../agent/src/autoupdate.c#L86)). Publish
  rules (latest pointer + versioned archive + `.ver` in lockstep) are in the
  repo-root docs — always bump before shipping.
- **Engine** (`retro-infer.exe`): versioned by `infer-v*` tags
  ([`Makefile:15`](../Makefile#L15), `make release` to bump). It is staged
  **next to the agent** (`C:\RETRO_AGENT\retro-infer.exe`) because the agent
  spawns it from its own directory
  ([`agent/src/ai.c:97-99`](../../agent/src/ai.c#L97)). There is **no engine
  auto-update**: push new builds over the agent link
  (kill → move → `AI_RESTART`; exact commands in
  [TRAINING-AND-INFERENCE.md](TRAINING-AND-INFERENCE.md#deploy-the-engine-to-a-fleet-box)).
  `AI_HELLO` reports the running engine version
  ([`src/serve.c:180-190`](../src/serve.c#L180)) — verify after every push.

Transport compatibility: agent v1.9.1+ has the generic `AI_RAW`/`AI_RAWP`
pass-through ([`agent/src/ai.c:424-427`](../../agent/src/ai.c#L424)), so **new
engine verbs need no agent release** — only new two-frame semantics or
host-side detection changes do. Resident models live in `models\<name>.rim`
next to the agent ([`agent/src/ai.c:335-338`](../../agent/src/ai.c#L335)) and
survive engine restarts on disk (but must be re-`LOAD`ed — the resident table
is in-memory, [`src/serve.c:65`](../src/serve.c#L65)).

## Fleet gotchas

Hard-won; each one cost a broken run or a locked-out box.

- **Win98 RST crash** — abrupt TCP disconnects crash Win98 Winsock. Always
  `await conn.close()`; the brain tooling enforces this
  ([`scripts/retro_brain_tools.py:98-114`](../../scripts/retro_brain_tools.py#L98)).
- **Orphaned-batch socket-inheritance lockout** — restarting the agent via a
  detached batch used to leak the listening socket into the orphaned
  `cmd.exe`; the next agent then failed to bind :9898 and came up on the alt
  port only (hit on .143 during the v1.9.0 rollout). Fix: listen sockets are
  marked non-inheritable
  ([`agent/src/main.c:601-604`](../../agent/src/main.c#L601) and
  [`:637`](../../agent/src/main.c#L637)). **Recovery on an affected old
  agent**: connect to the alt port **9897**
  (`AGENT_TCP_PORT_ALT`, [`agent/src/protocol.h:9`](../../agent/src/protocol.h#L9)),
  `PROCKILL` the stale `cmd.exe`, restart once more (commit `22f3f35` ops
  note). Same reasoning applies to any child the agent spawns
  ([`agent/src/exec.c:64`](../../agent/src/exec.c#L64)).
- **Dual-boot Windows-on-D:** — some boxes (.124) boot XP from **D:** while
  the running agent exe may live on the C: (Win98) volume. Never assume
  `%SystemDrive%`; confirm with
  `EXEC wmic process where "name='retro_agent.exe'" get ExecutablePath`
  before swapping binaries. Display-driver files are under
  `D:\WINDOWS\system32` there
  ([`docs/machines/ai-capability-profiles.md`](../../docs/machines/ai-capability-profiles.md#124--pentium-iii--voodoo3)).
- **glide3x.dll must match the display driver** — a vendor-kit `glide3x.dll`
  can load fine yet fail `grSstWinOpen` ("no Voodoo?") because it doesn't
  match the installed retro3dfx display driver (resolved driver flag on .124).
  Rule: the Glide runtime staged next to the agent must be the build paired
  with the installed `3dfxvs.dll`. The agent surfaces exactly this as
  `driver_flag` in `AI_HELLO`
  ([`agent/src/ai.c:207-217`](../../agent/src/ai.c#L207) `host_gpu_json`) and
  on the startup banner
  ([`agent/src/ai.c:264`](../../agent/src/ai.c#L264) `ai_status_thread`).
- **Hung Glide compute / kill–wait–poll** — the glide backend goes fullscreen;
  a wedged run must be killed, then **poll `PROCLIST` until the process is
  really gone** before relaunching (discipline documented in
  [`benchmarks/README.md`](../../benchmarks/README.md), item 3). For the
  engine specifically, `AI_RESTART`
  ([`agent/src/ai.c:454`](../../agent/src/ai.c#L454)) shuts down/respawns it;
  the crash-isolation design means a dead engine never takes the agent with it
  ([`agent/src/ai.c:5-9`](../../agent/src/ai.c#L5)).
- **Watchdog thread** — if any agent command wedges >75 s *while a known
  fullscreen game is running*, the watchdog kills the game and restores the
  desktop display mode
  ([`agent/src/watchdog.c:33-38`](../../agent/src/watchdog.c#L33) thresholds,
  [`:86-95`](../../agent/src/watchdog.c#L86) recovery). The game-running guard
  means long AI jobs (a big `EXECW` training run) never trigger a false
  recovery — but note `retro-infer.exe` is *not* in the watchdog's game list;
  a hung engine is `AI_RESTART`'s job.
- **One AI command at a time per box** — the agent serializes all AI traffic
  through a critical section
  ([`agent/src/ai.c:29-48`](../../agent/src/ai.c#L29)) and the engine serves
  one client ([`src/serve.c:21`](../src/serve.c#L21)). Don't build
  coordinators that open parallel AI streams to the same box; parallelism is
  *across* boxes.
- **Frame cap** — the engine rejects frames >32 MB
  ([`src/serve.c:52`](../src/serve.c#L52) `MAX_SRV_FRAME`); shard big datasets
  (`GBINIT` payloads, `NTSTEP` batches ≤1024,
  [`src/serve.c:406`](../src/serve.c#L406)).

## Adding a new op to the executor

1. **Spec first**: extend
   [`tools/rim/FORMAT.md`](../../tools/rim/FORMAT.md) — it is the binding
   contract ("change here first",
   [`tools/rim/FORMAT.md:1-4`](../../tools/rim/FORMAT.md#L1)).
2. **Reference first**: implement the op in
   [`tools/rim/eval_ref.py`](../../tools/rim/eval_ref.py) (or a dedicated
   reference like `eval_bnn_ref.py`) and generate a **mini fixture**
   (`.rim` + input + expected bytes) via the pattern in
   [`tools/rim/gen_eval.py`](../../tools/rim/gen_eval.py) — single-op models
   are how executor bugs get localized.
3. **C executor**: add the enum member
   ([`src/exec.c:17-20`](../src/exec.c#L17)), manifest parsing + validation in
   `model_open` ([`src/exec.c:124-199`](../src/exec.c#L124)), the
   shape-trace rule ([`src/exec.c:216-253`](../src/exec.c#L216)), and the
   forward case in `model_infer`
   ([`src/exec.c:404-544`](../src/exec.c#L404)). Kernel-level math goes in
   [`src/ops/nn.c`](../src/ops/nn.c). Respect the buffer model: f32 (`af`) or
   i8+scale (`ai`) or packed bits (`abit`), one live at a time.
4. **Rounding**: any new quant math uses `ri_round`/`ri_quant_clamp`
   ([`src/ops/nn.c:14-28`](../src/ops/nn.c#L14)) — half-away-from-zero, never
   `rint()`.
5. **Parity**: host build (`make host`) vs the reference on the mini fixture,
   then the full-model eval, integer bit-exact / f32 tolerance per
   [`../README.md`](../README.md#rules-that-keep-parity-exact). Then deploy
   and re-verify on a real box (`--eval` + remote `INFER_RUN`).

## Adding a new ISA kernel

The pattern that keeps a `-march=i586` binary safe on a plain Pentium:

1. New TU under `src/ops/` — it is the **only** file compiled with the ISA
   flag. Add it to the Makefile's per-ISA list + rule
   ([`Makefile:33-40`](../Makefile#L33), rules
   [`Makefile:59-69`](../Makefile#L59) — copy the `.sse.o`/`.mmx.o`/`.3dnow.o`
   shape).
2. Declare the entry points in [`src/kernels.c`](../src/kernels.c) (top-of-file
   extern block, [`src/kernels.c:9-27`](../src/kernels.c#L9)) and wire them in
   `kernels_init` **behind the matching `cpu_caps_t` bit**
   ([`src/kernels.c:31-58`](../src/kernels.c#L31)). Keep the preference order
   explicit; extend `kernels_force_scalar`
   ([`src/kernels.c:60`](../src/kernels.c#L60)) so `--scalar` still covers it.
3. Numeric contract: match the scalar oracle's accumulation order if bit-exact
   parity is claimed; otherwise document the tolerance (see the SSE-NT/3DNow!
   precedent, [ALGORITHMS.md](ALGORITHMS.md#gemm-kernels-runtime-isa-dispatch)).
4. **MMX/3DNow! only**: obey the x87-state rule — extract accumulators as
   integer bits, `femms`/`emms` before any float tail
   ([ALGORITHMS.md](ALGORITHMS.md#the-x87-under-mmx-state-hazard)).
5. If a host build should exercise it, add it to `HOST_SRCS`
   ([`Makefile:76`](../Makefile#L76)); guard non-x86_64 ISAs like 3DNow! with
   `RI_NO_3DNOW` ([`src/kernels.c:16`](../src/kernels.c#L16)).
6. Verify: `--selfcheck` shows the new kernel name (they flow into
   `hello_json` and `AI_HELLO` automatically,
   [`src/serve.c:180-198`](../src/serve.c#L180)); benchmark vectorized vs
   `--scalar`; run the full parity suite; log the before/after to ai_runs.

## Adding a model to the zoo

1. Trainer/exporter under [`tools/rim/`](../../tools/rim) producing a `.rim`
   via `rim_pack.tref`/`write_rim`
   ([`tools/rim/rim_pack.py:1-20`](../../tools/rim/rim_pack.py#L1)); only
   FORMAT.md ops (or extend FORMAT.md + the executor first, above).
2. Reference-execute it (`eval_ref.py`) and commit golden fixtures + a
   `*_report.json` with the achieved numbers (follow
   `tools/rim/out/ref_report.json` / `bnn/out/bnn_report.json`).
3. Verify with `rim_dump.py` (bounds/alignment invariants), run host parity,
   then remote: `MODEL_LOAD` + `INFER_RUN` on ≥2 fleet boxes (the M3
   acceptance shape).
4. Add it to the zoo table in [MODELS.md](MODELS.md#zoo) and log fleet runs to
   ai_runs.

## Security notes

- **`SPECPICKS_DATABASE_URL` must come from the environment.**
  [`scripts/ai_metrics.py:23-26`](../../scripts/ai_metrics.py#L23) currently
  ships a **committed default DSN literal (credentials included) as the env
  fallback — scrub that literal from the source and git history and rotate the
  password**; the env var is the only sanctioned configuration path. (Not
  reproduced here on purpose.)
- The agent secret used by the Python tooling defaults from
  `RETRO_AGENT_SECRET` (e.g.
  [`scripts/retro_ai_fleet.py:32`](../../scripts/retro_ai_fleet.py#L32)); set
  it in the environment on any non-lab deployment.
- The engine listens on **127.0.0.1 only**
  ([`src/serve.c:644`](../src/serve.c#L644)) and must stay that way — all
  remote access goes through the authenticated agent. `MODEL_LOAD`/`TENSOR`
  names are sanitized to `[A-Za-z0-9_-]`
  ([`agent/src/ai.c:305-315`](../../agent/src/ai.c#L305)) to keep path
  traversal out of `models\`.
- The chat brain's destructive-command guardrail
  ([`scripts/retro_brain_tools.py:52-87`](../../scripts/retro_brain_tools.py#L52))
  gates `REBOOT`/`SHUTDOWN`/`PROCKILL`/… behind an explicit `confirm=true`;
  don't weaken it when adding AI tools.
