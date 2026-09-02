#!/usr/bin/env python3
"""Contract C5, Part C follow-up — time-split held-out evaluation of the
reply-delay quantile model (scripts/fit_reply_delay_model.py) against a
single global-median heuristic.

Read-only against ~/Library/Messages/chat.db. No daemon, no :8741, no
network. Fits the SAME hierarchical quantile model as
fit_reply_delay_model.py, but on a TEMPORAL split of the reply-gap samples
(earliest --train-frac by timestamp fit the model; the latest remainder is
held out and never touched during fitting) — this is the standard
time-series-safe held-out design: fitting on later data and testing on
earlier data would let the model see the "future" relative to its test
set, which is not how the C loader will ever actually use it in
production (always predicting forward in time from whatever chat.db holds
so far).

For each held-out reply gap we compute a POINT prediction (the p50 of the
resolved bucket — same hierarchical fallback chain the C loader uses:
exact (hour,len,freq) cell -> (hour,len) marginal -> (hour) marginal ->
global) for two candidates:

  (a) the fitted per-hour/len/freq model, trained on the EARLIEST 80%
  (b) a single global-median heuristic — the TRAIN set's overall median
      reply delay, used as the prediction for every held-out example
      regardless of hour/length/contact

(c) "the daemon's current heuristic reproduced in Python" is DELIBERATELY
NOT evaluated here. src/daemon.c:9770-9781 is the only delay-shaped
heuristic in the codebase adjacent to a reply:

    if (agent->persona && agent->persona->avg_response_time_sec > 0.0) {
        double base_sec = agent->persona->avg_response_time_sec;
        unsigned int jitter = (unsigned int)(rand() % 2000u);
        unsigned int delay_ms = (unsigned int)(base_sec * 300.0) + jitter;
        if (delay_ms > 8000)
            delay_ms = 8000;
        hu_platform_sleep_ms(delay_ms);
    }

That heuristic is CAPPED AT 8000ms (8 seconds) and models a completely
different quantity: the "reading + typing pause" before an
already-decided-to-reply-now reactive turn types its response. It cannot
express a value above 8 seconds. The quantity this script's model
predicts is the full "message arrives -> Seth notices and replies" gap,
whose held-out median-per-hour ranges from ~30s to ~1000s and whose global
p90 is in the multi-hour range (see ~/.human/reply_delay_model.json).
Reproducing the 8-second-capped heuristic and scoring it against
multi-minute/hour gaps would not be a faithful "is the model better than
the status quo" comparison — it would structurally lose to everything
above 8 seconds regardless of how good or bad the actual heuristic is at
its own (different) job. Per this task's own escape hatch, (c) is skipped
and only (a) and (b) are evaluated.

Metrics, computed per held-out sample and aggregated:
  - MAE (mean absolute error, seconds)
  - median absolute log-error: median(|ln(max(pred,1) / max(actual,1))|)
    (both floored at 1s so a same-second exact hit or a same-second miss
    never divides by / takes the log of zero)

Also reports a 95% PAIRED bootstrap CI (resampling held-out examples with
replacement, B=2000 by default) on (model - global), for both the
log-error metric and the MAE-seconds metric.

CAVEAT (documented, not hidden): contact reply-frequency terciles are
computed from ALL messages in the lookback window (train+test), not just
the train portion — a minor leak into which BUCKET a held-out example's
frequency falls into, not into any predicted VALUE (the quantiles
themselves are fit strictly on the train samples). Given the scale of this
evaluation (a directional accuracy check, not a rigorous ML benchmark),
this is called out rather than engineering a second full historical
message load scoped to before the split timestamp.

Refuses (exit 2, writes nothing) if the held-out set has fewer than
--min-heldout-n samples (default 200).

Writes ~/.human/logs/reply-delay-heldout-<date>.json (numbers only — no
message content, no contact identifiers) and, when --copy-to-plan is set
(default on), an identical copy under
docs/plans/2026-08-02-semantic-retrieval/.
"""
import argparse
import json
import math
import os
import random
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eval_when_to_speak import open_ro  # noqa: E402
from fit_reply_delay_model import (  # noqa: E402
    build_delay_samples,
    fit,
    freq_tercile,
    length_bucket,
)


def temporal_split(samples, train_frac):
    """Sort by ts ascending, earliest train_frac -> train, remainder ->
    test. Stable and deterministic — no shuffling, this IS a time split."""
    ordered = sorted(samples, key=lambda s: s["ts"])
    split_idx = int(len(ordered) * train_frac)
    return ordered[:split_idx], ordered[split_idx:]


