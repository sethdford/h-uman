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
from typing import List, Optional, Tuple

# Import the classifier from the sibling script
import sys
sys.path.insert(0, str(Path(__file__).parent))
from eval_shape_classifier import classify  # noqa: E402
from personaeval_speaker_id import classify_text as _speaker_classify  # noqa: E402

# --- Blended scoring: shape (AI-tell penalties) + speaker-id P(Seth) --------
#
# The shape classifier is penalty-only: it starts at 1.0 and deducts for
# AI-tells (bullets, headers, 'Certainly' openers, excessive length). Any
# plausible short casual string scores 1.0, so on 2026-07-16 BOTH the base
# pass and the adapter pass saturated at mean 1.0 across 29 real generations
# and the delta gate (PASS needs delta >= 0.05) was structurally unwinnable.
#
# The blend adds a discriminative component with dynamic range on clean text:
# the PersonaEval speaker-id logistic regression P(Seth | text) over 15 style
# features (lowercase ratio, Seth-openers, contractions, terminal punctuation
# — see personaeval_speaker_id.featurize). Base-model "clean assistant casual"
# and Seth's measured register genuinely differ on those features (memory:
# measured_style_card — 79% no terminal punct vs model 10%).
#
# Threshold provenance (per .claude/rules/classifier-score-plus-flag-gate.md):
# PRACTICAL_DELTA_FLOOR=0.05 in eval_fidelity_nightly.py was derived for the
# shape scorer. Under this 0.5/0.5 blend, when shape saturates (both passes
# clean) a 0.05 blended delta corresponds to a 0.10 P(Seth) delta — well
# inside measured adapter effects (v4-repair moved fidelity +27pp; register
# shifts move P(Seth) by 0.3+). The floor is therefore kept at 0.05.
SHAPE_WEIGHT = 0.5
SPEAKER_WEIGHT = 0.5
# Persistent home, NOT /tmp: macOS wipes /tmp on reboot, and a wiped model
# means every subsequent nightly silently degrades to shape-only scoring —
# which saturates, SKIPs, and leaves the adapter unmeasured until someone
# notices the FIDELITY_SCORER_DEGRADED marker. Reboot must not cost the gate.
DEFAULT_SPEAKER_MODEL_PATH = Path.home() / ".human/models/seth_speaker_id.json"

# Keys a usable speaker-id logreg model must carry (personaeval_speaker_id
# serialization format, v1 and v2 alike).
_SPEAKER_MODEL_REQUIRED_KEYS = ("weights", "bias", "means", "stds", "feature_names")


def load_speaker_model(path=DEFAULT_SPEAKER_MODEL_PATH) -> Optional[dict]:
    """Load the speaker-id classifier; None when missing/corrupt/not-a-model.

    The model may be absent on a fresh machine or before first training, so
    absence is an expected state — callers must degrade loudly (shape-only
    saturates!), not crash.
    """
    try:
        model = json.loads(Path(path).read_text())
    except (OSError, json.JSONDecodeError):
        return None
    if not isinstance(model, dict):
        return None
    if not all(k in model for k in _SPEAKER_MODEL_REQUIRED_KEYS):
        return None
    return model


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
    channel: str = "imessage",
    speaker_model: Optional[dict] = None,
) -> Tuple[List[dict], float]:
    """Score responses with the shape classifier, optionally blended with
    the speaker-id P(Seth) classifier.

    Args:
        responses: list of model response strings
        channel: channel name (imessage, telegram, discord, slack, email)
        speaker_model: loaded speaker-id logreg dict (load_speaker_model).
            None → pure shape scoring (backward compatible, saturates at
            1.0 on clean casual text — not gate-grade for delta measurement).

    Returns:
        (classifications, mean_score). Each classification keeps the shape
        classify() fields; "score" is the blended score when a speaker model
        is provided, with "shape_score" and "p_seth" recording the components.
    """
    if not responses:
        return ([], 0.0)

    classifications = []
    for r in responses:
        c = dict(classify(r, channel=channel))
        c["shape_score"] = c["score"]
        if speaker_model is not None:
            p = _speaker_classify(speaker_model, r)["p_seth"] if r else 0.0
            c["p_seth"] = p
            c["score"] = SHAPE_WEIGHT * c["shape_score"] + SPEAKER_WEIGHT * p
        classifications.append(c)

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
