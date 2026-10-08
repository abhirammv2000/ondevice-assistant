"""Measure a trained model on CLINC150: accuracy, how well confidence separates requests the device can handle from
requests it cannot, and calibration.

    python tools/evaluate.py models/clinc150.pmodel --float models/clinc150_float.pmodel

The headline number for a device is not accuracy alone. A request the model gets wrong with high confidence goes
to the wrong handler. A request it is unsure about can be sent to a larger model instead. So the report shows, for a
range of confidence thresholds, how many in-scope requests are still answered on the device and how many
out-of-scope requests are caught.
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import clinc  # noqa: E402
import model_format  # noqa: E402
from train import Encoded, encode  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def batch_probs(model: model_format.Model, data: Encoded, chunk: int = 512) -> np.ndarray:
    out = []
    for lo in range(0, len(data.feats), chunk):
        parts = data.feats[lo:lo + chunk]
        lens = np.array([len(p) for p in parts])
        starts = np.concatenate([[0], np.cumsum(lens)[:-1]])
        cols = np.concatenate(parts)
        inv = (1.0 / np.sqrt(data.counts[lo:lo + chunk].astype(np.float32))).astype(np.float32)
        if model.scales is not None:
            acc = np.add.reduceat(model.weights[cols].astype(np.int32), starts, axis=0).astype(np.float32)
            z = model.bias + model.scales * acc * inv[:, None]
        else:
            z = model.bias + np.add.reduceat(model.weights[cols], starts, axis=0) * inv[:, None]
        z = z - z.max(axis=1, keepdims=True)
        e = np.exp(z)
        out.append((e / e.sum(axis=1, keepdims=True)).astype(np.float32))
    return np.concatenate(out)


def _average_ranks(values: np.ndarray) -> np.ndarray:
    """Ranks from 1, with tied values sharing the mean of the ranks they span."""
    order = np.argsort(values, kind="mergesort")
    sorted_values = values[order]
    ranks = np.empty(len(values), dtype=np.float64)
    i = 0
    while i < len(values):
        j = i
        while j + 1 < len(values) and sorted_values[j + 1] == sorted_values[i]:
            j += 1
        ranks[order[i:j + 1]] = (i + j) / 2 + 1
        i = j + 1
    return ranks


def auroc(positive: np.ndarray, negative: np.ndarray) -> float:
    """Probability that a random positive scores above a random negative (ties count half)."""
    ranks = _average_ranks(np.concatenate([positive, negative]))
    u = ranks[: len(positive)].sum() - len(positive) * (len(positive) + 1) / 2
    return float(u / (len(positive) * len(negative)))


def ece(confidence: np.ndarray, correct: np.ndarray, bins: int = 10) -> float:
    """Expected calibration error: how far stated confidence is from how often the answer is right."""
    edges = np.linspace(0.0, 1.0, bins + 1)
    total = 0.0
    for lo, hi in zip(edges[:-1], edges[1:]):
        inside = (confidence > lo) & (confidence <= hi)
        if inside.any():
            total += inside.mean() * abs(correct[inside].mean() - confidence[inside].mean())
    return float(total)


def threshold_table(p_in: np.ndarray, y_in: np.ndarray, p_oos: np.ndarray, taus: list[float]) -> list[dict]:
    conf_in, pred_in = p_in.max(axis=1), p_in.argmax(axis=1)
    conf_oos = p_oos.max(axis=1)
    rows = []
    for tau in taus:
        accepted = conf_in >= tau
        rows.append({
            "threshold": tau,
            "answered_on_device": float(accepted.mean()),                    # of in-scope requests
            "accuracy_when_answered": float((pred_in[accepted] == y_in[accepted]).mean()) if accepted.any() else float("nan"),
            "out_of_scope_caught": float((conf_oos < tau).mean()),           # sent on instead of answered wrongly
            "out_of_scope_answered_wrongly": float((conf_oos >= tau).mean()),
        })
    return rows


def evaluate(model: model_format.Model, data: clinc.Clinc) -> dict:
    n_features = model.n_features
    test = encode(data.test.texts, data.test.labels, data.intents, n_features)
    oos = encode(data.oos_test.texts, None, data.intents, n_features)
    p_in, p_oos = batch_probs(model, test), batch_probs(model, oos)
    pred = p_in.argmax(axis=1)
    correct = pred == test.labels
    conf = p_in.max(axis=1)
    return {
        "n_features": n_features,
        "in_scope_accuracy": float(correct.mean()),
        "n_in_scope": int(len(correct)),
        "n_out_of_scope": int(len(p_oos)),
        "auroc_out_of_scope": auroc(conf, p_oos.max(axis=1)),
        "ece": ece(conf, correct),
        "thresholds": threshold_table(p_in, test.labels, p_oos, [0.0, 0.2, 0.3, 0.4, 0.5, 0.6, 0.7, 0.8, 0.9]),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("model", type=Path)
    parser.add_argument("--float", type=Path, default=None, help="the unquantised model, to measure what int8 costs")
    parser.add_argument("--out", type=Path, default=None)
    args = parser.parse_args()

    data = clinc.load()
    report = {"int8": evaluate(model_format.read(args.model), data), "int8_bytes": args.model.stat().st_size}
    if args.float:
        report["float32"] = evaluate(model_format.read(args.float), data)
        report["float32_bytes"] = args.float.stat().st_size
    for kind in ("int8", "float32"):
        if kind not in report:
            continue
        r = report[kind]
        print(f"\n{kind}: in-scope accuracy {r['in_scope_accuracy']:.4f} on {r['n_in_scope']}, "
              f"out-of-scope AUROC {r['auroc_out_of_scope']:.4f}, ECE {r['ece']:.4f}")
        print(f"  {'threshold':>9} {'answered':>9} {'accuracy':>9} {'oos caught':>11}")
        for row in r["thresholds"]:
            print(f"  {row['threshold']:9.2f} {row['answered_on_device']:9.1%} {row['accuracy_when_answered']:9.1%} {row['out_of_scope_caught']:11.1%}")
    if args.out:
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()
