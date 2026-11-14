"""Fill model: does knowing more than the queue size predict fills better?

    python3 tools/fill_model.py <probes.csv> [target ...]

Reads fill_study's probe table. Each `day` value is one recorded session, in time order.
Walk-forward: for every session k from the 4th on, train on all sessions before it and
test on session k alone, so the model never sees the future it is judged on.

  baseline: logistic regression on side and log(queue size) only
  model:    gradient-boosted trees on all 14 features known when the probe joined

Scored by log loss on the test session (lower is better; it punishes confident
mistakes) and AUC (how well it ranks fills above non-fills; 0.5 is a coin toss).
The rule for calling a win was fixed before the first run (see notes/state.md):
lower loss in 7+ of 9 test sessions and a mean gain of 2%+.

Leak check: the same model trained on shuffled labels must do no better than
guessing the training fill rate. If it does, something in the features is leaking.
"""
import csv
import sys

import numpy as np
from sklearn.ensemble import HistGradientBoostingClassifier
from sklearn.linear_model import LogisticRegression
from sklearn.metrics import log_loss, roc_auc_score

FEATURES = ["side", "queue_btc", "other_queue_btc", "queue_imbalance", "spread_cents", "bid_depth5_btc",
            "ask_depth5_btc", "buy_1s_btc", "sell_1s_btc", "buy_5s_btc", "sell_5s_btc", "trades_1s",
            "mid_move_5s_cents", "mid_wiggle_5s_cents"]
FIRST_TEST = 4  # the first session tested; it trains on sessions 1 to 3
EPS = 1e-6


def load(path, targets):
    with open(path) as f:
        reader = csv.reader(f)
        header = next(reader)
        col = {name: i for i, name in enumerate(header)}
        rows = [r for r in reader]
    day = np.array([int(r[col["day"]]) for r in rows])
    x = np.array([[float(r[col[n]]) for n in FEATURES] for r in rows])
    y = {t: np.array([int(r[col[t]]) for r in rows]) for t in targets}
    return day, x, y


def baseline_inputs(x):
    side = x[:, 0]
    log_queue = np.log(x[:, 1] + 1e-4)  # queue in BTC; the small floor keeps an empty queue finite
    return np.column_stack([side, log_queue])


def make_model(seed=0):
    # Settings fixed before the first run and never tuned on test sessions.
    return HistGradientBoostingClassifier(max_iter=200, learning_rate=0.1, max_leaf_nodes=31, min_samples_leaf=200,
                                          early_stopping=False, random_state=seed)


def score(y, p):
    p = np.clip(p, EPS, 1 - EPS)
    auc = roc_auc_score(y, p) if 0 < y.sum() < len(y) else float("nan")
    return log_loss(y, p, labels=[0, 1]), auc


def run(day, x, y, target, leak_check):
    print(f"\n== {target}: fill rate {y.mean():.1%} over {len(y):,} probes ==")
    print(f"{'test':>4} {'probes':>7} {'rate':>6} {'const':>7} {'base':>7} {'model':>7} {'gain':>6} "
          f"{'baseAUC':>7} {'modAUC':>7}")
    wins, gains = 0, []
    sessions = sorted(set(day))
    for k in sessions[FIRST_TEST - 1:]:
        train, test = day < k, day == k
        const = np.full(test.sum(), y[train].mean())
        base = LogisticRegression(max_iter=1000).fit(baseline_inputs(x[train]), y[train])
        model = make_model().fit(x[train], y[train])
        c_loss, _ = score(y[test], const)
        b_loss, b_auc = score(y[test], base.predict_proba(baseline_inputs(x[test]))[:, 1])
        m_loss, m_auc = score(y[test], model.predict_proba(x[test])[:, 1])
        gain = (b_loss - m_loss) / b_loss
        wins += m_loss < b_loss
        gains.append(gain)
        print(f"{k:>4} {test.sum():>7,} {y[test].mean():>6.1%} {c_loss:>7.4f} {b_loss:>7.4f} {m_loss:>7.4f} "
              f"{gain:>+6.1%} {b_auc:>7.3f} {m_auc:>7.3f}")
    folds = len(gains)
    mean_gain = float(np.mean(gains))
    verdict = ("win" if wins >= 7 and mean_gain >= 0.02 else "barely" if wins >= 7 else "fail") if folds == 9 else "n/a"
    print(f"model beat baseline in {wins} of {folds} sessions, mean gain {mean_gain:+.1%} -> {verdict}")

    if leak_check:
        k = sessions[-1]
        train, test = day < k, day == k
        shuffled = np.random.default_rng(1).permutation(y[train])
        model = make_model().fit(x[train], shuffled)
        s_loss, s_auc = score(y[test], model.predict_proba(x[test])[:, 1])
        c_loss, _ = score(y[test], np.full(test.sum(), shuffled.mean()))
        ok = s_loss >= c_loss - 1e-3
        print(f"leak check (session {k}, shuffled labels): loss {s_loss:.4f} vs constant {c_loss:.4f}, "
              f"AUC {s_auc:.3f} -> {'clean' if ok else 'LEAK?'}")
    return verdict


def main():
    if len(sys.argv) < 2:
        sys.exit("usage: fill_model.py <probes.csv> [target ...]")
    targets = sys.argv[2:] or ["certain_5s"]
    day, x, y = load(sys.argv[1], targets)
    for i, t in enumerate(targets):
        run(day, x, y[t], t, leak_check=(i == 0))


if __name__ == "__main__":
    main()
