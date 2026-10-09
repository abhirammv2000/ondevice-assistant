import struct

import numpy as np
import pytest

import model_format
from conftest import GOLDEN


def make(int8: bool, n_features=64, n_classes=5, seed=0):
    rng = np.random.default_rng(seed)
    dense = rng.normal(size=(n_features, n_classes)).astype(np.float32)
    bias = rng.normal(size=n_classes).astype(np.float32)
    names = [f"class{i}" for i in range(n_classes)]
    if int8:
        q, scales = model_format.quantize(dense)
        return model_format.Model(names, bias, q, scales), dense
    return model_format.Model(names, bias, dense, None), dense


@pytest.mark.parametrize("int8", [True, False])
def test_round_trip(int8):
    model, _ = make(int8)
    again = model_format.from_bytes(model_format.to_bytes(model))
    assert again.class_names == model.class_names
    assert np.array_equal(again.weights, model.weights) and np.array_equal(again.bias, model.bias)
    assert (again.scales is None) == (model.scales is None)


def test_sections_are_aligned_as_the_header_documents():
    scales_offset, bias_offset, weights_offset = struct.unpack_from("<III", model_format.to_bytes(make(True)[0]), 28)
    assert scales_offset % 4 == 0 and bias_offset % 4 == 0
    assert weights_offset % 64 == 0  # int8 weights start on a cache line
    float_weights_offset = struct.unpack_from("<III", model_format.to_bytes(make(False)[0]), 28)[2]
    assert float_weights_offset % 4 == 0


def test_damage_is_detected():
    data = bytearray(model_format.to_bytes(make(True)[0]))
    data[200] ^= 0x55
    with pytest.raises(ValueError, match="checksum"):
        model_format.from_bytes(bytes(data))


def test_bad_magic_and_short_files():
    with pytest.raises(ValueError):
        model_format.from_bytes(b"XXXX" + b"\0" * 100)
    with pytest.raises(ValueError):
        model_format.from_bytes(b"PMDL")


def test_writer_refuses_bad_shapes():
    rng = np.random.default_rng(0)
    for rows in (63, 100):  # not a power of two, or too small
        weights = rng.normal(size=(rows, 2)).astype(np.float32)
        with pytest.raises(ValueError):
            model_format.to_bytes(model_format.Model(["a", "b"], np.zeros(2, np.float32), weights, None))
    with pytest.raises(ValueError):  # names do not match the columns
        model_format.to_bytes(model_format.Model(["a"], np.zeros(2, np.float32), np.zeros((64, 2), np.float32), None))
    with pytest.raises(ValueError):  # int8 flag with float weights
        model_format.to_bytes(model_format.Model(["a", "b"], np.zeros(2, np.float32), np.zeros((64, 2), np.float32), np.ones(2, np.float32)))


def test_quantization_error_is_at_most_half_a_step():
    _, dense = make(True, n_features=256, n_classes=7, seed=3)
    q, scales = model_format.quantize(dense)
    assert q.dtype == np.int8 and np.abs(q).max() <= 127
    assert np.all(np.abs(dense - q.astype(np.float32) * scales) <= scales / 2 + 1e-6)
    assert np.all(np.abs(q).max(axis=0) == 127)  # each class uses the full range


def test_all_zero_class_does_not_divide_by_zero():
    weights = np.zeros((64, 3), np.float32)
    weights[:, 1] = 1.0
    q, scales = model_format.quantize(weights)
    assert np.all(np.isfinite(scales)) and np.all(q[:, 0] == 0)


def test_probabilities_sum_to_one_and_empty_input_uses_bias_only():
    for int8 in (True, False):
        model, _ = make(int8)
        p = model_format.probabilities(model, [3, 17, 40])
        assert abs(float(p.sum()) - 1.0) < 1e-5
        empty = model_format.probabilities(model, [])
        expect = np.exp(model.bias - model.bias.max())
        assert np.allclose(empty, expect / expect.sum(), atol=1e-6)


def test_int8_scores_stay_close_to_float_scores():
    int8_model, dense = make(True, n_features=512, n_classes=20, seed=9)
    float_model = model_format.Model(int8_model.class_names, int8_model.bias, dense, None)
    rng = np.random.default_rng(1)
    worst = 0.0
    for _ in range(50):
        feats = [int(x) for x in rng.integers(0, 2**32, size=12)]
        diff = np.abs(model_format.probabilities(int8_model, feats) - model_format.probabilities(float_model, feats))
        worst = max(worst, float(diff.max()))
    assert worst < 0.05


@pytest.mark.parametrize("kind", ["int8", "float"])
def test_golden_expectations(kind):
    model = model_format.read(GOLDEN / f"tiny_{kind}.pmodel")
    checked = 0
    for line in (GOLDEN / "tiny_expect.tsv").read_text(encoding="utf-8").splitlines():
        k, feats, probs = line.split("\t")
        if k != kind:
            continue
        got = model_format.probabilities(model, [int(x) for x in feats.split(",")] if feats else [])
        assert np.allclose(got, [float(x) for x in probs.split(",")], atol=1e-7)
        checked += 1
    assert checked >= 3
