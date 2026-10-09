import numpy as np

import evaluate


def brute_force_auroc(pos, neg):
    wins = sum((p > n) + 0.5 * (p == n) for p in pos for n in neg)
    return wins / (len(pos) * len(neg))


def test_auroc_known_values():
    assert evaluate.auroc(np.array([0.9, 0.8]), np.array([0.1, 0.2])) == 1.0
    assert evaluate.auroc(np.array([0.1, 0.2]), np.array([0.9, 0.8])) == 0.0
    assert evaluate.auroc(np.array([0.5, 0.5]), np.array([0.5, 0.5])) == 0.5


def test_auroc_matches_pairwise_definition_with_ties():
    rng = np.random.default_rng(4)
    pos = np.round(rng.random(60), 1)  # rounding forces ties
    neg = np.round(rng.random(80) * 0.8, 1)
    assert abs(evaluate.auroc(pos, neg) - brute_force_auroc(pos, neg)) < 1e-12


def test_ece_extremes():
    assert evaluate.ece(np.array([1.0, 1.0, 1.0]), np.array([True, True, True])) == 0.0
    assert abs(evaluate.ece(np.array([1.0, 1.0]), np.array([False, False])) - 1.0) < 1e-12
    # 80% confident and right 4 times in 5 is calibrated
    assert evaluate.ece(np.full(5, 0.8), np.array([True, True, True, True, False])) < 1e-9


def test_threshold_table_trades_coverage_for_accuracy():
    rng = np.random.default_rng(2)
    p_in = rng.dirichlet(np.ones(4) * 0.3, size=400)
    y_in = np.where(rng.random(400) < 0.85, p_in.argmax(axis=1), rng.integers(0, 4, size=400))
    p_oos = rng.dirichlet(np.ones(4) * 2.0, size=100)
    rows = evaluate.threshold_table(p_in, y_in, p_oos, [0.0, 0.3, 0.6, 0.9])
    answered = [r["answered_on_device"] for r in rows]
    caught = [r["out_of_scope_caught"] for r in rows]
    assert rows[0]["answered_on_device"] == 1.0 and rows[0]["out_of_scope_caught"] == 0.0
    assert answered == sorted(answered, reverse=True) and caught == sorted(caught)
    for r in rows:
        assert abs(r["out_of_scope_caught"] + r["out_of_scope_answered_wrongly"] - 1.0) < 1e-12
