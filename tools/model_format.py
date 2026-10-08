"""Read and write the .pmodel file the C++ runtime maps. The layout is described in core/include/assist/model.hpp.

Also contains probabilities(), a reference implementation of the on-device scoring in numpy. It follows the C++
arithmetic (an integer sum per class, then one float scale), so tests can compare the two.
"""
from __future__ import annotations

import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

import numpy as np

MAGIC = b"PMDL"
VERSION = 1
HEADER_BYTES = 64
FLAG_INT8 = 1
MAX_CLASSES = 4096


@dataclass
class Model:
    class_names: list[str]
    bias: np.ndarray            # float32 [C]
    weights: np.ndarray         # int8 [F, C] when scales is set, otherwise float32 [F, C]
    scales: np.ndarray | None   # float32 [C] for the int8 variant

    @property
    def n_features(self) -> int:
        return int(self.weights.shape[0])

    @property
    def n_classes(self) -> int:
        return int(self.weights.shape[1])


def quantize(weights: np.ndarray) -> tuple[np.ndarray, np.ndarray]:
    """Per-class symmetric int8: each class column is scaled so its largest weight maps to 127."""
    peak = np.abs(weights).max(axis=0)
    scales = np.where(peak > 0, peak / 127.0, 1.0).astype(np.float32)
    q = np.clip(np.rint(weights / scales), -127, 127).astype(np.int8)
    return q, scales


def _pad(buf: bytearray, alignment: int) -> None:
    buf.extend(b"\0" * (-len(buf) % alignment))


def to_bytes(model: Model) -> bytes:
    n_features, n_classes = model.weights.shape
    int8 = model.scales is not None
    if int8 and model.weights.dtype != np.int8:
        raise ValueError("an int8 model needs int8 weights")
    if not int8 and model.weights.dtype != np.float32:
        raise ValueError("a float model needs float32 weights")
    if n_features & (n_features - 1) or n_features < 64:
        raise ValueError("n_features must be a power of two, at least 64")
    if not 1 <= n_classes <= MAX_CLASSES or len(model.class_names) != n_classes:
        raise ValueError("class count out of range or not matching the names")

    body = bytearray(b"\0" * HEADER_BYTES)

    names_offset = len(body)
    for name in model.class_names:
        raw = name.encode("utf-8")
        body += struct.pack("<H", len(raw)) + raw
    names_bytes = len(body) - names_offset
    _pad(body, 4)

    scales_offset = 0
    if int8:
        scales_offset = len(body)
        body += np.ascontiguousarray(model.scales, dtype="<f4").tobytes()
    bias_offset = len(body)
    body += np.ascontiguousarray(model.bias, dtype="<f4").tobytes()

    _pad(body, 64 if int8 else 4)
    weights_offset = len(body)
    weights = np.ascontiguousarray(model.weights, dtype=np.int8 if int8 else "<f4")
    body += weights.tobytes()
    weights_bytes = weights.nbytes
    _pad(body, 4)

    struct.pack_into("<4sIIIIIIIIIQ", body, 0, MAGIC, VERSION, n_classes, n_features, FLAG_INT8 if int8 else 0,
                     names_offset, names_bytes, scales_offset, bias_offset, weights_offset, weights_bytes)
    body += struct.pack("<I", zlib.crc32(bytes(body)) & 0xFFFFFFFF)
    return bytes(body)


def write(path: str | Path, model: Model) -> int:
    data = to_bytes(model)
    Path(path).write_bytes(data)
    return len(data)


def from_bytes(data: bytes) -> Model:
    if len(data) < HEADER_BYTES + 4 or data[:4] != MAGIC:
        raise ValueError("not a pmodel file")
    (_, version, n_classes, n_features, flags, names_offset, names_bytes, scales_offset, bias_offset, weights_offset,
     weights_bytes) = struct.unpack_from("<4sIIIIIIIIIQ", data, 0)
    if version != VERSION:
        raise ValueError(f"unsupported version {version}")
    if zlib.crc32(data[:-4]) & 0xFFFFFFFF != struct.unpack_from("<I", data, len(data) - 4)[0]:
        raise ValueError("checksum mismatch")
    int8 = bool(flags & FLAG_INT8)

    names, cursor = [], names_offset
    for _ in range(n_classes):
        (length,) = struct.unpack_from("<H", data, cursor)
        names.append(data[cursor + 2: cursor + 2 + length].decode("utf-8"))
        cursor += 2 + length
    if cursor != names_offset + names_bytes:
        raise ValueError("names section has the wrong size")

    bias = np.frombuffer(data, dtype="<f4", count=n_classes, offset=bias_offset).copy()
    scales = np.frombuffer(data, dtype="<f4", count=n_classes, offset=scales_offset).copy() if int8 else None
    dtype = np.int8 if int8 else "<f4"
    weights = np.frombuffer(data, dtype=dtype, count=n_features * n_classes, offset=weights_offset).reshape(n_features, n_classes).copy()
    return Model(names, bias, weights.astype(np.int8 if int8 else np.float32), scales)


def read(path: str | Path) -> Model:
    return from_bytes(Path(path).read_bytes())


def probabilities(model: Model, feature_hashes: list[int]) -> np.ndarray:
    """Class probabilities for one utterance, computed the way core/src/model.cpp does it."""
    n = len(feature_hashes)
    if n == 0:
        logits = model.bias.astype(np.float32)
    else:
        rows = np.array(feature_hashes, dtype=np.uint32) & np.uint32(model.n_features - 1)
        inv_norm = np.float32(1.0) / np.sqrt(np.float32(n))
        if model.scales is not None:
            acc = model.weights[rows].astype(np.int32).sum(axis=0)
            logits = model.bias + model.scales * acc.astype(np.float32) * inv_norm
        else:
            logits = model.bias + model.weights[rows].sum(axis=0, dtype=np.float32) * inv_norm
    logits = logits.astype(np.float32)
    shifted = np.exp(logits - logits.max())
    return (shifted / shifted.sum()).astype(np.float32)
