#!/usr/bin/env python3
"""Reference sklearn models on the SAME binned u8 features and SAME splits.

Trains on exactly the bytes the C trainer will see (out/*.features.bin) with
the identical sequential 80/20 split, so any metric gap between the C
implementation and these numbers isolates the training algorithm, not the
preprocessing.

Models:
  adult  - HistGradientBoostingClassifier (val AUC, val logloss)
  calif  - HistGradientBoostingRegressor  (val RMSE, RMSE/std(target))
  adult  - RandomForestClassifier         (val accuracy, F1)
  adult  - LinearSVC on features/255      (val accuracy, precision, recall, F1)

All metrics + exact hyperparams -> out/ref_tabular.json.
"""
import json
import os
import time

import numpy as np
from sklearn.ensemble import (HistGradientBoostingClassifier,
                              HistGradientBoostingRegressor,
                              RandomForestClassifier)
from sklearn.metrics import (accuracy_score, f1_score, log_loss,
                             precision_score, recall_score, roc_auc_score)
from sklearn.svm import LinearSVC

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "out")
SEED = 42


def load_dataset(prefix, target_kind):
    with open(os.path.join(OUT, f"{prefix}.meta.json")) as fh:
        meta = json.load(fh)
    n, f = meta["N"], meta["F"]
    x = np.fromfile(os.path.join(OUT, f"{prefix}.features.bin"),
                    dtype=np.uint8).reshape(n, f)
    if target_kind == "u8":
        y = np.fromfile(os.path.join(OUT, f"{prefix}.labels.bin"), dtype=np.uint8)
    else:
        y = np.fromfile(os.path.join(OUT, f"{prefix}.targets.bin"), dtype="<f4")
    assert len(y) == n
    n_train = meta["split"]["n_train"]
    return (x[:n_train], y[:n_train], x[n_train:], y[n_train:], meta)


def timed_fit(model, x, y):
    t0 = time.time()
    model.fit(x, y)
    return time.time() - t0


def main():
    results = {"seed": SEED, "note": "all models trained on the u8-binned "
               "features from out/*.features.bin with the sequential 80/20 "
               "split described in the meta files"}

    # ---- adult: gradient-boosted trees ---------------------------------
    xtr, ytr, xva, yva, ameta = load_dataset("adult", "u8")
    hp = dict(max_iter=200, learning_rate=0.1, max_leaf_nodes=31,
              max_bins=255, early_stopping=False, random_state=SEED)
    gbdt = HistGradientBoostingClassifier(**hp)
    fit_s = timed_fit(gbdt, xtr.astype(np.float32), ytr)
    proba = gbdt.predict_proba(xva.astype(np.float32))[:, 1]
    auc = roc_auc_score(yva, proba)
    ll = log_loss(yva, proba)
    results["adult_gbdt"] = {
        "model": "sklearn.ensemble.HistGradientBoostingClassifier",
        "hyperparams": hp, "fit_seconds": round(fit_s, 2),
        "val_auc": round(float(auc), 5), "val_logloss": round(float(ll), 5),
    }
    print(f"adult GBDT     : val AUC {auc:.5f}  logloss {ll:.5f}  ({fit_s:.1f}s)")

    # ---- calif: gradient-boosted regression ----------------------------
    cxtr, cytr, cxva, cyva, cmeta = load_dataset("calif", "f32")
    rhp = dict(max_iter=200, learning_rate=0.1, max_leaf_nodes=31,
               max_bins=255, early_stopping=False, random_state=SEED)
    gbr = HistGradientBoostingRegressor(**rhp)
    fit_s = timed_fit(gbr, cxtr.astype(np.float32), cytr)
    pred = gbr.predict(cxva.astype(np.float32))
    rmse = float(np.sqrt(np.mean((pred - cyva) ** 2)))
    tstd = float(np.std(cyva))
    results["calif_gbdt"] = {
        "model": "sklearn.ensemble.HistGradientBoostingRegressor",
        "hyperparams": rhp, "fit_seconds": round(fit_s, 2),
        "val_rmse": round(rmse, 5),
        "val_target_std": round(tstd, 5),
        "val_rmse_over_std": round(rmse / tstd, 5),
    }
    print(f"calif GBDT     : val RMSE {rmse:.5f}  RMSE/std {rmse / tstd:.5f}  "
          f"({fit_s:.1f}s)")

    # ---- adult: random forest ------------------------------------------
    fhp = dict(n_estimators=200, max_depth=None, min_samples_leaf=1,
               n_jobs=-1, random_state=SEED)
    rf = RandomForestClassifier(**fhp)
    fit_s = timed_fit(rf, xtr, ytr)
    pred = rf.predict(xva)
    acc = accuracy_score(yva, pred)
    f1 = f1_score(yva, pred)
    results["adult_random_forest"] = {
        "model": "sklearn.ensemble.RandomForestClassifier",
        "hyperparams": {k: v for k, v in fhp.items() if k != "n_jobs"},
        "fit_seconds": round(fit_s, 2),
        "val_accuracy": round(float(acc), 5), "val_f1": round(float(f1), 5),
    }
    print(f"adult RF       : val acc {acc:.5f}  F1 {f1:.5f}  ({fit_s:.1f}s)")

    # ---- adult: linear SVM on features/255 -----------------------------
    shp = dict(C=1.0, loss="squared_hinge", max_iter=20000, tol=1e-4,
               random_state=SEED)
    svm = LinearSVC(**shp)
    fit_s = timed_fit(svm, xtr.astype(np.float32) / 255.0, ytr)
    pred = svm.predict(xva.astype(np.float32) / 255.0)
    results["adult_linear_svm"] = {
        "model": "sklearn.svm.LinearSVC",
        "feature_scaling": "u8 bins / 255.0",
        "hyperparams": shp, "fit_seconds": round(fit_s, 2),
        "val_accuracy": round(float(accuracy_score(yva, pred)), 5),
        "val_precision": round(float(precision_score(yva, pred)), 5),
        "val_recall": round(float(recall_score(yva, pred)), 5),
        "val_f1": round(float(f1_score(yva, pred)), 5),
    }
    r = results["adult_linear_svm"]
    print(f"adult LinearSVC: val acc {r['val_accuracy']:.5f}  "
          f"prec {r['val_precision']:.5f}  rec {r['val_recall']:.5f}  "
          f"F1 {r['val_f1']:.5f}  ({fit_s:.1f}s)")

    results["datasets"] = {
        "adult": {"N": ameta["N"], "F": ameta["F"],
                  "n_train": ameta["split"]["n_train"],
                  "n_val": ameta["split"]["n_val"]},
        "calif": {"N": cmeta["N"], "F": cmeta["F"],
                  "n_train": cmeta["split"]["n_train"],
                  "n_val": cmeta["split"]["n_val"]},
    }
    with open(os.path.join(OUT, "ref_tabular.json"), "w") as fh:
        json.dump(results, fh, indent=2)
    print("wrote out/ref_tabular.json")


if __name__ == "__main__":
    main()
