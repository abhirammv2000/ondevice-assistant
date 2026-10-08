"""How big does the model need to be? Train at several hash sizes and record accuracy against file size.

    python tools/sweep.py            # writes docs/results/size_sweep.json

Fewer hashed features means a smaller file but more collisions between different n-grams. This finds where the
curve flattens, so the size is chosen from a measurement and not a guess.
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import clinc  # noqa: E402
import evaluate  # noqa: E402
import model_format  # noqa: E402
from train import encode, fit  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--bits", type=int, nargs="+", default=[12, 13, 14, 15, 16])
    parser.add_argument("--epochs", type=int, default=10)
    args = parser.parse_args()

    data = clinc.load()
    rows = []
    for bits in args.bits:
        n_features = 1 << bits
        train = encode(data.train.texts, data.train.labels, data.intents, n_features)
        val = encode(data.val.texts, data.val.labels, data.intents, n_features)
        started = time.time()
        w, b, val_acc = fit(train, val, n_features, len(data.intents), epochs=args.epochs, log=lambda *_: None)
        q, scales = model_format.quantize(w)
        int8 = model_format.Model(list(data.intents), b, q, scales)
        float_model = model_format.Model(list(data.intents), b, w, None)
        row = {
            "bits": bits, "n_features": n_features, "train_seconds": round(time.time() - started),
            "validation_accuracy": val_acc,
            "int8_bytes": len(model_format.to_bytes(int8)), "float32_bytes": len(model_format.to_bytes(float_model)),
            "test_accuracy_int8": evaluate.evaluate(int8, data)["in_scope_accuracy"],
            "test_accuracy_float32": evaluate.evaluate(float_model, data)["in_scope_accuracy"],
        }
        rows.append(row)
        print(row, flush=True)
    out = ROOT / "docs" / "results"
    out.mkdir(parents=True, exist_ok=True)
    (out / "size_sweep.json").write_text(json.dumps(rows, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()
