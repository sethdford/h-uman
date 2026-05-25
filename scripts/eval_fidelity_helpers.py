#!/usr/bin/env python3
"""
Shared bootstrap CI and shape-scoring utilities for nightly fidelity eval.

Reuses the deterministic shape classifier from eval_shape_classifier.py
(which is h-uman's canonical persona-fidelity scorer) and provides
bootstrap confidence interval calculation for gate thresholds.
"""

import json
import random
import statistics
from pathlib import Path
from typing import List, Tuple

# Import the classifier from the sibling script
import sys
sys.path.insert(0, str(Path(__file__).parent))
from eval_shape_classifier import classify  # noqa: E402


def bootstrap_ci(
    values: List[float],
    n_resamples: int = 100,
    confidence: float = 0.975,
    seed: int = 42
) -> Tuple[float, float, float]:
    """Compute bootstrap confidence interval on a list of values.

    Args:
        values: list of numeric values (e.g., shape scores [0, 1])
        n_resamples: number of bootstrap resamples (default 100)
        confidence: confidence level (default 0.975 for one-sided α=0.025)
        seed: RNG seed for reproducibility

    Returns:
        (mean, lower_ci, upper_ci) tuple. For one-sided intervals,
        confidence=0.975 gives a one-sided α=0.025 test.
    """
    if not values:
        return (0.0, 0.0, 0.0)
    if len(values) == 1:
        return (values[0], values[0], values[0])

    rng = random.Random(seed)
    n = len(values)
    resample_means = []

    for _ in range(n_resamples):
        sample = [values[rng.randrange(n)] for _ in range(n)]
        resample_means.append(sum(sample) / n)

    resample_means.sort()
    alpha = (1 - confidence) / 2  # for two-sided; one-sided uses just alpha
    lo_idx = int(alpha * n_resamples)
    hi_idx = int((1 - alpha) * n_resamples)

    mean = statistics.mean(values)
    lo = resample_means[lo_idx]
    hi = resample_means[hi_idx]

    return (mean, lo, hi)


def compute_persona_fidelity_scores(
    responses: List[str],
    channel: str = "imessage"
) -> Tuple[List[dict], float]:
    """Score a list of responses using the deterministic shape classifier.

    Args:
        responses: list of model response strings
        channel: channel name (imessage, telegram, discord, slack, email)

    Returns:
        (classifications, mean_score) where classifications is a list of
        classify() result dicts and mean_score is the mean shape score.
    """
    if not responses:
        return ([], 0.0)

    classifications = [classify(r, channel=channel) for r in responses]
    mean = statistics.mean(c["score"] for c in classifications)
    return (classifications, mean)


def load_held_out_prompts_from_jsonl(jsonl_path: str) -> List[dict]:
    """Load held-out prompts from a JSONL file.

    Expected format: one JSON object per line with at least a "prompt" field.

    Args:
        jsonl_path: path to JSONL file

    Returns:
        List of prompt objects (dicts) with "prompt" key
    """
    prompts = []
    path = Path(jsonl_path)
    if not path.exists():
        return []

    try:
        with open(path, "r") as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                obj = json.loads(line)
                if "prompt" in obj:
                    prompts.append(obj)
    except (IOError, json.JSONDecodeError) as e:
        print(f"[ERROR] Failed to load held-out prompts from {jsonl_path}: {e}")
        return []

    return prompts


if __name__ == "__main__":
    # Smoke test
    scores = [0.5, 0.7, 0.9, 0.6, 0.8]
    mean, lo, hi = bootstrap_ci(scores, n_resamples=100)
    print(f"Bootstrap CI test: mean={mean:.3f}, CI=[{lo:.3f}, {hi:.3f}]")

    responses = ["hey whatup", "Depending on what you need...", "cool cool"]
    classifications, mean_score = compute_persona_fidelity_scores(responses)
    print(f"Persona fidelity test: mean_score={mean_score:.3f}")
    for c in classifications:
        print(f"  - score={c['score']}, pass={c['pass']}")
