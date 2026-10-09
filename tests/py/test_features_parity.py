"""The Python feature extractor must agree with the C++ one on every golden case (the C++ tests read the same file)."""
import random

import features
from conftest import GOLDEN


def golden_cases():
    for line in (GOLDEN / "features.tsv").read_text(encoding="utf-8").splitlines():
        hex_text, _, expected = line.partition("\t")
        yield bytes.fromhex(hex_text), [int(x) for x in expected.split(",")] if expected else []


def test_every_golden_case_matches():
    cases = list(golden_cases())
    assert len(cases) > 20
    for text, expected in cases:
        assert features.features(text) == expected, text


def test_output_is_sorted_unique_and_bounded():
    rng = random.Random(1)
    for _ in range(300):
        data = bytes(rng.choice(b"abc DEF123'.,\xc3\xa9\xff\n") for _ in range(rng.randrange(0, 700)))
        out = features.features(data)
        assert out == sorted(set(out))
        assert len(out) <= 2 * features.MAX_TOKENS + 2
        assert all(0 <= f < 2**32 for f in out)


def test_digits_collapse_and_case_is_ignored():
    assert features.features("Call 555 now") == features.features("call 7 now")
    assert features.features("TIMER") == features.features("timer")
    assert features.features("") == [] and features.features("  ,, ") == []


def test_text_and_bytes_agree():
    assert features.features("café au lait") == features.features("café au lait".encode("utf-8"))


def test_fnv1a_known_vectors():
    assert features.fnv1a(b"") == features.FNV_OFFSET
    assert features.fnv1a(b"a") == 0xAF63DC4C8601EC8C  # published FNV-1a 64-bit test vector
    assert features.fnv1a(b"foobar") == 0x85944171F73967E8
