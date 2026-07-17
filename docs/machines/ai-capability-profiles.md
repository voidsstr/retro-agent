# Fleet AI capability profiles — per-machine hardware, kernels, drivers

Generated from live inspection of the fleet on 2026-07-17 (`AI_HELLO` +
`VIDEODIAG` + `retro-infer --selfcheck` + `--glide-check`). This is the
authoritative record of what each box is, which AI code path it uses, and
any driver actions flagged. The agent re-derives all of this at runtime — see
["How the agent detects and reports capability"](#how-the-agent-detects-and-reports-capability)
— this doc is the human-readable snapshot + rationale.

## Summary

| Box | CPU | ISA used | GPU | Glide AI backend | f32 kernel | int8 | GEMM-NT/TN (training) |
|---|---|---|---|---|---|---|---|
| **.124** | Pentium III 850MHz (fam6/m8) | **SSE** + MMX | Voodoo3 (PCI 121a:0005) | ✅ glide-mac | sse | mmx | sse |
| **.143** | Athlon 1GHz classic (fam6/m2) | **3DNow!** + MMX (no SSE) | Voodoo5 5500 (PCI 121a:0009) | ✅ glide-mac | 3dnow | mmx | 3dnow |

Both boxes: Windows XP SP3, agent **v1.10.0**, `ready=1` for AI requests,
`retro-infer` engine staged next to the agent, `glide3x.dll` loadable.

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
   mismatches (3dfx present but no loadable Glide; NVIDIA present but M6 not
   hardware-validated; no AI GPU → CPU only).
3. **Startup readiness banner** — `agent/src/ai.c:ai_status_thread` runs a
   few seconds after boot, spawns/probes the engine, and prints to the agent
   console + `agent.log`:
   `AI: READY for fleet AI requests` / `AI: engine <caps>` /
   `AI: host <gpu + driver_flag>`. So the box itself shows, on its own
   screen, when it can take AI work. Verified in `agent.log`:
   `AI: engine ready … "driver_flag":"ok: 3dfx GPU with loadable glide3x …"`.
4. **Fleet view** — `mcp__retro__ai_list` (chat) and
   `scripts/retro_infer_console.py` [d]iscover surface the same data
   fleet-wide; the UDP beacon carries an `ai=1` flag so AI-capable boxes are
   findable without a full handshake.

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