def predict_point(model, hour, len_chars, freq):
    """Returns (p50_prediction, n_in_resolved_cell) using the SAME
    hierarchical fallback chain as src/daemon/daemon_reply_delay.c's
    hu_reply_delay_from_model: exact cell -> (hour,len) marginal ->
    (hour) marginal -> global."""
    lt = model["length_bucket_thresholds"]
    ft_b = model["freq_tercile_boundaries"]
    lb = length_bucket(len_chars, (lt["lo_chars"], lt["hi_chars"]))
    ft = freq_tercile(freq, (ft_b["lo_count"], ft_b["hi_count"]))
    h = max(0, min(23, hour))
    key_full = f"h{h}_l{lb}_f{ft}"
    key_hl = f"h{h}_l{lb}"
    key_h = f"h{h}"

    cell = model["cells"].get(key_full)
    if not cell:
        cell = model["hour_len_marginals"].get(key_hl)
    if not cell:
        cell = model["hour_marginals"].get(key_h)
    if not cell:
        cell = model["global"]
    return cell["quantiles"]["p50"], cell["n"]


def abs_log_error(pred, actual):
    p = max(pred, 1.0)
    a = max(actual, 1.0)
    return abs(math.log(p / a))


def compute_metrics(test_samples, model, contact_msg_counts):
    """Returns dict with per-sample lists (ae_model, ae_global, ale_model,
    ale_global) plus aggregate MAE / median-abs-log-error for each of
    model and global."""
    global_p50 = model["global"]["quantiles"]["p50"]

    ae_model, ae_global, ale_model, ale_global = [], [], [], []
    for s in test_samples:
        actual = s["delay_secs"]
        freq = contact_msg_counts.get(s["contact"], 0)
        pred_model, _n = predict_point(model, s["hour"], s["len_chars"], freq)
        pred_global = global_p50

        ae_model.append(abs(pred_model - actual))
        ae_global.append(abs(pred_global - actual))
        ale_model.append(abs_log_error(pred_model, actual))
        ale_global.append(abs_log_error(pred_global, actual))

    def median(vals):
        s = sorted(vals)
        n = len(s)
        mid = n // 2
        return s[mid] if n % 2 else (s[mid - 1] + s[mid]) / 2.0

    return {
        "ae_model": ae_model,
        "ae_global": ae_global,
        "ale_model": ale_model,
        "ale_global": ale_global,
        "mae_model": sum(ae_model) / len(ae_model),
        "mae_global": sum(ae_global) / len(ae_global),
        "median_ale_model": median(ale_model),
        "median_ale_global": median(ale_global),
    }


