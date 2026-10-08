"""Write the golden files the C++ tests compare against.

    python tools/make_golden.py

tests/golden/features.tsv
    Each line is the hex of an utterance, a tab, and its comma-separated feature hashes, from tools/features.py.
    Hex keeps tabs, newlines, invalid UTF-8 and very long inputs from breaking the file format.

tests/golden/calendar.tsv
    "year month day days-since-1970 weekday" for edge dates and 3000 random dates from 1600 to 2600, from Python's
    datetime (weekday 0 is Sunday). The C++ calendar maths has to agree.

tests/golden/tiny_int8.pmodel, tiny_float.pmodel, tiny_expect.tsv
    Two small models written by tools/model_format.py, and the probabilities the Python reference computes for some
    feature lists. The C++ loader reads the same files and has to agree, which checks the file format end to end.
"""
from __future__ import annotations

import datetime
import random
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import model_format  # noqa: E402
from features import features  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]

CASES: list[bytes] = [
    b"",
    b"   \t\n  ",
    b"!!!???",
    b"hello",
    b"Hello World",
    b"set a timer for 10 minutes",
    b"set a timer for 25 minutes",
    b"what's the weather like in San Francisco",
    b"wake me up at 7:30 tomorrow",
    b"3rd of May",
    b"007",
    b"a'b'c",
    b"'",
    "café au lait".encode(),
    "タイマーをセット".encode(),
    b"\xff\xfe broken \xc3 bytes",
    b"UPPER lower MiXeD",
    b"call mom, then text dad!",
    b"the the the the",
    b"a " * 120,
    b"word " * 200,
    (b"averyveryverylongtoken" * 30),
    b"x" * 600,
    b"1 " * 130,
]

TINY_FEATURE_LISTS: list[list[int]] = [
    [],
    [0],
    [5],
    [63],
    [64],                       # wraps to row 0
    [4294967295],               # the largest hash, masked down to row 63
    [1, 2, 3],
    [10, 20, 30, 40, 50, 60],
    list(range(0, 64)),
    [7, 7 + 64, 7 + 128],       # three hashes that land on the same row
    [3, 17, 29, 41, 53],
    [2**31, 2**31 + 1],
]
TINY_NAMES = ["alpha", "beta", "gamma", "delta", "épsilon"]


def write_feature_golden(out: Path) -> None:
    lines = [f"{case.hex()}\t{','.join(str(f) for f in features(case))}" for case in CASES]
    (out / "features.tsv").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"features.tsv: {len(lines)} cases")


def write_calendar_golden(out: Path) -> None:
    edge = [(1970, 1, 1), (1969, 12, 31), (1900, 2, 28), (1900, 3, 1), (2000, 2, 29), (2100, 2, 28), (2100, 3, 1),
            (2024, 2, 29), (2023, 12, 31), (2038, 1, 19), (1600, 1, 1), (2600, 12, 31), (1600, 3, 1)]
    rng = random.Random(7)
    base = datetime.date(1600, 1, 1).toordinal()
    span = datetime.date(2600, 12, 31).toordinal() - base
    dates = edge + [datetime.date.fromordinal(base + rng.randrange(span)).timetuple()[:3] for _ in range(3000)]
    lines = []
    for y, m, d in dates:
        date = datetime.date(y, m, d)
        days = date.toordinal() - datetime.date(1970, 1, 1).toordinal()
        lines.append(f"{y} {m} {d} {days} {(date.weekday() + 1) % 7}")
    (out / "calendar.tsv").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"calendar.tsv: {len(lines)} dates")


def write_model_golden(out: Path) -> None:
    rng = np.random.default_rng(20261008)
    dense = rng.normal(0.0, 1.0, size=(64, len(TINY_NAMES))).astype(np.float32)
    bias = rng.normal(0.0, 0.5, size=len(TINY_NAMES)).astype(np.float32)
    q, scales = model_format.quantize(dense)
    models = {
        "int8": model_format.Model(TINY_NAMES, bias, q, scales),
        "float": model_format.Model(TINY_NAMES, bias, dense, None),
    }
    lines = []
    for kind, model in models.items():
        model_format.write(out / f"tiny_{kind}.pmodel", model)
        # the file must read back to the same model before it is trusted as a golden
        again = model_format.read(out / f"tiny_{kind}.pmodel")
        assert again.class_names == model.class_names and np.array_equal(again.weights, model.weights)
        for feats in TINY_FEATURE_LISTS:
            probs = model_format.probabilities(model, feats)
            lines.append(f"{kind}\t{','.join(str(f) for f in feats)}\t{','.join(repr(float(p)) for p in probs)}")
    (out / "tiny_expect.tsv").write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    print(f"tiny models: {len(lines)} expectations")


def main() -> None:
    out = ROOT / "tests" / "golden"
    out.mkdir(parents=True, exist_ok=True)
    write_feature_golden(out)
    write_calendar_golden(out)
    write_model_golden(out)


if __name__ == "__main__":
    main()
