"""CLINC150 loading. The data is Larson et al., "An Evaluation Dataset for Intent Classification and Out-of-Scope
Prediction", EMNLP 2019, released under CC BY 3.0: https://github.com/clinc/oos-eval

150 intents across ten domains (alarms, timers, reminders, weather, calendar, translation, calls, music, banking and
so on), 100 training utterances per intent, plus utterances that match none of the 150 ("out of scope"). That last
part is what a device needs: the common case is a request it can handle, and the dangerous one is a request it
thinks it can handle and cannot.

    python tools/clinc.py      # downloads data_full.json into data/raw/
"""
from __future__ import annotations

import json
import urllib.request
from dataclasses import dataclass
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
URL = "https://raw.githubusercontent.com/clinc/oos-eval/master/data/data_full.json"
PATH = ROOT / "data" / "raw" / "clinc_data_full.json"
OOS = "oos"


@dataclass
class Split:
    texts: list[str]
    labels: list[str]


@dataclass
class Clinc:
    intents: list[str]   # the 150 in-scope intents, sorted
    train: Split
    val: Split
    test: Split
    oos_train: Split
    oos_val: Split
    oos_test: Split


def download(path: Path = PATH) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    if not path.exists():
        urllib.request.urlretrieve(URL, path)  # noqa: S310 - a fixed https URL
    return path


def _split(rows: list[list[str]]) -> Split:
    return Split([r[0] for r in rows], [r[1] for r in rows])


def load(path: Path = PATH) -> Clinc:
    data = json.loads(download(path).read_text(encoding="utf-8"))
    intents = sorted({label for _, label in data["train"]})
    assert len(intents) == 150, len(intents)
    assert all(label == OOS for _, label in data["oos_test"])
    return Clinc(intents, _split(data["train"]), _split(data["val"]), _split(data["test"]),
                 _split(data["oos_train"]), _split(data["oos_val"]), _split(data["oos_test"]))


if __name__ == "__main__":
    c = load()
    print(f"{len(c.intents)} intents, {len(c.train.texts)} train, {len(c.val.texts)} val, {len(c.test.texts)} test, "
          f"{len(c.oos_test.texts)} out-of-scope test")
