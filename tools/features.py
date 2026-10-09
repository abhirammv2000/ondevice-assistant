"""The feature extractor, in Python, following the same specification as core/src/tokenizer.cpp.

The model is trained on what this produces and run on a device that computes the same thing in C++, so the two
must agree exactly. tests/py/test_features_parity.py checks this file against golden values, and the C++ tests check
the C++ code against the same golden values (tools/make_golden.py writes them from this file).

Specification (docs/DESIGN.md has the reasoning):
  - the input is bytes. ASCII letters are lower-cased. Letters, digits, apostrophes and every byte >= 0x80 form a
    token. Anything else separates tokens.
  - a token of only ASCII digits becomes "<num>".
  - at most 96 tokens and 512 bytes of normalised text. A token that does not fit is dropped with everything after it.
  - features are 32-bit folded FNV-1a hashes of "u:<token>" and of "b:<a> <b>" for neighbouring pairs, where "^" and
    "$" stand for the start and end of the utterance. No tokens means no features.
  - the result is sorted and has no duplicates.
"""
from __future__ import annotations

FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = 0xFFFFFFFFFFFFFFFF

MAX_BYTES = 512
MAX_TOKENS = 96


def fnv1a(data: bytes, h: int = FNV_OFFSET) -> int:
    for byte in data:
        h ^= byte
        h = (h * FNV_PRIME) & MASK64
    return h


def fold32(h: int) -> int:
    return (h ^ (h >> 32)) & 0xFFFFFFFF


def _is_token_byte(b: int) -> bool:
    return (0x61 <= b <= 0x7A) or (0x41 <= b <= 0x5A) or (0x30 <= b <= 0x39) or b == 0x27 or b >= 0x80


def tokenize(text: bytes | str) -> list[bytes]:
    data = text.encode("utf-8") if isinstance(text, str) else bytes(text)
    tokens: list[bytes] = []
    used = 0
    i, n = 0, len(data)
    while len(tokens) < MAX_TOKENS:
        while i < n and not _is_token_byte(data[i]):
            i += 1
        if i >= n:
            break
        j = i
        all_digits = True
        while j < n and _is_token_byte(data[j]):
            all_digits = all_digits and 0x30 <= data[j] <= 0x39
            j += 1
        token = b"<num>" if all_digits else bytes((b + 32) if 0x41 <= b <= 0x5A else b for b in data[i:j])
        if used + len(token) > MAX_BYTES:
            break
        tokens.append(token)
        used += len(token)
        i = j
    return tokens


def features(text: bytes | str) -> list[int]:
    tokens = tokenize(text)
    if not tokens:
        return []
    unigram = fnv1a(b"u:")
    bigram = fnv1a(b"b:")

    def pair(a: bytes, b: bytes) -> int:
        return fold32(fnv1a(b, fnv1a(b" ", fnv1a(a, bigram))))

    found = [fold32(fnv1a(t, unigram)) for t in tokens]
    found.append(pair(b"^", tokens[0]))
    found += [pair(tokens[k], tokens[k + 1]) for k in range(len(tokens) - 1)]
    found.append(pair(tokens[-1], b"$"))
    return sorted(set(found))
