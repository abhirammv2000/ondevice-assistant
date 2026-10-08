"""Run the C++ engine over CLINC150 and measure the whole path, not only the classifier.

    python tools/eval_system.py --cli build/assist_cli --model models/clinc150.pmodel

Two checks:

  parity   The C++ model must give the same answers as the Python reference, because the model was trained in Python
           and runs on the device in C++. Same intent for every utterance, probabilities within 1e-4.

  routing  What happens to a request in the real system. A request whose intent the device can handle should be
           handled, and handled as the right intent. A request it cannot handle, or an out-of-scope one, should
           be sent on. The costly mistake is a handler that runs when it should not have: the wrong action taken for
           the user. That is counted separately from a request that was only sent on unnecessarily.
"""
from __future__ import annotations

import argparse
import json
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import clinc  # noqa: E402
import evaluate  # noqa: E402
import model_format  # noqa: E402
from train import encode  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
HANDLED = {"timer", "alarm", "time", "date", "calculator", "flip_coin", "roll_dice", "make_call", "text",
           "reminder_update", "greeting", "goodbye", "thank_you"}


def run_cli(cli: str, model: str, mode: str, texts: list[str], extra: list[str] | None = None) -> list[list[str]]:
    with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False, encoding="utf-8", newline="\n") as f:
        f.write("\n".join(t.replace("\n", " ") for t in texts) + "\n")
        path = f.name
    out = subprocess.run([cli, "--model", model, "--now", "2026-10-08T09:15", *(extra or []), mode, path], capture_output=True, text=True,
                         encoding="utf-8", check=True).stdout
    Path(path).unlink()
    rows = [line.split("\t") for line in out.split("\n") if line != ""]
    assert len(rows) == len(texts), (len(rows), len(texts))
    return rows


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--cli", required=True)
    parser.add_argument("--model", default=str(ROOT / "models" / "clinc150.pmodel"))
    parser.add_argument("--out", type=Path, default=ROOT / "docs" / "results" / "system_eval.json")
    parser.add_argument("--no-rescue", action="store_true", help="turn the arithmetic rescue off, to see the model alone")
    args = parser.parse_args()
    args.cli = str(Path(args.cli).resolve())

    data = clinc.load()
    model = model_format.read(args.model)
    texts = data.test.texts + data.oos_test.texts
    truth = data.test.labels + [clinc.OOS] * len(data.oos_test.texts)

    # parity
    py_probs = evaluate.batch_probs(model, encode(texts, None, data.intents, model.n_features))
    classified = run_cli(args.cli, args.model, "--classify", texts)
    same = sum(model.class_names[int(py_probs[i].argmax())] == classified[i][0] for i in range(len(texts)))
    worst = max(abs(float(py_probs[i].max()) - float(classified[i][1])) for i in range(len(texts)))
    parity = {"utterances": len(texts), "same_intent": same, "max_confidence_difference": worst}
    print(f"parity: {same}/{len(texts)} the same intent, largest confidence difference {worst:.2e}")

    # routing
    rows = run_cli(args.cli, args.model, "--batch", texts, ["--no-rescue"] if args.no_rescue else None)
    cells: Counter = Counter()
    for (intent, _conf, route, reason, _action), true in zip(rows, truth):
        if true == clinc.OOS:
            kind = "oos"
        elif true in HANDLED:
            kind = "handled_intent"
        else:
            kind = "other_intent"
        if route == "escalate":
            outcome = "sent_on"
        elif intent == true:
            outcome = "handled_correctly"
        else:
            outcome = "handled_wrongly"
        cells[(kind, outcome)] += 1

    report = {"parity": parity, "routing": {}}
    print("\nrouting (each row sums to 100%)")
    print(f"  {'request is':28} {'n':>5} {'handled right':>14} {'sent on':>9} {'handled wrongly':>16}")
    labels = {"handled_intent": "an intent the device handles", "other_intent": "an intent it does not handle", "oos": "out of scope"}
    for kind in ("handled_intent", "other_intent", "oos"):
        n = sum(v for (k, _), v in cells.items() if k == kind)
        row = {o: cells[(kind, o)] / n for o in ("handled_correctly", "sent_on", "handled_wrongly")}
        row["n"] = n
        report["routing"][kind] = row
        print(f"  {labels[kind]:28} {n:5d} {row['handled_correctly']:14.1%} {row['sent_on']:9.1%} {row['handled_wrongly']:16.1%}")
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()
