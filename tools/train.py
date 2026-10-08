"""Train the intent classifier, quantize it and write the .pmodel the device loads.

    python tools/train.py --bits 15 --out models/clinc150.pmodel

The model is multinomial logistic regression over the hashed features in tools/features.py. Each active feature has
weight 1/sqrt(n), the same as the C++ runtime, so the logits are  bias + sum(W[f]) / sqrt(n). Training uses minibatch
Adam in float32 and numpy only, so the whole pipeline needs one dependency.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import clinc  # noqa: E402
import model_format  # noqa: E402
from features import features  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


@dataclass
class Encoded:
    feats: list[np.ndarray]      # one int64 array of masked feature rows per utterance
    counts: np.ndarray           # the true feature count n of each utterance, for the 1/sqrt(n) factor
    labels: np.ndarray           # class index, or -1 for out of scope


def encode(texts: list[str], labels: list[str] | None, intents: list[str], n_features: int) -> Encoded:
    index = {name: i for i, name in enumerate(intents)}
    mask = n_features - 1
    raw = [features(t) for t in texts]
    feats = [np.array(f, dtype=np.int64) & mask for f in raw]
    ys = np.array([index.get(l, -1) for l in labels] if labels is not None else [-1] * len(texts), dtype=np.int64)
    return Encoded(feats, np.array([len(f) for f in raw], dtype=np.int64), ys)


def _gather(data: Encoded, idx: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray]:
    parts = [data.feats[i] for i in idx]
    lens = np.array([len(p) for p in parts])
    starts = np.concatenate([[0], np.cumsum(lens)[:-1]])
    return np.concatenate(parts), starts, 1.0 / np.sqrt(data.counts[idx].astype(np.float32))


def logits(weights: np.ndarray, bias: np.ndarray, data: Encoded, idx: np.ndarray) -> np.ndarray:
    cols, starts, inv = _gather(data, idx)
    summed = np.add.reduceat(weights[cols], starts, axis=0)
    return bias + summed * inv[:, None]


def softmax(z: np.ndarray) -> np.ndarray:
    e = np.exp(z - z.max(axis=1, keepdims=True))
    return e / e.sum(axis=1, keepdims=True)


def predict_proba(weights: np.ndarray, bias: np.ndarray, data: Encoded, chunk: int = 512) -> np.ndarray:
    out = []
    for lo in range(0, len(data.feats), chunk):
        idx = np.arange(lo, min(lo + chunk, len(data.feats)))
        out.append(softmax(logits(weights, bias, data, idx)))
    return np.concatenate(out) if out else np.zeros((0, bias.shape[0]), dtype=np.float32)


def fit(train: Encoded, val: Encoded, n_features: int, n_classes: int, epochs: int = 12, batch: int = 128,
        lr: float = 0.03, l2: float = 1e-7, seed: int = 0, log=print) -> tuple[np.ndarray, np.ndarray, float]:
    rng = np.random.default_rng(seed)
    w = np.zeros((n_features, n_classes), dtype=np.float32)
    b = np.zeros(n_classes, dtype=np.float32)
    m_w, v_w = np.zeros_like(w), np.zeros_like(w)
    m_b, v_b = np.zeros_like(b), np.zeros_like(b)
    best = (-1.0, w.copy(), b.copy())
    step = 0
    n = len(train.feats)
    for epoch in range(1, epochs + 1):
        order = rng.permutation(n)
        for lo in range(0, n, batch):
            idx = order[lo:lo + batch]
            cols, starts, inv = _gather(train, idx)
            lens = np.diff(np.append(starts, len(cols)))
            p = softmax(np.add.reduceat(w[cols], starts, axis=0) * inv[:, None] + b)
            p[np.arange(len(idx)), train.labels[idx]] -= 1.0
            p /= len(idx)
            grad_w = np.zeros_like(w)
            np.add.at(grad_w, cols, np.repeat(p * inv[:, None], lens, axis=0))
            grad_w += l2 * w
            grad_b = p.sum(axis=0)
            step += 1
            c1, c2 = 1 - 0.9 ** step, 1 - 0.999 ** step
            for param, grad, m, v in ((w, grad_w, m_w, v_w), (b, grad_b, m_b, v_b)):
                m *= 0.9
                m += 0.1 * grad
                v *= 0.999
                v += 0.001 * grad * grad
                param -= lr * (m / c1) / (np.sqrt(v / c2) + 1e-8)
        acc = float((predict_proba(w, b, val).argmax(axis=1) == val.labels).mean())
        log(f"  epoch {epoch:2d}  validation accuracy {acc:.4f}")
        if acc > best[0]:
            best = (acc, w.copy(), b.copy())
    return best[1], best[2], best[0]


def calibrate(weights: np.ndarray, bias: np.ndarray, val: Encoded) -> float:
    """The temperature T that gives the lowest log loss on the validation set.

    Dividing every logit by T changes how confident the model sounds without changing which class wins, so accuracy
    stays the same and the confidence becomes something a threshold can be set on. T is folded into the weights and
    the bias before the file is written, so the device needs no extra step.
    """
    raw = np.vstack([logits(weights, bias, val, np.arange(lo, min(lo + 512, len(val.feats))))
                     for lo in range(0, len(val.feats), 512)])
    best_t, best_loss = 1.0, np.inf
    for t in np.arange(0.4, 3.01, 0.05):
        z = raw / t
        z = z - z.max(axis=1, keepdims=True)
        log_p = z - np.log(np.exp(z).sum(axis=1, keepdims=True))
        loss = -log_p[np.arange(len(val.labels)), val.labels].mean()
        if loss < best_loss:
            best_t, best_loss = float(t), float(loss)
    return best_t


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bits", type=int, default=15, help="log2 of the number of hashed features")
    parser.add_argument("--epochs", type=int, default=12)
    parser.add_argument("--out", type=Path, default=ROOT / "models" / "clinc150.pmodel")
    parser.add_argument("--float-out", type=Path, default=None, help="also write the unquantised float32 model here")
    args = parser.parse_args()

    n_features = 1 << args.bits
    data = clinc.load()
    train = encode(data.train.texts, data.train.labels, data.intents, n_features)
    val = encode(data.val.texts, data.val.labels, data.intents, n_features)
    print(f"training {len(data.intents)} intents on {len(data.train.texts)} utterances, {n_features} hashed features")
    started = time.time()
    w, b, best = fit(train, val, n_features, len(data.intents), epochs=args.epochs)
    print(f"best validation accuracy {best:.4f} in {time.time() - started:.0f}s")
    temperature = calibrate(w, b, val)
    w, b = w / temperature, b / temperature
    print(f"temperature {temperature:.2f} folded into the weights")

    q, scales = model_format.quantize(w)
    size = model_format.write(args.out, model_format.Model(list(data.intents), b, q, scales))
    print(f"wrote {args.out} ({size / 1e6:.2f} MB, int8)")
    if args.float_out:
        size = model_format.write(args.float_out, model_format.Model(list(data.intents), b, w, None))
        print(f"wrote {args.float_out} ({size / 1e6:.2f} MB, float32)")


if __name__ == "__main__":
    main()
