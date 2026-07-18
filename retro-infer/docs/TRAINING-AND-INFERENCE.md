# Fleet AI runbook — build, train, infer, verify

Copy-pasteable commands for every workflow, dev box → fleet. Background:
[ARCHITECTURE.md](ARCHITECTURE.md); math: [ALGORITHMS.md](ALGORITHMS.md);
artifacts: [MODELS.md](MODELS.md); operational gotchas:
[MAINTENANCE.md](MAINTENANCE.md).

## Build

```bash
cd retro-infer
make          # retro-infer.exe — i586 Windows (MinGW i686-w64-mingw32-gcc),
              # SSE/MMX/3DNow! TUs behind runtime CPUID dispatch
make host     # retro-infer-host — native Linux, for fast local parity
              # iteration (no 3DNow! on x86_64; builds with -DRI_NO_3DNOW)
make release  # bump infer-vX.Y.Z tag + clean build (BUMP=minor|major)
```

Version comes from the highest `infer-v*` git tag
([`Makefile:15`](../Makefile#L15)); host targets at
[`Makefile:74-79`](../Makefile#L74). Sanity: `./retro-infer-host --selfcheck`
prints ISA, RAM, kernels, and a GFLOP/s microbench
([`src/main.c:27`](../src/main.c#L27)).

## Fetch datasets + export the .rim zoo

All Python needs numpy only (tabular refs additionally need scikit-learn).

```bash
cd tools/rim
python3 fetch_mnist.py       # MNIST -> data/ (idempotent)
python3 train_lenet.py       # LeNet-5 + logreg -> out/*.npz
python3 export_models.py     # 4 .rim models -> out/
python3 gen_eval.py          # eval bins, ref logits, ref_report.json, mini models

cd tabular
python3 prep_adult.py        # UCI adult -> out/adult.{features,labels}.bin
python3 prep_calif.py        # california housing -> out/calif.*
python3 ref_gbdt.py          # sklearn baselines -> out/ref_tabular.json

cd ../bnn
python3 fetch_cifar.py       # CIFAR-10 binary -> data/
python3 train_bnn.py         # BNN training (fp32 shadow) -> out/bnn-cifar10.rim
python3 eval_bnn_ref.py      # integer reference -> out/bnn_ref_labels_1000.bin
python3 check_rim.py         # container verification incl. the 'bin' dtype
```

Full pipeline docs: [`tools/rim/README.md`](../../tools/rim/README.md),
[`tools/rim/tabular/README.md`](../../tools/rim/tabular/README.md),
[`tools/rim/bnn/BNN-SPEC.md`](../../tools/rim/bnn/BNN-SPEC.md).

## Local parity tests

The C engine must match the numpy reference
([`tools/rim/eval_ref.py`](../../tools/rim/eval_ref.py)) — **bit-exact** on
integer paths, tolerance on f32 (rules:
[`../README.md`](../README.md#rules-that-keep-parity-exact)).

```bash
cd retro-infer && make host

# container sanity
./retro-infer-host --riminfo ../tools/rim/out/lenet5-mnist-int8.rim

# single-op fixtures (fastest way to localize an executor bug)
./retro-infer-host --infer ../tools/rim/out/mini_dense_f32.rim \
    ../tools/rim/out/mini_dense.input.bin
./retro-infer-host --infer ../tools/rim/out/mini_conv_int8.rim \
    ../tools/rim/out/mini_conv.input.bin

# full logit parity: engine dump vs reference dump
./retro-infer-host --logits /tmp/c_int8.bin --eval \
    ../tools/rim/out/lenet5-mnist-int8.rim \
    ../tools/rim/out/mnist_test_1000.images.bin \
    ../tools/rim/out/mnist_test_1000.labels.bin 1000
cmp /tmp/c_int8.bin ../tools/rim/out/ref_logits_lenet5_int8.bin && echo BIT-EXACT

# vectorized-vs-scalar A/B on the same machine
./retro-infer-host --eval ../tools/rim/out/lenet5-mnist-f32.rim \
    ../tools/rim/out/mnist_test_1000.images.bin \
    ../tools/rim/out/mnist_test_1000.labels.bin 1000 --scalar

# BNN integer conformance (CPU path)
./retro-infer-host --bnn-eval ../tools/rim/bnn/out/bnn-cifar10.rim \
    ../tools/rim/bnn/out/cifar_test_1000.images.bin \
    ../tools/rim/bnn/out/cifar_test_1000.labels.bin 1000 cpu
```

`--eval` prints `eval.top1/eval.img_per_sec`; `--logits` writes
`N × n_classes` fp32 LE for the `cmp`
([`src/main.c:143`](../src/main.c#L143)).

## Deploy the engine to a fleet box

The agent spawns `retro-infer.exe --serve 9896` from **its own directory**
([`agent/src/ai.c:90`](../../agent/src/ai.c#L90)), so stage the exe next to
the agent (fleet convention: `C:\RETRO_AGENT\`). The engine has no
auto-update — push it over the agent link:

```python
import asyncio
from client.retro_protocol import RetroConnection

async def deploy(ip):
    c = RetroConnection(ip, 9898)
    await c.connect('retro-agent-secret', timeout=15.0)
    exe = open('retro-infer/retro-infer.exe', 'rb').read()
    await c.send_command(r'UPLOAD C:\RETRO_AGENT\retro-infer.exe.new',
                         binary_payload=exe)
    # can't overwrite a running exe: stop engine, swap, respawn
    await c.command_text('EXEC taskkill /f /im retro-infer.exe', timeout=30)
    await c.command_text(r'EXEC cmd /c move /Y C:\RETRO_AGENT\retro-infer.exe.new '
                         r'C:\RETRO_AGENT\retro-infer.exe', timeout=30)
    print(await c.command_text('AI_RESTART', timeout=60))
    await c.close()

asyncio.run(deploy('192.168.1.143'))
```

Verify: `AI_HELLO` should report `"ready":1` with the expected kernels
(`hello_json`, [`src/serve.c:171`](../src/serve.c#L171)), the discovery beacon
flips to `ai=1` ([`agent/src/protocol.c:222`](../../agent/src/protocol.c#L222)),
and the box prints `AI: READY for fleet AI requests` on its console at next
agent start ([`agent/src/ai.c:264`](../../agent/src/ai.c#L264)). On a Voodoo
box also stage the **matching** `glide3x.dll` next to the agent — see the
driver-flag rule in [MAINTENANCE.md](MAINTENANCE.md#fleet-gotchas).

## Remote inference

```python
import asyncio
from client.retro_protocol import RetroConnection
from client.retro_ai import RetroAI

async def run(ip):
    c = RetroConnection(ip, 9898)
    await c.connect('retro-agent-secret', timeout=15.0)
    ai = RetroAI(c)
    print(await ai.hello())                         # capability JSON
    await ai.model_load('lenet5-int8',
                        open('tools/rim/out/lenet5-mnist-int8.rim', 'rb').read())
    imgs = open('tools/rim/out/mnist_test_1000.images.bin', 'rb').read()
    logits = await ai.infer_run('lenet5-int8', imgs[:784])   # sample 0
    print('pred:', int(logits.argmax()))
    await c.close()                                 # ALWAYS graceful close

asyncio.run(run('192.168.1.143'))
```

From chat, the same three steps are `mcp__retro__ai_list` / `ai_load` /
`ai_infer` ([`scripts/retro_brain_tools.py:275-421`](../../scripts/retro_brain_tools.py#L275)).
Interactive TUI: `python3 scripts/retro_infer_console.py`
(`[d]iscover [t]rain [i]nfer [b]ench`,
[`scripts/retro_infer_console.py`](../../scripts/retro_infer_console.py)).

## On-device training

Stage the data files on the box first (UPLOAD or copy from the share), then
run the trainer under `EXECW` (bounded long-exec; `EXEC` caps at 60 s). Arg
orders are positional — see [`src/main.c:285-334`](../src/main.c#L285).

```text
# MLP: trX trY Ntr teX teY Nte arch epochs lr mom batch seed out.rim ('-' = don't save)
EXECW 900 C:\RETRO_AGENT\retro-infer.exe --train-mlp
  C:\AI\mnist_train.images.bin C:\AI\mnist_train.labels.bin 20000
  C:\AI\mnist_test.images.bin  C:\AI\mnist_test.labels.bin  2000
  784,128,10 3 0.1 0.9 64 42 C:\AI\mlp.rim

# GBDT classify: feat lab N F valfrac rounds depth minchild lr lambda
EXECW 900 C:\RETRO_AGENT\retro-infer.exe --train-gbdt
  C:\AI\adult.features.bin C:\AI\adult.labels.bin 45222 14 0.2 200 6 20 0.1 1.0

# GBDT regress (targets are f32 LE): add --regress
EXECW 900 C:\RETRO_AGENT\retro-infer.exe --train-gbdt
  C:\AI\calif.features.bin C:\AI\calif.targets.bin 20640 8 0.2 200 6 20 0.1 1.0 --regress

# Random forest: feat lab N F valfrac ntrees depth seed
EXECW 900 C:\RETRO_AGENT\retro-infer.exe --train-rf
  C:\AI\adult.features.bin C:\AI\adult.labels.bin 45222 14 0.2 100 12 42

# Linear SVM: feat lab N F valfrac epochs lr reg [seed]
EXECW 300 C:\RETRO_AGENT\retro-infer.exe --train-svm
  C:\AI\adult.features.bin C:\AI\adult.labels.bin 45222 14 0.2 10 0.01 0.0001 42
```

(One line each on the wire; wrapped here for readability.) All trainers print
`key=value` progress lines and are deterministic for a fixed seed. The same
data-file formats as `--eval` apply
([`tools/rim/FORMAT.md`](../../tools/rim/FORMAT.md#eval-file-formats-for---eval--parity-tests);
tabular formats in [`tools/rim/tabular/README.md`](../../tools/rim/tabular/README.md)).

## Fleet training

All three coordinators authenticate with `RETRO_AGENT_SECRET` (env, defaults
to the fleet secret) and talk to agents on :9898.

```bash
# data-parallel SGD (M7): identical seeds, brain-root allreduce
python3 scripts/retro_ai_fleet.py dp-train \
    --ips 192.168.1.124,192.168.1.143 \
    --arch 784,128,10 --epochs 2 --global-batch 128 --train-n 20000 \
    [--export C:\\RETRO_AGENT\\models\\dp.rim]

# single-node baseline for the accuracy comparison: same flags, one --ips entry
# failover acceptance: kill one engine mid-run
python3 scripts/retro_ai_fleet.py dp-train --ips A,B \
    --kill-node B --kill-at-step 100 ...

# distributed GBDT (rows sharded, histogram aggregation)
python3 scripts/retro_ai_gbdt.py --ips 192.168.1.124,192.168.1.143 \
    --rounds 100 --depth 6 --lr 0.1
# local smoke test (no fleet): --local, ips are ports of local --serve procs
python3 scripts/retro_ai_gbdt.py --ips 9896,9899 --local --rounds 50

# pipeline parallelism (stage 1 on A, stage 2 on B)
python3 scripts/retro_ai_pipeline.py --a 192.168.1.124 --b 192.168.1.143 --n 200
```

## GPU backend acceptance (Voodoo boxes)

Run via the agent with a generous `EXECW` bound; the Voodoo goes fullscreen
640×480 during compute ([`src/gpu/glide_mac.c:26-27`](../src/gpu/glide_mac.c#L26)),
so restore the desktop afterwards (`setmode`, and use the kill–wait–poll
discipline from [`benchmarks/README.md`](../../benchmarks/README.md) if a run
wedges).

```text
# exact-GEMM acceptance: mismatches must be 0, result_hash == rerun_hash
EXECW 600 C:\RETRO_AGENT\retro-infer.exe --glide-check 256 256 256 42

# model-level: BNN on-GPU must agree with CPU on all N labels
EXECW 900 C:\RETRO_AGENT\retro-infer.exe --bnn-eval
  C:\AI\bnn-cifar10.rim C:\AI\cifar_test_1000.images.bin
  C:\AI\cifar_test_1000.labels.bin 1000 glide
```

`--glide-check` prints `glide.mismatches`, `glide.max_abs_err_steps`,
`glide.result_hash`/`rerun_hash`, GPU-vs-CPU MMAC/s, and the honest bit-packed
CPU comparator ([`src/gpu/glide_check.c:45`](../src/gpu/glide_check.c#L45)).
`--bnn-eval … glide` prints `bnn.label_agreement=N/N EXACT`
([`src/bnn_eval.c:167`](../src/bnn_eval.c#L167)). A hung engine is recovered
with `AI_RESTART` ([`agent/src/ai.c:454`](../../agent/src/ai.c#L454)).

## Logging results to ai_runs

Every train/infer/bench result gets a row keyed model × machine × backend ×
precision ([`scripts/ai_metrics.py`](../../scripts/ai_metrics.py)). Set
`SPECPICKS_DATABASE_URL` in the environment (never commit a DSN).

```bash
export SPECPICKS_DATABASE_URL='postgresql://...'   # from your secret store
python3 scripts/ai_metrics.py init                 # one-time: create table
python3 scripts/ai_metrics.py log --ip 192.168.1.143 --model lenet5-mnist-int8 \
    --phase infer --backend cpu-mmx --precision i8 \
    --metrics '{"top1":0.97,"img_per_sec":274}' \
    --engine-ver 0.4.0 --dataset mnist-test-1000
python3 scripts/ai_metrics.py board --model lenet5-mnist-int8 --metric img_per_sec
```

The console's `[b]`ench logs automatically
([`scripts/retro_infer_console.py:160-166`](../../scripts/retro_infer_console.py#L160)).

## Milestone acceptance tests (M0–M8)

The definition of done per milestone, from
[`docs/roadmap-fleet-ai.md`](../../docs/roadmap-fleet-ai.md#milestones) — all
except M6 have passed on real hardware (status table there):

| M | acceptance test | how to re-run |
|---|---|---|
| M0 | `--selfcheck` reports the right ISA on a real P3 (SSE) and Athlon (3DNow!); binary runs on Win98/XP with no missing DLL | `EXECW 120 C:\RETRO_AGENT\retro-infer.exe --selfcheck` |
| M1 | int8 LeNet top-1 within 0.5% of f32, per-logit error bounded; SSE beats scalar; kNN/logreg labels identical to reference | `--eval` + `--logits` + `cmp` (parity section above; achieved: **bit-exact**) |
| M2 | logreg ≥90%, MLP ≥96% MNIST; GBDT AUC within 0.02 of sklearn; fixed seed reproduces bit-for-bit | on-device training section above, run twice, diff the output |
| M3 | `.rim` round-trip within quant bound; same model infers identically on multiple fleet machines | `rim_dump.py` + remote `INFER_RUN` on ≥2 boxes |
| M4 | ai_list shows capabilities; `MODEL_LOAD`/`MODEL_LIST`/remote `INFER_RUN` = local label; `TENSOR` round-trips intact; no Win98 RST | remote-inference section + `mcp__retro__ai_list` |
| M5 | Glide GEMM exact vs CPU (≤1 step; achieved 0), BNN GPU=CPU labels on 1000 images, stable hash across runs | GPU acceptance section above |
| M6 | nv-combiner int8 GEMM within tolerance on a GeForce 2; LeNet top-1 within 0.5% | blocked on hardware ([`src/gpu/nv_gl.c:12-15`](../src/gpu/nv_gl.c#L12)) |
| M7 | data-parallel = single-node within 1% (achieved bit-identical); failover completes; distributed GBDT within 0.01 AUC; pipeline label-identical | fleet-training section (incl. `--kill-node`) |
| M8 | every run rows into ai_runs with the full metric set; console works on 80×25; leaderboards stable | ai_runs section + `retro_infer_console.py` |
