# Fleet AI model zoo — what's built, where it lives, what it scores

Every model actually trained and shipped, with its training script, `.rim`
artifact, dataset, and the achieved acceptance numbers (from the parity
reports in `tools/rim/*/out/`, the fleet-run commit messages, and
[`docs/machines/ai-capability-profiles.md`](../../docs/machines/ai-capability-profiles.md)).
Numbers not in those sources live in the `ai_runs` DB
([`scripts/ai_metrics.py`](../../scripts/ai_metrics.py)).

## The .rim container (summary)

Binding spec: [`tools/rim/FORMAT.md`](../../tools/rim/FORMAT.md) — change it
there first, then keep [`src/rim.c`](../src/rim.c)/[`src/exec.c`](../src/exec.c)
and [`tools/rim/rim_pack.py`](../../tools/rim/rim_pack.py) in sync.

```
RIM1 | flags (bit0 = LE) | manifest_len | manifest JSON | pad to 16 | weights blob
```

- Tensor refs `{"off","dtype","shape"[,"scale"]}`; offsets are blob-relative,
  every tensor start 16-byte aligned ([`src/rim.c:4-11`](../src/rim.c#L4)).
- dtypes: `f32 | i8 | i32 | u8 | bin` (bin = packed BNN bits, spec extension
  in [`tools/rim/bnn/BNN-SPEC.md`](../../tools/rim/bnn/BNN-SPEC.md#bdense-manifest-op-formatmd-extension)).
- Ops: `conv2d, dense, relu, maxpool, flatten, softmax, knn, bdense`
  (executor's op list at [`src/exec.c:17-20`](../src/exec.c#L17)).
- Writers: Python [`tools/rim/rim_pack.py`](../../tools/rim/rim_pack.py)
  (`tref` + `write_rim`); C [`src/rim_save.c:25`](../src/rim_save.c#L25)
  `rim_save_dense` (on-device-trained MLPs). Inspector/verifier:
  [`tools/rim/rim_dump.py`](../../tools/rim/rim_dump.py).

## Zoo

| model | .rim artifact | trainer | dataset | key numbers |
|---|---|---|---|---|
| LeNet-5 f32 | `tools/rim/out/lenet5-mnist-f32.rim` | [`train_lenet.py`](../../tools/rim/train_lenet.py) (numpy) | MNIST | **97.97%** top-1 / 10k |
| LeNet-5 int8 | `tools/rim/out/lenet5-mnist-int8.rim` | + [`quantize.py`](../../tools/rim/quantize.py), [`export_models.py`](../../tools/rim/export_models.py) | MNIST | **97.94%** / 10k (Δ −0.03%); logits **bit-exact** C↔numpy |
| logistic regression | `tools/rim/out/logreg-mnist-f32.rim` | [`train_lenet.py`](../../tools/rim/train_lenet.py) | MNIST | **91.96%** / 10k; 91.3% / 1k parity set |
| kNN (k=3) | `tools/rim/out/knn-mnist.rim` | [`export_models.py`](../../tools/rim/export_models.py) (2000 refs) | MNIST | **88.0%** / 1k; labels identical C↔ref |
| MLP 784-128-10 | `tools/rim/out/mlp_60k.rim` (+ any `--train-mlp` output) | on-device [`src/train/train_nn.c`](../src/train/train_nn.c) | MNIST 60k | **96.04%** on .124 (≥96% target met on both boxes) |
| GBDT classify | metrics-only (no .rim yet) | on-device [`src/train/gbdt.c`](../src/train/gbdt.c) | adult (binned u8) | val AUC **0.9216** (sklearn ref 0.9205) |
| GBDT regress | metrics-only | same, `--regress` | california housing | RMSE within ~1.5% of sklearn ref 0.4445 |
| random forest | metrics-only | on-device [`src/train/forest.c`](../src/train/forest.c) | adult | sklearn ref acc 0.8495/F1 0.6795; on-device runs → see ai_runs DB |
| linear SVM | metrics-only | on-device [`src/train/svm.c`](../src/train/svm.c) | adult | sklearn ref acc 0.8104; on-device runs → see ai_runs DB |
| BNN XNOR 3072-1024-1024-10 | `tools/rim/bnn/out/bnn-cifar10.rim` | [`bnn/train_bnn.py`](../../tools/rim/bnn/train_bnn.py) | CIFAR-10 | **56.87%** / 10k integer; GPU = CPU **1000/1000** labels |

Details per model below.

### LeNet-5 (f32 + int8) — the M1/M3 parity flagship

- Train: [`tools/rim/train_lenet.py`](../../tools/rim/train_lenet.py)
  (pure-numpy SGD+momentum, im2col; arch in its header) → `out/lenet5_f32.npz`.
- Quantize + pack: [`tools/rim/quantize.py`](../../tools/rim/quantize.py)
  (symmetric int8, 2000-image calibration) via
  [`tools/rim/export_models.py`](../../tools/rim/export_models.py). The last
  dense sets `act_scale_out: 0` → fp32 logits.
- Reference outputs: [`tools/rim/gen_eval.py`](../../tools/rim/gen_eval.py) →
  `out/mnist_test_1000.{images,labels}.bin`,
  `out/ref_logits_lenet5_{f32,int8}.bin`, `out/ref_report.json`.
- Numbers (`tools/rim/out/ref_report.json` + commit `d66eaf7`): 97.97% f32 /
  97.94% int8 on the 10k test set; on the 1k parity set both 97.0%, int8↔f32
  prediction agreement 99.8%. **Fleet acceptance**: int8 logits bit-exact vs
  numpy on both boxes; throughput .124 P3/SSE f32 289 img/s (scalar 189),
  int8-MMX 241; .143 Athlon/3DNow! f32 370, int8-MMX 274.

### Logistic regression + kNN

- Both packed by [`tools/rim/export_models.py`](../../tools/rim/export_models.py);
  kNN is a single `knn` op (k=3, 2000 train vectors, integer squared-L2,
  executor at [`src/exec.c:277`](../src/exec.c#L277)).
- `ref_report.json`: logreg 91.3% / kNN 88.0% on the 1k set (logreg 91.96% on
  10k per [`tools/rim/README.md`](../../tools/rim/README.md#results-2026-07-17-build-seed-42));
  labels identical to the Python reference on the fleet (M1 acceptance).

### MLP (on-device trained)

- `retro-infer --train-mlp` ([`src/main.c:285`](../src/main.c#L285) →
  [`src/train/train_nn.c:115`](../src/train/train_nn.c#L115)); exports
  dense-f32 `.rim` via [`src/rim_save.c:25`](../src/rim_save.c#L25). Arch
  `"784,10"` doubles as the logistic-regression trainer (M2: ≥90% target,
  91.3% achieved on host).
- 60k-sample run on .124: **96.04%** (commit `c4556e4`; ≥96% M2 target).
  `tools/rim/out/mlp_60k.rim` is the artifact the pipeline demo splits.
- Fleet-trained variant: `NTEXPORT` writes the same format from a
  data-parallel session ([`src/train/nn_session.c:293`](../src/train/nn_session.c#L293)).

### GBDT (adult classify + calif regress)

- Datasets: [`tools/rim/tabular/prep_adult.py`](../../tools/rim/tabular/prep_adult.py)
  / [`prep_calif.py`](../../tools/rim/tabular/prep_calif.py) — u8
  quantile-binned features, seed-42 permuted, sequential 80/20 split
  ([`tools/rim/tabular/README.md`](../../tools/rim/tabular/README.md)).
- Reference: [`tools/rim/tabular/ref_gbdt.py`](../../tools/rim/tabular/ref_gbdt.py)
  trains sklearn on the **same bytes and split** →
  `out/ref_tabular.json` (adult AUC 0.92047, calif RMSE 0.44445).
- C trainer `--train-gbdt` ([`src/train/gbdt.c:34`](../src/train/gbdt.c#L34)):
  adult val AUC **0.9216** (beats the 0.02-band M2 target vs 0.9205), calif
  RMSE within ~1.5% of ref (commit `e61b854`, roadmap status table).
- Distributed variant: 2-node run val AUC 0.9167 @50 rounds, Δ 0.005 vs
  single-node — within the 0.01 M7 band (commit `8a822ee`).

### Random forest + linear SVM

- `--train-rf` ([`src/train/forest.c:15`](../src/train/forest.c#L15)):
  bootstrap gini trees, OOB error, val acc/F1/precision/recall/AUC printed as
  `forest.*` key=values. `--train-svm`
  ([`src/train/svm.c:13`](../src/train/svm.c#L13)): Pegasos hinge SGD.
- sklearn baselines in `tools/rim/tabular/out/ref_tabular.json` (RF acc
  0.84953 / F1 0.67954; LinearSVC acc 0.81039 — the ordinal-encoded byte
  format makes the low SVM recall the honest linear baseline, see
  [`tools/rim/tabular/README.md`](../../tools/rim/tabular/README.md#reference-results-outref_tabularjson)).
- On-device result rows: see the ai_runs DB (no numbers committed to the repo
  beyond "RF, SVM ✅" in the roadmap status table).

### BNN (XNOR CIFAR-10) — the M5 GPU showcase

- Train: [`tools/rim/bnn/train_bnn.py`](../../tools/rim/bnn/train_bnn.py)
  (BinaryConnect STE, BN with gamma≡1 so BN+sign folds to integer
  thresholds; details in its header) → `out/bnn-cifar10.rim` +
  `bnn_train_report.json`.
- Integer reference: [`tools/rim/bnn/eval_bnn_ref.py`](../../tools/rim/bnn/eval_bnn_ref.py)
  → `bnn_ref_labels_1000.bin` (byte-for-byte conformance target) +
  `bnn_report.json`.
- Numbers (`bnn_report.json` + commit `c4556e4`): integer accuracy **56.87%**
  on the full 10k (57.3% on the 1k subset), float-shadow agreement 10000/10000.
  On the real Voodoo5 (.143): GPU labels = CPU labels **1000/1000**, 15.9
  img/s GPU vs 20.4 CPU; Voodoo3 (.124): 1000/1000 at 13.7 img/s. Honest
  comparator: GPU exact 61 MMAC/s vs CPU bit-packed XNOR 695 MMAC/s.
- Format checker for the `bin` dtype:
  [`tools/rim/bnn/check_rim.py`](../../tools/rim/bnn/check_rim.py).

### Debug fixtures (not models, but shipped)

`tools/rim/out/mini_dense_f32.rim` and `mini_conv_int8.rim` with matching
`.input.bin`/`.expected.bin` — single-op models for isolating C-executor bugs
(generated by [`tools/rim/gen_eval.py`](../../tools/rim/gen_eval.py); usage in
[TRAINING-AND-INFERENCE.md](TRAINING-AND-INFERENCE.md#local-parity-tests)).

## Still open (from the roadmap zoo)

Char-transformer pipeline flagship (needs attention ops), RNN/GRU/LSTM,
VAE/keyword-spotting, int8-via-bitplanes on Glide, the GeForce hardware pass —
see [`docs/roadmap-fleet-ai.md`](../../docs/roadmap-fleet-ai.md#implementation-status-2026-07-17).
