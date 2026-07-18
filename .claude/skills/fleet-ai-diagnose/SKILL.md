---
name: fleet-ai-diagnose
description: Health-check the retro-infer AI stack across the fleet — engine reachability, zombie retro-infer.exe processes, stale/mismatched engine versions, and recovery via AI_RESTART. Use when the user asks "is the AI stack healthy", "check for zombie processes", "why is AI_HELLO failing / engine unavailable", "the fleet AI seems broken", or before starting a big distributed training/inference run.
---

# Fleet AI stack health check

A per-box checklist for the retro-infer engine layer, distinct from the
general agent (that's a different health surface). Companion:
[[fleet-ai-train]] (what to run once healthy), [[fleet-ai-monitor]] (the
live console, which surfaces some of this passively via `AI_HELLO`
reachability in the fleet table).

## Step 1 — Reachability + readiness

```python
import asyncio, json
from client.retro_protocol import RetroConnection

async def check(ip):
    c = RetroConnection(ip, 9898)
    await c.connect('retro-agent-secret', timeout=8.0)
    s, d = await c.send_command('AI_HELLO', timeout=15)
    print(ip, json.loads(d))
    await c.close()
```

Look at `ready` (should be `1`) and `host_gpu.driver_flag` (a human-readable
sentence — flags GPU/driver mismatches). `"AI engine unavailable"` at this
step means `retro-infer.exe` isn't staged or isn't responding — go to Step 3.

## Step 2 — Zombie process check (the known recurring issue)

```
PROCLIST
```

Count `retro-infer.exe` entries. **More than one is a real bug, not normal**
— the agent supervises exactly one `--serve` child per box. This has a known
historical root cause on every non-3dfx (nv-gl) box: `nvgl_shutdown()` used
to call `FreeLibrary()` on `opengl32.dll`, which hangs a driver cleanup
thread on some GPU stacks (confirmed on NVIDIA) — every one-shot
`--nv-check`/`--nv-check-multi`/`--bnn-eval` invocation left an orphaned OS
process behind. Fixed in commit `7eb9848` (`retro-infer/src/gpu/nv_gl.c`) —
if zombies are still accumulating on a box running that fix or later, it's
a *new* bug, not a recurrence of the known one; check the engine version
first (Step 3) before assuming it's the same root cause.

**Recovery**: `EXEC cmd /c taskkill /f /im retro-infer.exe` (kills all of
them) then `AI_RESTART` (respawns exactly one clean instance). Verify with
`PROCLIST` again and a fresh `AI_HELLO`.

## Step 3 — Engine staleness vs the share

The agent self-stages `retro-infer.exe` from the SMB share
(`agent/src/ai.c:stage_engine_from_share`) but **only on missing-file or a
byte-size mismatch, and only at agent startup / first `AI_HELLO`-triggered
spawn** — it does not periodically re-check. A box can silently run an old
engine build indefinitely if its agent hasn't restarted since a new engine
was published. To force-refresh a specific box without waiting for a
restart: `taskkill /f /im retro-infer.exe` the running engine, `UPLOAD` the
current `retro-infer/retro-infer.exe` build directly to
`C:\RETRO_AGENT\retro-infer.exe` (a ~2s wait after taskkill avoids a
"Cannot create file: error 32" sharing-violation race), then `AI_RESTART`.

Cross-check `AI_HELLO`'s `version` field and `docs/machines/
ai-capability-profiles.md`'s per-box notes for drift — that doc is a
snapshot, re-verify against a live `AI_HELLO` rather than trusting it blindly
if it's been a while.

## Step 4 — Acceptance re-verification (optional, after any engine change)

```
EXECW 120 C:\RETRO_AGENT\retro-infer.exe --nv-check     # or --glide-check on a 3dfx box
EXECW 300 C:\RETRO_AGENT\retro-infer.exe --nv-check-multi
```

Both should report `OK`/`0 mismatches`. Run 3-5 times back to back and
`PROCLIST` before/after — this is exactly how the zombie-process regression
in Step 2 gets caught early, so it's worth doing after any engine rebuild,
not just when something already looks broken.

## Notes

- `docs/machines/ai-capability-profiles.md` has the full per-box hardware
  context (CPU ISA, GPU vendor, expected backend) if a `driver_flag` message
  needs interpreting.
- Restarting the engine (`AI_RESTART`) is safe and fast — it does not affect
  the agent's own connection or other agent commands, only the supervised
  `retro-infer.exe` child. It does drop any resident models on that box
  (`MODEL_LOAD` again after).
