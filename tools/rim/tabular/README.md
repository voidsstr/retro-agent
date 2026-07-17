# RIM tabular datasets + reference models (Fleet AI M2)

Device-ready binary tabular datasets and sklearn reference-model metrics that
the C GBDT / random-forest / SVM trainer is compared against. Everything is
deterministic (seed 42): reruns produce byte-identical `.bin` files.

## Regenerate

```bash
cd tools/rim/tabular
python3 prep_adult.py   # downloads UCI adult.data/adult.test into cache/
python3 prep_calif.py   # sklearn fetch_california_housing (~/scikit_learn_data)
python3 ref_gbdt.py     # trains reference models -> out/ref_tabular.json
```

Requires numpy + scikit-learn (`pip install --user --break-system-packages
scikit-learn` on PEP 668 systems). Network is only needed on first run; source
files are cached.

## File formats (all in `out/`)

| File | Format |
|---|---|
| `adult.features.bin` | u8, row-major `[N, F]` = `[45222, 14]` (633,108 bytes) |
| `adult.labels.bin` | u8 `[N]`, 1 = income >50K, 0 = <=50K |
| `adult.meta.json` | N, F, column names, per-column bin info, class balance, split rule |
| `calif.features.bin` | u8, row-major `[N, F]` = `[20640, 8]` (165,120 bytes) |
| `calif.targets.bin` | f32 little-endian `[N]`, median house value in $100k |
| `calif.meta.json` | N, F, columns, bin info, target mean/std, split rule |
| `ref_tabular.json` | all reference metrics + exact hyperparams |

## Preprocessing (identical for the C side and the reference models)

Every feature is exactly **one byte** (u8 bin index, always <= 254):

- **Numeric columns** — quantile-binned into <= 255 bins: edges are
  `np.quantile(col, linspace(0, 1, 256))` with duplicate edges collapsed
  (`np.unique`); the bin index is
  `searchsorted(interior_edges, v, side="right")`. Skewed columns (e.g. adult
  `capital_gain`) end up with far fewer than 255 bins.
- **Categorical columns** (adult only) — integer codes in **alphabetical**
  category order (categories listed in the meta JSON).
- Adult rows containing `?` in any field are dropped **before** anything else
  (3,620 rows across both source files).

## Train/val split rule

The rows in every `.bin` are already shuffled with
`numpy.random.default_rng(42).permutation(N)`. The consumer takes them
**sequentially**:

- train = first `floor(0.8 * N)` rows
- val   = remaining rows

(adult: 36,177 / 9,045 — calif: 16,512 / 4,128). No further shuffling is
needed or wanted on the C side; using the same split is what makes the
reference metrics comparable.

## Reference results (out/ref_tabular.json)

Reference models are trained on the **same u8 bytes and same split** as the C
trainer, so metric gaps isolate the training algorithm, not preprocessing.

| Model | Dataset | Val metrics |
|---|---|---|
| HistGradientBoostingClassifier (200 iters, lr 0.1, 31 leaves, 255 bins) | adult | AUC 0.9205, logloss 0.2988 |
| HistGradientBoostingRegressor (same hyperparams) | calif | RMSE 0.4445, RMSE/std 0.3831 |
| RandomForestClassifier (200 trees, unlimited depth) | adult | acc 0.8495, F1 0.6795 |
| LinearSVC (C=1, squared hinge) on features/255 | adult | acc 0.8104, prec 0.7877, rec 0.3495, F1 0.4842 |

Note on the linear SVM: categorical codes are ordinal-encoded (one byte per
feature by design), which a linear model cannot exploit well — the low recall
is expected and is the honest linear baseline for this exact byte format.

`binning.py` holds the shared binning/split helpers used by both prep scripts.