def bootstrap_ci_paired_diff(values_a, values_b, n_bootstrap=2000, seed=1234, alpha=0.05):
    """Paired bootstrap CI on mean(a_i - b_i). Resamples INDICES (not a
    and b independently) with replacement so pairing is preserved.
    Returns (mean_diff, ci_lo, ci_hi)."""
    n = len(values_a)
    assert n == len(values_b) and n > 0
    diffs = [a - b for a, b in zip(values_a, values_b)]
    mean_diff = sum(diffs) / n

    rng = random.Random(seed)
    boot_means = []
    for _ in range(n_bootstrap):
        resampled = [diffs[rng.randrange(n)] for _ in range(n)]
        boot_means.append(sum(resampled) / n)
    boot_means.sort()
    lo_idx = int((alpha / 2) * n_bootstrap)
    hi_idx = min(n_bootstrap - 1, int((1 - alpha / 2) * n_bootstrap))
    return mean_diff, boot_means[lo_idx], boot_means[hi_idx]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--days", type=int, default=90)
    ap.add_argument("--max-delay-hours", type=float, default=24.0)
    ap.add_argument("--min-cell-n", type=int, default=5)
    ap.add_argument("--train-frac", type=float, default=0.8)
    ap.add_argument("--min-heldout-n", type=int, default=200)
    ap.add_argument("--n-bootstrap", type=int, default=2000)
    ap.add_argument("--bootstrap-seed", type=int, default=1234)
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--out", default=None, help="defaults to ~/.human/logs/reply-delay-heldout-<date>.json")
    ap.add_argument("--copy-to-plan", action="store_true", default=True)
    ap.add_argument("--no-copy-to-plan", dest="copy_to_plan", action="store_false")
    ap.add_argument(
        "--plan-dir",
        default=os.path.join(
            os.path.dirname(os.path.abspath(__file__)), "..", "docs", "plans", "2026-08-02-semantic-retrieval"
        ),
    )
    args = ap.parse_args()

    now = time.time()
    since = now - args.days * 86400.0

    chat_db = open_ro(args.chat_db)
    if chat_db is None:
        print(f"REFUSE: chat.db not found at {args.chat_db}", file=sys.stderr)
        return 2

    samples, contact_msg_counts = build_delay_samples(chat_db, since, args.max_delay_hours * 3600.0)
    train, test = temporal_split(samples, args.train_frac)

    if len(test) < args.min_heldout_n:
        print(
            f"REFUSE: held-out set has {len(test)} samples, need >= {args.min_heldout_n}",
            file=sys.stderr,
        )
        return 2

    model = fit(train, contact_msg_counts, args.min_cell_n)
    if model is None:
        print("REFUSE: fit() on the train split produced no model", file=sys.stderr)
        return 2

    metrics = compute_metrics(test, model, contact_msg_counts)

    log_diff_mean, log_diff_lo, log_diff_hi = bootstrap_ci_paired_diff(
        metrics["ale_model"], metrics["ale_global"], args.n_bootstrap, args.bootstrap_seed
    )
    mae_diff_mean, mae_diff_lo, mae_diff_hi = bootstrap_ci_paired_diff(
        metrics["ae_model"], metrics["ae_global"], args.n_bootstrap, args.bootstrap_seed + 1
    )

    split_ts = train[-1]["ts"] if train else None
    result = {
        "generated_at": int(now),
        "days": args.days,
        "max_delay_hours": args.max_delay_hours,
        "train_frac": args.train_frac,
        "n_total": len(samples),
        "n_train": len(train),
        "n_heldout": len(test),
        "split_ts_unix": split_ts,
        "heuristic_c_evaluated": False,
        "heuristic_c_note": (
            "src/daemon.c:9770-9781's persona reading-delay heuristic "
            "(base_sec*300ms + jitter, capped at 8000ms) models a different "
            "quantity at a different timescale (sub-8s typing pause, not the "
            "multi-second-to-hour notice-and-reply gap this model targets) "
            "and would not be a faithful comparison; see this script's "
            "docstring for the full reasoning."
        ),
        "model": {
            "mae_seconds": metrics["mae_model"],
            "median_abs_log_error": metrics["median_ale_model"],
        },
        "global_median_heuristic": {
            "mae_seconds": metrics["mae_global"],
            "median_abs_log_error": metrics["median_ale_global"],
            "predicted_value_seconds": model["global"]["quantiles"]["p50"],
        },
        "bootstrap_95ci_model_minus_global": {
            "abs_log_error": {"mean_diff": log_diff_mean, "ci_lo": log_diff_lo, "ci_hi": log_diff_hi},
            "mae_seconds": {"mean_diff": mae_diff_mean, "ci_lo": mae_diff_lo, "ci_hi": mae_diff_hi},
            "n_bootstrap": args.n_bootstrap,
        },
        "caveats": [
            "contact frequency terciles computed over the full (train+test) "
            "window, not train-only — affects bucket selection only, not "
            "the fitted quantile values themselves",
            "point predictions use each resolved bucket's p50 (median), "
            "not a random sample from the quantile table (that sampling "
            "behavior is what the C production loader uses for natural "
            "variance; a point predictor is the correct comparison for "
            "accuracy metrics)",
        ],
    }

    date_str = time.strftime("%Y-%m-%d", time.localtime(now))
    out_path = args.out or os.path.expanduser(f"~/.human/logs/reply-delay-heldout-{date_str}.json")
    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    with open(out_path, "w") as f:
        json.dump(result, f, indent=2)
    print(f"wrote {out_path}")

    if args.copy_to_plan:
        plan_dir = os.path.abspath(args.plan_dir)
        if os.path.isdir(plan_dir):
            plan_path = os.path.join(plan_dir, f"reply-delay-heldout-{date_str}.json")
            with open(plan_path, "w") as f:
                json.dump(result, f, indent=2)
            print(f"wrote {plan_path}")
        else:
            print(f"NOTE: --copy-to-plan set but {plan_dir} does not exist; skipped", file=sys.stderr)

    print(f"n_total={result['n_total']} n_train={result['n_train']} n_heldout={result['n_heldout']}")
    print(
        f"model:            MAE={metrics['mae_model']:.1f}s  median_abs_log_error={metrics['median_ale_model']:.4f}"
    )
    print(
        f"global_heuristic: MAE={metrics['mae_global']:.1f}s  median_abs_log_error={metrics['median_ale_global']:.4f}"
        f"  (predicts {model['global']['quantiles']['p50']:.1f}s always)"
    )
    print(
        f"bootstrap 95% CI (model - global), abs_log_error: mean={log_diff_mean:.4f} "
        f"[{log_diff_lo:.4f}, {log_diff_hi:.4f}]"
    )
    print(
        f"bootstrap 95% CI (model - global), MAE seconds:   mean={mae_diff_mean:.1f} "
        f"[{mae_diff_lo:.1f}, {mae_diff_hi:.1f}]"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
