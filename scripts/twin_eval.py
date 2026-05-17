#!/usr/bin/env python3
"""Sprint 11 / US-11.10 — Twin-2K-500 forced-choice held-out evaluator.

Twin-2K-500 protocol (arXiv 2505.17479): 2,058 real people answer 500
behavioral questions; persona models are scored on forced-choice accuracy —
given a prompt + N options, the model must pick the option the real user
chose. The metric cannot be gamed by lexical fingerprints because the
answer-set is discrete; the model's predictive distribution must put more
mass on the user's actual choice than on the distractors.

Scope (decisions.md D2, BINDING)
--------------------------------
Sprint 11 ships:
  - This evaluator (`scripts/twin_eval.py`)
  - `tests/fixtures/twin2k_synthetic_10q.jsonl` (10 synthetic PII-free rows)
  - All AC-11.10.* pass against the synthetic fixture

Deferred to Sprint 12:
  - `~/.human/private/twin2k_seth_50q.jsonl` (Seth's real labeling, ~2-3h)
  - Real Gemma-4 forced-choice inference (NotImplementedError; M3 bridge)

The code path works end-to-end on synthetic; real labeling is a future
Seth-action. The implementer must NOT label real data on Seth's behalf.

Scoring math (per design doc, MMLU-style)
-----------------------------------------
For each fixture row:
    prompt          = item["prompt"]
    options         = item["options"]            # dict {A: text, B: text, ...}
    seth_answer     = item["seth_answer"]        # "A" | "B" | "C" | "D"
    # Per-option score: per-token mean log-probability under the model
    # given the prompt. Length-normalised so longer options aren't penalised.
    scores = { letter: logprob_sum / max(n_tokens, 1)
               for letter, (logprob_sum, n_tokens) in option_scores.items() }
    pred = argmax(scores)          # highest mean-LL (closest to 0) wins
    correct += (pred == seth_answer)

accuracy = correct / n_questions
stderr   = sqrt(p * (1 - p) / n_questions)   # binomial standard error

We run the full pass twice — once for base, once for adapter — and emit:
    delta_accuracy = adapter_accuracy - base_accuracy

Gate decision
-------------
PASS if:
  - adapter_accuracy ≥ ACCURACY_PASS_THRESHOLD (0.65 — Twin-2K-500 §5
    reports 0.6-0.7 for personalized models vs 0.5 random), AND
  - delta_accuracy > 0 (adapter beats base; "no improvement" doesn't pass)

FAIL otherwise. This mirrors yntp_eval's two-condition gate.

AC-11.10.7 regression guard
---------------------------
The Sprint 8 broken adapter (pad-token gibberish) cannot pick coherent
options at better than chance — its predictive distribution is dominated
by `<pad>`. The mock fixture `tests/fixtures/sprint8_broken_twin2k_log.jsonl`
simulates this by giving the adapter near-uniform log-probs across all
options (no signal → ~0.5 accuracy → FAIL).

Tier policy (mirrors yntp_eval D1 hybrid)
-----------------------------------------
  1. `HU_TWIN2K_HOLDOUT` env var if set: production path (Seth's machine).
  2. `--fixture <path>` flag: explicit override.
  3. `tests/fixtures/twin2k_synthetic_10q.jsonl`: CI fallback.

Honest-scope notice
-------------------
This module does NOT call MLX inference. The real Gemma-4 forced-choice
scoring path is gated behind `_real_compute_logprob` which raises
NotImplementedError pointing to the M3 frontier-model-bridge plan. CI
runs use `--mock-from-jsonl` which reads pre-recorded per-option log-prob
sums from a JSONL log.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple


# ── Schema + constants ────────────────────────────────────────────────────

# Required keys on every fixture row.
_FIXTURE_REQUIRED_KEYS = ("prompt", "options", "seth_answer")

# Valid option letters (we support 2-letter to 6-letter forced-choice).
_VALID_OPTION_LETTERS = ("A", "B", "C", "D", "E", "F")

# Accuracy floor for the PASS gate. Per Twin-2K-500 §5, personalized models
# score 0.6-0.7 on behavioral forced-choice; 0.5 is random. We pick 0.65 as
# "demonstrably above random, conservative for n=10 fixture stderr".
ACCURACY_PASS_THRESHOLD = 0.65

# Tiny epsilon for length-normalisation.
_EPS = 1e-9

# Synthetic fixture path is repo-relative.
_REPO_ROOT = Path(__file__).resolve().parent.parent
_SYNTHETIC_FIXTURE = _REPO_ROOT / "tests" / "fixtures" / "twin2k_synthetic_10q.jsonl"


# ── Data classes ──────────────────────────────────────────────────────────


@dataclass
class ForcedChoiceRow:
    """One held-out forced-choice item."""

    prompt: str
    options: Dict[str, str]   # {"A": text, "B": text, ...}
    seth_answer: str          # "A" | "B" | ... must be a key in options
    metadata: dict
    row_id: int               # ordinal in the file


@dataclass
class TwinEvalResult:
    """Output of a full Twin-2K-500 forced-choice evaluation pass.

    Shape mirrors AC-11.10.1 (n_questions, adapter_accuracy, base_accuracy,
    delta_accuracy, stderr) plus the gate decision + provenance fields.
    """

    n_questions: int
    n_correct_adapter: int
    n_correct_base: int
    adapter_accuracy: float
    base_accuracy: float
    delta_accuracy: float
    stderr: float
    fixture_path: str
    gate_decision: str   # "PASS" or "FAIL"
    notes: List[str]


# ── Fixture loading + schema validation (AC-11.10.4) ──────────────────────


def resolve_fixture(explicit: Optional[str]) -> Path:
    """Pick the fixture file per the D1-style hybrid policy.

    Precedence:
      1. `--fixture <path>` flag (explicit override)
      2. `HU_TWIN2K_HOLDOUT` env var (production path)
      3. tests/fixtures/twin2k_synthetic_10q.jsonl (CI fallback)
    """
    if explicit:
        return Path(explicit).expanduser()
    env_path = os.environ.get("HU_TWIN2K_HOLDOUT")
    if env_path:
        return Path(env_path).expanduser()
    return _SYNTHETIC_FIXTURE


def _validate_row_schema(obj: dict, path: Path, line_no: int) -> None:
    """Raise ValueError if `obj` is not a well-formed Twin-2K-500 row.

    Schema:
      - "prompt": non-empty string
      - "options": dict with 2-6 entries, each key in {A..F}, each value
        a non-empty string
      - "seth_answer": single uppercase letter that IS a key in options
    """
    for key in _FIXTURE_REQUIRED_KEYS:
        if key not in obj:
            raise ValueError(
                f"{path}:{line_no}: missing required key '{key}'"
            )

    if not isinstance(obj["prompt"], str) or not obj["prompt"].strip():
        raise ValueError(
            f"{path}:{line_no}: key 'prompt' must be non-empty string"
        )

    options = obj["options"]
    if not isinstance(options, dict) or not options:
        raise ValueError(
            f"{path}:{line_no}: key 'options' must be non-empty dict"
        )
    if len(options) < 2 or len(options) > 6:
        raise ValueError(
            f"{path}:{line_no}: 'options' must have 2-6 entries, got {len(options)}"
        )
    for letter, text in options.items():
        if letter not in _VALID_OPTION_LETTERS:
            raise ValueError(
                f"{path}:{line_no}: option key '{letter}' not in {_VALID_OPTION_LETTERS}"
            )
        if not isinstance(text, str) or not text.strip():
            raise ValueError(
                f"{path}:{line_no}: option '{letter}' must be non-empty string"
            )

    answer = obj["seth_answer"]
    if not isinstance(answer, str) or answer not in options:
        raise ValueError(
            f"{path}:{line_no}: 'seth_answer'={answer!r} is not one of "
            f"the option letters {sorted(options.keys())}"
        )


def load_fixture(path: Path) -> List[ForcedChoiceRow]:
    """Load + validate a JSONL Twin-2K-500 fixture.

    Raises ValueError on schema violations with a line number; raises
    FileNotFoundError if the path does not exist.
    """
    if not path.exists():
        raise FileNotFoundError(f"Twin-2K-500 fixture not found: {path}")

    rows: List[ForcedChoiceRow] = []
    with path.open("r", encoding="utf-8") as fp:
        for line_no, raw in enumerate(fp, start=1):
            raw = raw.strip()
            if not raw:
                continue
            try:
                obj = json.loads(raw)
            except json.JSONDecodeError as exc:
                raise ValueError(
                    f"{path}:{line_no}: invalid JSON ({exc})"
                ) from exc
            _validate_row_schema(obj, path, line_no)
            rows.append(
                ForcedChoiceRow(
                    prompt=obj["prompt"],
                    options=dict(obj["options"]),
                    seth_answer=obj["seth_answer"],
                    metadata=obj.get("metadata", {}),
                    row_id=len(rows) + 1,
                )
            )
    if not rows:
        raise ValueError(f"{path}: fixture is empty")
    return rows


# ── Mock scoring backend (CI path) ────────────────────────────────────────


def load_mock_log(path: Path) -> List[dict]:
    """Load a pre-recorded mock log (JSONL) of per-option log-probs.

    Each entry has shape:
        {
          "row_id": int,
          "base_option_scores": {"A": [logprob_sum, n_tokens], "B": ...},
          "adapter_option_scores": {"A": [logprob_sum, n_tokens], ...},
          "note": str (optional)
        }

    Used by AC-11.10.7 to simulate the Sprint 8 broken-adapter regression:
    the broken adapter gives near-uniform option scores (no signal → ~0.5).
    """
    out: List[dict] = []
    with path.open("r", encoding="utf-8") as fp:
        for raw in fp:
            raw = raw.strip()
            if not raw:
                continue
            out.append(json.loads(raw))
    return out


# ── Real scoring backend (production path; gated import) ──────────────────


def _real_compute_logprob(
    model_id: str,
    adapter_path: Optional[str],
    rows: Sequence[ForcedChoiceRow],
) -> List[Dict[str, Tuple[float, int]]]:
    """Real MLX forced-choice scoring path. Gated import.

    Returns, per fixture row, a dict mapping each option letter to a
    `(logprob_sum, n_tokens)` pair under the conditional
    `p(option_text | prompt)`.

    Out of scope for this story: actual Gemma-4 inference. The bridge plan
    is in docs/plans/2026-05-10-m3-frontier-model-bridge.md.
    """
    try:
        import mlx_lm  # noqa: F401  (proves importability)
    except ImportError as exc:
        raise RuntimeError(
            "Real MLX inference path not available in this environment: "
            f"{exc}. Use --mock-from-jsonl <path> for CI/test runs, or see "
            "docs/plans/2026-05-10-m3-frontier-model-bridge.md for the "
            "Gemma-4 inference bridge plan."
        ) from exc

    raise NotImplementedError(
        "Real MLX forced-choice scoring is wired but the per-option "
        "teacher-forced loop is deferred to the M3 frontier-model-bridge "
        "plan. For now, pass --mock-from-jsonl to exercise the aggregation "
        "+ gate logic on synthetic fixtures."
    )


# ── Forced-choice scorer (pure function; AC-11.10.2) ──────────────────────


def score_forced_choice(
    option_scores: Dict[str, Tuple[float, int]],
) -> str:
    """Return the argmax option letter under length-normalised mean log-prob.

    `option_scores` maps letter → (logprob_sum, n_tokens). We pick the option
    with the highest mean LL (per-token), which is the option the model
    assigns the most probability mass to. Length-normalisation makes the
    score comparable across options of different lengths (Twin-2K-500 §4.2).

    Tie-break: deterministic — alphabetical order on the option letter.
    This matches MMLU's reference implementation and the design doc's
    OQ#3 recommendation (ties count as wrong only if the seth_answer is
    not first alphabetically; here we make tie-resolution deterministic
    so the test fixture can exercise every position).
    """
    if not option_scores:
        raise ValueError("score_forced_choice: empty option_scores")

    best_letter: Optional[str] = None
    best_score: float = -math.inf
    for letter in sorted(option_scores.keys()):
        logprob_sum, n_tokens = option_scores[letter]
        score = logprob_sum / max(n_tokens, _EPS)
        if score > best_score:
            best_score = score
            best_letter = letter
    assert best_letter is not None
    return best_letter


# ── Aggregation + gate ────────────────────────────────────────────────────


def compute_accuracy(
    rows: Sequence[ForcedChoiceRow],
    per_row_scores: Sequence[Dict[str, Tuple[float, int]]],
) -> Tuple[int, float]:
    """Return (n_correct, accuracy) over the held-out fixture."""
    if len(rows) != len(per_row_scores):
        raise ValueError(
            f"score-row count mismatch: {len(rows)} rows vs "
            f"{len(per_row_scores)} score entries"
        )
    n_correct = 0
    for row, scores in zip(rows, per_row_scores):
        pred = score_forced_choice(scores)
        if pred == row.seth_answer:
            n_correct += 1
    accuracy = n_correct / len(rows) if rows else 0.0
    return n_correct, accuracy


def binomial_stderr(p: float, n: int) -> float:
    """Standard binomial standard error: sqrt(p*(1-p)/n).

    For n=10, p=0.5 → 0.158; n=50 → 0.071. The design doc cites these.
    """
    if n <= 0:
        return 0.0
    p = max(0.0, min(1.0, p))
    return math.sqrt(p * (1 - p) / n)


def decide_gate(
    adapter_accuracy: float,
    delta_accuracy: float,
) -> Tuple[str, List[str]]:
    """Decide PASS/FAIL for the Twin-2K-500 forced-choice metric.

    Rules:
      - FAIL if adapter_accuracy < ACCURACY_PASS_THRESHOLD (0.65)
        Rationale: anything at or near 0.5 is random; 0.65 is the floor
        Twin-2K-500 §5 reports for personalized models.
      - FAIL if delta_accuracy <= 0 (adapter doesn't beat base)
        Rationale: an adapter that doesn't improve over base on
        forced-choice is not earning its compute cost.
      - PASS otherwise.

    Returns (decision, notes) with a human-readable explanation.
    """
    notes: List[str] = []
    if adapter_accuracy < ACCURACY_PASS_THRESHOLD:
        notes.append(
            f"adapter_accuracy={adapter_accuracy:.3f} < "
            f"{ACCURACY_PASS_THRESHOLD} (below personalized-model floor; "
            "AC-11.10.7 broken-adapter regression guard)"
        )
        return "FAIL", notes
    if delta_accuracy <= 0:
        notes.append(
            f"delta_accuracy={delta_accuracy:+.3f} <= 0 "
            "(adapter did not beat base on forced-choice)"
        )
        return "FAIL", notes
    notes.append(
        f"adapter_accuracy={adapter_accuracy:.3f} >= "
        f"{ACCURACY_PASS_THRESHOLD} and delta_accuracy="
        f"{delta_accuracy:+.3f} > 0"
    )
    return "PASS", notes


# ── Top-level pipeline ────────────────────────────────────────────────────


def _per_row_from_mock(
    rows: Sequence[ForcedChoiceRow],
    mock_entries: Sequence[dict],
    key: str,
) -> List[Dict[str, Tuple[float, int]]]:
    """Project mock log entries into the per-row option-score shape.

    `key` is `"base_option_scores"` or `"adapter_option_scores"`.
    Each option_scores value is expected as [logprob_sum, n_tokens].
    Raises ValueError if any row's mock entry is missing options the
    fixture defined (catches accidental mismatches early).
    """
    out: List[Dict[str, Tuple[float, int]]] = []
    for row, entry in zip(rows, mock_entries):
        raw = entry.get(key)
        if not isinstance(raw, dict):
            raise ValueError(
                f"mock log row {row.row_id}: missing '{key}' dict"
            )
        scores: Dict[str, Tuple[float, int]] = {}
        for letter in row.options:
            if letter not in raw:
                raise ValueError(
                    f"mock log row {row.row_id}: '{key}' missing option "
                    f"'{letter}' (fixture defines it)"
                )
            pair = raw[letter]
            if not (isinstance(pair, (list, tuple)) and len(pair) == 2):
                raise ValueError(
                    f"mock log row {row.row_id}: '{key}.{letter}' must "
                    "be [logprob_sum, n_tokens]"
                )
            scores[letter] = (float(pair[0]), int(pair[1]))
        out.append(scores)
    return out


def evaluate(
    fixture_path: Path,
    mock_log_path: Optional[Path],
    adapter_path: Optional[str],
    base_model: str,
) -> TwinEvalResult:
    """Run the full Twin-2K-500 forced-choice eval and return result."""
    rows = load_fixture(fixture_path)

    if mock_log_path is not None:
        mock_entries = load_mock_log(mock_log_path)
        # Strict equality, not <: same critic-MED fix as yntp_eval.evaluate
        # — zip silently truncates the longer iterable, which would let a
        # 10-row fixture paired with a 5-row mock log report a partial
        # result. The gate is supposed to refuse that.
        if len(mock_entries) != len(rows):
            raise ValueError(
                f"mock log has {len(mock_entries)} entries; fixture has "
                f"{len(rows)} rows. Counts must match exactly."
            )
        base_scores = _per_row_from_mock(rows, mock_entries, "base_option_scores")
        adapter_scores = _per_row_from_mock(
            rows, mock_entries, "adapter_option_scores"
        )
    else:
        # Real path. Will raise NotImplementedError unless the bridge ships.
        base_scores = _real_compute_logprob(base_model, None, rows)
        adapter_scores = _real_compute_logprob(base_model, adapter_path, rows)

    n_correct_base, base_acc = compute_accuracy(rows, base_scores)
    n_correct_adapter, adapter_acc = compute_accuracy(rows, adapter_scores)
    delta = adapter_acc - base_acc
    stderr = binomial_stderr(adapter_acc, len(rows))
    decision, notes = decide_gate(adapter_acc, delta)

    return TwinEvalResult(
        n_questions=len(rows),
        n_correct_adapter=n_correct_adapter,
        n_correct_base=n_correct_base,
        adapter_accuracy=adapter_acc,
        base_accuracy=base_acc,
        delta_accuracy=delta,
        stderr=stderr,
        fixture_path=str(fixture_path),
        gate_decision=decision,
        notes=notes,
    )


# ── CLI ───────────────────────────────────────────────────────────────────


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="twin_eval.py",
        description=(
            "Sprint 11 / US-11.10 — Twin-2K-500 forced-choice held-out "
            "evaluator. Computes adapter and base accuracy on a held-out "
            "set of behavioral forced-choice questions and reports the "
            "delta with binomial standard error. Secondary metric to YNTP."
        ),
    )
    p.add_argument(
        "--adapter",
        type=str,
        default=None,
        help="Path to adapter (real MLX path). If omitted, requires "
        "--mock-from-jsonl for the CI/test path.",
    )
    p.add_argument(
        "--base-model",
        type=str,
        default="gemma-4-e2b",
        help="Base model id (default: gemma-4-e2b). Only used on the real "
        "MLX path.",
    )
    p.add_argument(
        "--protocol",
        type=str,
        default="forced-choice",
        choices=["forced-choice"],
        help="Evaluation protocol. Currently only 'forced-choice' is "
        "supported (Twin-2K-500 §4.2 MMLU-style).",
    )
    p.add_argument(
        "--fixture",
        type=str,
        default=None,
        help="Override the fixture path. Default precedence: "
        "$HU_TWIN2K_HOLDOUT -> tests/fixtures/twin2k_synthetic_10q.jsonl.",
    )
    p.add_argument(
        "--mock-from-jsonl",
        type=str,
        default=None,
        help="Read pre-recorded per-option log-prob sums from this JSONL "
        "(CI/test path). When set, bypasses MLX entirely.",
    )
    p.add_argument(
        "--output",
        type=str,
        default=None,
        help="Optional path to write the result JSON. Default: stdout.",
    )
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)

    fixture_path = resolve_fixture(args.fixture)
    mock_path = (
        Path(args.mock_from_jsonl).expanduser() if args.mock_from_jsonl else None
    )

    if mock_path is None and args.adapter is None:
        parser.error(
            "must specify either --adapter (real MLX path) or "
            "--mock-from-jsonl (CI/test path)"
        )

    try:
        result = evaluate(
            fixture_path=fixture_path,
            mock_log_path=mock_path,
            adapter_path=args.adapter,
            base_model=args.base_model,
        )
    except (FileNotFoundError, ValueError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 2
    except RuntimeError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 3
    except NotImplementedError as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 4

    payload = json.dumps(asdict(result), indent=2, sort_keys=True)
    if args.output:
        Path(args.output).expanduser().write_text(payload + "\n", encoding="utf-8")
    else:
        print(payload)
    return 0 if result.gate_decision == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
