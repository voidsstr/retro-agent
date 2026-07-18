# Fleet AI capability profiles — per-machine hardware, kernels, drivers

Generated from live inspection of the fleet on 2026-07-17/18/19 (`AI_HELLO` +
`VIDEODIAG` + `retro-infer --selfcheck` + `--glide-check`). This is the
authoritative record of what each box is, which AI code path it uses, and
any driver actions flagged. The agent re-derives all of this at runtime — see
["How the agent detects and reports capability"](#how-the-agent-detects-and-reports-capability)
— this doc is the human-readable snapshot + rationale.

## Summary (6 connected machines)

| Box | Host | CPU | ISA used | GPU | GPU AI backend | f32 / int8 / NT-TN kernels |
|---|---|---|---|---|---|---|
| **.124** | ADMIN | Pentium III 850MHz (fam6/m8) | **SSE** + MMX (no SSE2) | Voodoo3 (PCI 121a:0005) | ✅ glide-mac | sse / mmx / sse |
| **.143** | 1GHZ | Athlon 1GHz classic (fam6/m2) | **3DNow!** + MMX (no SSE) | Voodoo5 5500 (PCI 121a:0009) | ✅ glide-mac | 3dnow / mmx / 3dnow |
| **.240** | USER-41EA3B3330 | Athlon 64 3300+ (fam15/m12) | **SSE + SSE2** + MMX + 3DNow! | Radeon 9800 XT | ✅ nv-gl | sse / **sse2** / sse |
| **.123** | 2004-XP | Athlon 64 4000+ | **SSE + SSE2** + MMX + 3DNow! | Radeon HD 3850 AGP | ✅ nv-gl | sse / **sse2** / sse |
| **.145** | DELL | Core i5-2400 (Sandy Bridge) | **SSE + SSE2** + MMX | Intel HD Graphics | ✅ nv-gl | sse / **sse2** / sse |
| **.82** | WHITEBEAST | AMD Ryzen 9 9950X 16-Core (fam26/m68) | **SSE + SSE2** + MMX (no 3DNow!) | **NVIDIA GeForce RTX 4080 SUPER** | ✅ nv-gl | sse / **sse2** / sse |

int8 kernel dispatch: **SSE2** (128-bit, 8-wide) on the Athlon 64 / Core i5 /
Ryzen boxes, **MMX** (64-bit, 4-wide) on the P3/Athlon-classic (no SSE2),
scalar fallback otherwise — all bit-identical (integer-exact). On .240 the
SSE2 int8 path runs LeNet-5 at 843 img/s vs 536 scalar (1.57×). `.82`'s
Ryzen 9950X is the fastest CPU in the fleet by a wide margin (15.5 SSE
GFLOP/s vs 0.7–0.9 on the older boxes) — but only the SSE2 path is exploited
today; the CPUID detector doesn't check AVX2/AVX-512 yet (open work item),
so a modern box like this is running well below its ceiling.

All: `ready=1` for AI requests, `retro-infer` engine staged next to the
agent (auto-staged from the share since agent **v1.11.0**). **All six boxes
now have a working GPU AI backend**: the two Voodoo boxes (.124/.143) run
**glide-mac** (3dfx Glide fixed-function); the four non-3dfx boxes
(.240/.123/.145/.82) run **nv-gl**, a portable OpenGL 1.1 + ARB_multitexture
backend (despite the filename, it's vendor-neutral — verified on Radeon,
Intel integrated, *and* now real NVIDIA GeForce silicon). `AI_HELLO` reports
`nv_gl_status: verified` and lists `nv-gl` in `backends` on all four.

Getting `nv-gl` working took a real debugging pass on the first Radeon box:
an `--nv-check` acceptance run (exact binary GEMM, mirrors `--glide-check`)
first surfaced garbage output from a printf argument-order bug, then a real
render bug (`GL_ALPHA8` textures carry zero RGB, so the accumulation never
worked), then a 2× geometry error, then — isolated by a new
`--nv-check-multi` stress mode once the raw GEMM checked out exact but the
BNN model-level eval didn't — a `GL_PACK_ALIGNMENT` row-padding bug that
only bites at odd output widths (the BNN's 10-class layer). All four are
fixed; full write-up in the code comment at the top of
`retro-infer/src/gpu/nv_gl.c`. BNN CIFAR-10 (the M5 flagship test) now
passes 1000/1000 on **every** non-3dfx box, including `.82`'s real GeForce
RTX 4080 — this closes the M6 milestone's original hardware gap ("no
GeForce box has been online to date").

Per the project's honest-numbers rule: the GPU path is exact everywhere but
usually not the fastest — each box's own CPU bit-packed XNOR wins on raw
throughput (e.g. .240: CPU 1826 MMAC/s vs GPU 368 MMAC/s). The RTX 4080 is
the one exception where the gap narrows sharply: GPU 3720 MMAC/s vs CPU
bit-packed 8070 MMAC/s (2.2×, not the 5–11× gap seen on older cards) — a
genuinely powerful modern GPU actually starts closing the distance on this
fixed-function-only workload. Agents are on **v1.14.0** (self-heal engine
staging + startup AI banner + NVIDIA-aware driver flag).

`.82` (WHITEBEAST) is notable operationally too: it **self-onboarded with
zero manual intervention** — the agent's `stage_engine_from_share` pulled
`retro-infer.exe` from the share the moment `AI_HELLO` was first called, no
staging step required. It's also a real 16-core/32-thread modern desktop, a
sharp contrast to the 383 MB–2 GB RAM Windows XP boxes making up the rest
of the fleet (its own reported RAM is capped at 2047 MB by the 32-bit
`GlobalMemoryStatus` API the agent uses — a real limitation on modern
high-RAM boxes, not a hardware fact).

> Every connected machine — from a 1998 Pentium III to a 2024 Ryzen 9950X
> with an RTX 4080 — now has a working, exact GPU AI backend and full
> capability-reporting parity via `AI_HELLO`.

## .124 — Pentium III / Voodoo3

- **CPU**: Intel Pentium III 850MHz Coppermine, family 6 model 8 stepping 3,
  383 MB RAM. **Has SSE**, no SSE2, no 3DNow!. CPUID brand string empty
  (early P3) — detected via feature bits, not the brand leaf.
- **AI CPU path**: f32 GEMM → **SSE** (`gemm_f32_sse`, ~3.4× the scalar
  path); int8 → **MMX** (`gemm_i8_mmx`); training NT/TN → SSE variants. This
  is the fastest inference box for f32 on a per-image basis.
- **GPU**: 3dfx Voodoo3 AGP, sole display adapter, Windows on the **D:**
  volume (dual-boot box — the XP `3dfxvs.dll`/`3dfxvsm.sys` live under
  `D:\WINDOWS\system32`, not C:). Our open retro3dfx stack is the installed
  display driver.
- **glide-mac**: ✅ works after a **driver fix** (see flag below). Exact
  binary GEMM verified (0 mismatch to 256³, hash `31872c0d`); BNN CIFAR-10 on
  the Voodoo3 = **1000/1000 label agreement** vs CPU, 13.7 img/s.
- **⚠️ DRIVER FLAG (resolved)**: the `glide3x.dll` from the *stock* Voodoo3
  driver kit (`C:\Drivers\voodoo3tm_driver_kit_1.03.04\`) loads but
  **`grSstWinOpen` fails** ("no Voodoo?") — it doesn't match the installed
  retro3dfx display driver. The **working** `glide3x.dll` is the one paired
  with our stack (the same build resident on .143, 348160 bytes). Action
  taken: staged that build to `C:\RETRO_AGENT\glide3x.dll`. **Maintenance
  rule: the Glide runtime next to the agent must be the build that matches
  the installed `3dfxvs.dll`, not a vendor kit.**

## .143 — Athlon / Voodoo5 5500

- **CPU**: AMD Athlon 1GHz "classic" (K7), family 6 model 2 stepping 2,
  511 MB RAM. **Has 3DNow! + 3DNow!-ext + MMX but NO SSE** — this is the
  load-bearing fleet fact: an SSE-only ML build would fall back to scalar
  here and run ~1.7× slower.
- **AI CPU path**: f32 GEMM → **3DNow!** (`gemm_f32_3dnow`, 2-wide pfmul/
  pfadd); int8 → **MMX**; training NT/TN → **3DNow! NT/TN kernels** (added
  in this pass — 1.59× faster than the scalar NT/TN fallback: MLP epoch
  146s → 92s).
- **GPU**: 3dfx Voodoo5 5500 AGP, our retro3dfx stack, the driver-bench box.
  1024×768×32 desktop.
- **glide-mac**: ✅ exact binary GEMM (hash-stable), BNN CIFAR-10 =
  1000/1000 agreement, 15.9 img/s. This box validated the M5 backend first.
- **Driver flag**: none — `glide3x.dll` loads and `grSstWinOpen` succeeds
  with the resident stack.

## .82 — WHITEBEAST: Ryzen 9950X / RTX 4080 SUPER

The odd one out — not vintage hardware at all, and the box that finally
closed M6's real-hardware gap.

- **CPU**: AMD Ryzen 9 9950X, 16 cores / 32 threads (Zen 5, family 26 model
  68), a 2024-era flagship desktop part. **Has SSE + SSE2 + MMX; no
  3DNow!** (AMD dropped 3DNow! after the Bulldozer generation). SSE f32
  GEMM benches at **15.5 GFLOP/s** — 16–20× every other box in the fleet —
  but this is still only the SSE (128-bit) path; the CPUID detector in
  `src/cpuid.c` doesn't check AVX2/AVX-512 (both present on this CPU), so
  it's running well under its real ceiling. A real optimization
  opportunity, not yet taken.
- **AI CPU path**: f32 GEMM → SSE; int8 → SSE2. Remote `INFER_RUN` verified
  **10/10 bit-exact** vs the numpy reference over the agent transport.
- **GPU**: NVIDIA GeForce RTX 4080 SUPER (PCI 10de:2702), 3840×1600 @144Hz.
  The first genuine GeForce card in the fleet.
- **nv-gl**: ✅ exact binary GEMM at every tested tile up to 256³, hash-
  identical to every other backend on the same seed; `--nv-check-multi`
  10/10 varying-size shapes exact; **BNN CIFAR-10 = 1000/1000** agreement
  vs CPU. GPU throughput here (3720 MMAC/s) is ~10× the older Radeon cards
  and closes to within 2.2× of this box's own CPU bit-packed XNOR (8070
  MMAC/s) — the only box in the fleet where the GPU meaningfully narrows
  the gap instead of losing by 5–11×.
- **Driver flag**: `ok: GPU with loadable OpenGL (nv-gl backend available,
  hardware-verified on NVIDIA/Radeon/Intel)` (agent v1.14.0+).
- **Onboarding**: fully automatic. Agent v1.13.0 auto-updated itself from
  the share, and `stage_engine_from_share` pulled `retro-infer.exe` the
  first time `AI_HELLO` was called — no manual staging, no operator
  action. This is the self-heal machinery from earlier in the project
  working exactly as designed on a completely unplanned-for box.
- **Known limitation**: reported RAM is capped at 2047 MB — an artifact of
  the agent using the 32-bit `GlobalMemoryStatus` Win32 API (adequate for
  the vintage fleet's real RAM sizes, a real gap on a modern high-RAM box).
  `GlobalMemoryStatusEx` would fix this; not yet done.

## Optimization rationale (why each path is chosen)

The engine picks kernels at runtime from CPUID, never from a compile-time
assumption (`retro-infer/src/kernels.c:kernels_init`). Preference order:

- **f32 GEMM**: SSE > 3DNow! > scalar. SSE is 4-wide and exact-equal to the
  scalar oracle; 3DNow! is 2-wide with per-step single-precision rounding
  (close but not bit-identical to x87 scalar — f32 parity tests use
  tolerance, integer tests stay exact).
- **int8 GEMM**: MMX > scalar. Integer math is exact either way, so MMX is a
  pure speed win (`pmaddwd` 4-lane).
- **binary/XNOR GEMM**: glide-mac (Voodoo) *or* CPU bit-packed popcount. Both
  are exact. **Honest result**: the CPU bit-packed path is ~11× the Voodoo's
  GEMM throughput (685 vs 61 MMAC/s), so the GPU backend is used for its
  *existence proof* + offload, not raw speed — the roadmap explicitly wants
  honest numbers here.
- **Training (NT/TN GEMM)**: each ISA gets its own forward (NT, dot-product)
  and weight-gradient (TN, axpy) kernel so on-device training never drops to
  scalar. Critical 3DNow! detail: no x87 float op may run while an MMX/3DNow!
  register is live — the kernels extract accumulators as integer bits and
  `femms` before any scalar tail (a mixed-state bug we hit and fixed).

## Self-heal at startup (agent v1.11.0)

Three failure modes seen while bringing new boxes online, each now fixed so a
freshly-added machine reaches `ready=1` without manual staging:

- **Engine not staged** — a box that auto-updated the agent from the share but
  never got `retro-infer.exe` reported *"AI engine not available - files not
  staged"* and every AI command failed. The agent now **auto-stages the engine
  from the share** (`agent/src/ai.c:stage_engine_from_share` — `CopyFileA` from
  `\\192.168.1.122\files\...\retro-infer.exe`, registry-overridable
  `EnginePath`) at startup and on-demand.
- **Wallpaper not staged** — the retrowall thread no-ops when `C:\retro-wall\`
  is empty. Onboarding now stages the wallpaper/theme bundle from the share if
  the box has none (`provisioning/gen_onboard.py`), and the per-box wallpaper is
  deployed with `scripts/retro-wallpaper/deploy_rotation.py <ip>`.
- **Onboarding marked done despite a partial install** — onboard.cmd used to set
  the `Onboarded` flag even when game ZIPs were missing from the share, so the
  box never retried. It now leaves the flag unset when anything is missing and
  retries on the next boot. (The game ZIPs themselves are an operational
  publish step, separate from AI.)

## How the agent detects and reports capability

1. **Engine self-detect** — `retro-infer` runs CPUID at startup
   (`src/cpuid.c`) and probes `glide3x.dll` loadability
   (`src/gpu/glide_mac.c:glide_available`), then reports it all in the
   `AI_HELLO` / serve `HELLO` JSON (`src/serve.c:hello_json`): `backends`,
   `kernel_f32`/`kernel_i8`/`kernel_f32_nt`/`kernel_f32_tn`, the full `isa`
   block, `gpu.glide3x_loadable`, and `ready:1`.
2. **Agent host-GPU augmentation** — the agent enumerates the display
   adapter (`EnumDisplayDevices`) and probes `glide3x.dll` itself
   (`agent/src/ai.c:host_gpu_json`), then splices a `host_gpu` object into
   the `AI_HELLO` reply with a **`driver_flag`** string that calls out
   mismatches (3dfx present but no loadable Glide; no AI GPU → CPU only).
3. **Startup readiness banner** — `agent/src/ai.c:ai_status_thread` runs a
   few seconds after boot, spawns/probes the engine, and prints to the agent
   console + `agent.log`:
   `AI: READY for fleet AI requests` / `AI: engine <caps>` /
   `AI: host <gpu + driver_flag>`. So the box itself shows, on its own
   screen, when it can take AI work. Verified in `agent.log`:
   `AI: engine ready … "driver_flag":"ok: 3dfx GPU with loadable glide3x …"`.
4. **Fleet view** — `mcp__retro__ai_list` (chat) and the live console's `d`
   discover action (`scripts/retro_infer_console.py`, see
   [`retro-infer/docs/OPERATIONS.md`](../../retro-infer/docs/OPERATIONS.md))
   surface the same data fleet-wide; the UDP beacon carries an `ai=1` flag so
   AI-capable boxes are findable without a full handshake.

## Re-running the inspection

```bash
# one box, full capability + driver flag:
python3 - <<'PY'
import asyncio, json, sys; sys.path.insert(0,'.')
from client.retro_protocol import RetroConnection
async def go(ip):
    c=RetroConnection(ip,9898); await c.connect('retro-agent-secret')
    print(json.dumps(json.loads(await c.command_text('AI_HELLO',timeout=45)),indent=2))
    await c.close()
asyncio.run(go('192.168.1.143'))
PY

# GPU backend acceptance on a box:
#   EXECW 600 C:\RETRO_AGENT\retro-infer.exe --glide-check 256 256 256 42
# CPU kernel report:
#   EXECW 120 C:\RETRO_AGENT\retro-infer.exe --selfcheck
```
