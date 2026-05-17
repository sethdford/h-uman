#!/usr/bin/env python3
"""Sprint 11 / US-11.6 — Held-out next-utterance log-likelihood evaluator.

YNTP-100-style protocol (arXiv 2510.14398) + TwinVoice (arXiv 2510.25536),
scoped to n=1 user (Seth) with a small held-out set of (prompt, continuation)
pairs from real chat data.

The metric: for each held-out pair, compute log P(true_continuation | prompt)
under both the base+system-prompt baseline and the persona-adapted model.
Average across the holdout. `delta_ll = adapter_mean_ll - base_mean_ll`;
positive means the adapter assigns higher probability to the user's real
next words — i.e. it's actually predicting *Seth*, not just emitting any
fluent text.

This metric is UNGAMEABLE by lexical-style fingerprints because:

  - A pad-spamming adapter cannot get high log-likelihood on coherent
    real continuations — its predictive distribution puts mass on `<pad>`,
    not on the true tokens.
  - A "wrote like Seth in topic distribution but not in surface form"
    proxy cannot win — the metric is token-level scoring, not bag-of-words.

This is the replacement for Sprint 7/8's gameable lexical fingerprint that
let a 40% pad-leakage adapter "win" by +0.019 (Sprint 8 smoke-run #3, see
`sprints/sprint-8/SMOKE_RUN_NOTES.md`).

Honest-scope notice
-------------------
This module does NOT itself call `mlx_lm` on a real Gemma-4 model in CI.
The real MLX inference path is intentionally kept behind a single function
(`compute_logprob`) that:

  - In tests (or when --mock-from-jsonl is passed) reads pre-recorded
    log-probability sums from a JSONL log, simulating the model's output.
  - In production (when --adapter is a real path and --mock-from-jsonl is
    not passed) attempts to import `mlx_lm` and run teacher-forced
    scoring; if the import fails, it errors out cleanly with a message
    pointing to the bridge plan (docs/plans/2026-05-10-m3-frontier-model-bridge.md).

The mock seam exists for two reasons:

  1. CI doesn't have a Gemma-4 model loaded; we still need to verify the
     aggregation, comparison, gate decision, and AC-11.6.3 regression
     guard logic end-to-end.
  2. The Sprint 8 broken adapter doesn't exist in this worktree; the
     regression guard test needs to simulate its low-likelihood output
     via a fixture (tests/fixtures/sprint8_broken_yntp_log.jsonl).

Fixture-tier policy (decisions.md D1, BINDING)
----------------------------------------------
Three tiers, picked by precedence:

  1. `HU_YNTP_HOLDOUT` env var if set: production path, points to
     `~/.human/private/yntp_holdout_30.jsonl` on Seth's machine.
  2. `--fixture` flag if passed: explicit override for testing/manual runs.
  3. `tests/fixtures/yntp_synthetic_5.jsonl`: CI/clean-checkout default.

The git-ignored tier is enforced by `.gitignore` plus a pre-commit check
(see `scripts/check_no_yntp_holdout_staged.sh`).
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sys
from dataclasses import dataclass, asdict
from pathlib import Path
from typing import Any, Iterable, List, Optional, Sequence, Tuple


# ── Schema + constants ────────────────────────────────────────────────────

# Required keys on every fixture row.
_FIXTURE_REQUIRED_KEYS = ("prompt", "continuation")

# Pad-rate threshold above which the gate fails regardless of NLL delta.
# Matches Sprint 8 broken-adapter signature (40-80% pad leakage).
PAD_RATE_FAIL_THRESHOLD = 0.5

# Tiny epsilon to keep mean computation stable when all rows are skipped.
_EPS = 1e-12

# Synthetic fixture path is repo-relative.
_REPO_ROOT = Path(__file__).resolve().parent.parent
_SYNTHETIC_FIXTURE = _REPO_ROOT / "tests" / "fixtures" / "yntp_synthetic_5.jsonl"


# ── Data classes ──────────────────────────────────────────────────────────


@dataclass
class HoldoutRow:
    """One held-out (prompt, continuation) pair."""

    prompt: str
    continuation: str
    metadata: dict
    row_id: int  # ordinal in the file, for deterministic ordering


@dataclass
class YntpResult:
    """Output of a full YNTP evaluation pass.

    Keys are deliberately stable; downstream tools (pareto_picker, dashboard)
    consume this shape.
    """

    base_mean_ll: float
    adapter_mean_ll: float
    delta_ll: float
    n_pairs: int
    pad_rate: float
    fixture_path: str
    gate_decision: str  # "PASS" or "FAIL"
    notes: List[str]


# ── Fixture loading ───────────────────────────────────────────────────────


def resolve_fixture(explicit: Optional[str]) -> Path:
    """Pick the fixture file per the binding D1 hybrid policy.

    Precedence:
      1. `--fixture <path>` flag (explicit override)
      2. `HU_YNTP_HOLDOUT` env var (production path)
      3. tests/fixtures/yntp_synthetic_5.jsonl (CI fallback)
    """
    if explicit:
        return Path(explicit).expanduser()
    env_path = os.environ.get("HU_YNTP_HOLDOUT")
    if env_path:
        return Path(env_path).expanduser()
    return _SYNTHETIC_FIXTURE


def load_fixture(path: Path) -> List[HoldoutRow]:
    """Load + validate a JSONL fixture file.

    Raises ValueError on schema violations. The schema is intentionally
    minimal: every row must have non-empty `prompt` and `continuation`
    strings; `metadata` is optional.
    """
    if not path.exists():
        raise FileNotFoundError(f"YNTP fixture not found: {path}")

    rows: List[HoldoutRow] = []
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
            for key in _FIXTURE_REQUIRED_KEYS:
                if key not in obj:
                    raise ValueError(
                        f"{path}:{line_no}: missing required key '{key}'"
                    )
                if not isinstance(obj[key], str) or not obj[key].strip():
                    raise ValueError(
                        f"{path}:{line_no}: key '{key}' must be non-empty string"
                    )
            rows.append(
                HoldoutRow(
                    prompt=obj["prompt"],
                    continuation=obj["continuation"],
                    metadata=obj.get("metadata", {}),
                    row_id=len(rows) + 1,
                )
            )
    if not rows:
        raise ValueError(f"{path}: fixture is empty")
    return rows


# ── Mock scoring backend (CI path) ────────────────────────────────────────


def load_mock_log(path: Path) -> List[dict]:
    """Load a pre-recorded mock log (JSONL).

    Each entry has shape:
        {
          "row_id": int,
          "base_logprob_sum": float,
          "base_n_tokens": int,
          "adapter_logprob_sum": float,
          "adapter_n_tokens": int,
          "adapter_generated": str,
          "has_pad": bool,
          ...
        }

    Used by AC-11.6.3 to simulate the Sprint 8 broken adapter's output.
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
    model_id: str, adapter_path: Optional[str], rows: Sequence[HoldoutRow]
) -> List[Tuple[float, int, bool]]:
    """Real MLX teacher-forced scoring path. Gated import.

    Returns a list of `(logprob_sum, n_continuation_tokens, has_pad)` per row.

    This is intentionally implemented as a thin wrapper that imports `mlx_lm`
    lazily so CI never hits the import. Out of scope for this story: actual
    Gemma-4 inference. The bridge plan is in
    docs/plans/2026-05-10-m3-frontier-model-bridge.md.
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

    # Implementation note: the actual teacher-forced scoring loop lives in
    # the §1.4 design doc. It is intentionally not implemented here because
    # this story's deliverable is the evaluator + gate logic; the inference
    # bridge is US-11.6 step 8, gated on the M3 frontier-model-bridge plan.
    raise NotImplementedError(
        "Real MLX scoring is wired but the per-row teacher-forced loop "
        "is deferred to US-11.6 step 8 (frontier-model bridge). For now, "
        "pass --mock-from-jsonl to exercise the aggregation + gate logic."
    )


# ── Aggregation + gate ────────────────────────────────────────────────────


def aggregate(
    per_row: Sequence[Tuple[float, int, bool]],
    base_or_adapter: str,
) -> Tuple[float, float]:
    """Aggregate per-row (logprob_sum, n_tokens, has_pad) -> (mean_ll, pad_rate).

    `mean_ll` is the per-token mean log-likelihood — sum of logprobs divided
    by total continuation tokens across the holdout. This is the standard
    YNTP-style aggregation; it makes rows with longer continuations weigh
    proportionally.

    `pad_rate` is the fraction of rows whose generated output contained a
    pad token.
    """
    if not per_row:
        return 0.0, 0.0
    total_lp = sum(r[0] for r in per_row)
    total_tok = sum(r[1] for r in per_row)
    if total_tok == 0:
        return 0.0, 0.0
    mean_ll = total_lp / max(total_tok, _EPS)
    pad_rate = sum(1 for r in per_row if r[2]) / len(per_row)
    return mean_ll, pad_rate


def decide_gate(
    base_mean_ll: float,
    adapter_mean_ll: float,
    adapter_pad_rate: float,
) -> Tuple[str, List[str]]:
    """Decide PASS/FAIL given mean LLs and adapter pad rate.

    Rules (per AC-11.6.3):
      - FAIL if `adapter_pad_rate >= PAD_RATE_FAIL_THRESHOLD` (regression guard)
      - FAIL if `adapter_mean_ll <= base_mean_ll` (no improvement)
      - PASS otherwise

    Returns (decision, notes) where notes is a human-readable list of
    reasons, useful in the evidence dump.
    """
    notes: List[str] = []
    if adapter_pad_rate >= PAD_RATE_FAIL_THRESHOLD:
        notes.append(
            f"pad_rate={adapter_pad_rate:.3f} >= {PAD_RATE_FAIL_THRESHOLD} "
            "(AC-11.6.3 regression guard)"
        )
        return "FAIL", notes
    if adapter_mean_ll <= base_mean_ll:
        notes.append(
            f"delta_ll={adapter_mean_ll - base_mean_ll:.4f} <= 0 "
            "(adapter did not improve held-out likelihood)"
        )
        return "FAIL", notes
    notes.append(
        f"delta_ll={adapter_mean_ll - base_mean_ll:.4f} > 0 "
        f"and pad_rate={adapter_pad_rate:.3f} < {PAD_RATE_FAIL_THRESHOLD}"
    )
    return "PASS", notes


# ── Top-level pipeline ────────────────────────────────────────────────────


def evaluate(
    fixture_path: Path,
    mock_log_path: Optional[Path],
    adapter_path: Optional[str],
    base_model: str,
) -> YntpResult:
    """Run the full YNTP eval pipeline and return a structured result."""
    rows = load_fixture(fixture_path)

    if mock_log_path is not None:
        mock_entries = load_mock_log(mock_log_path)
        if len(mock_entries) < len(rows):
            raise ValueError(
                f"mock log has {len(mock_entries)} entries; fixture has "
                f"{len(rows)} rows. Counts must match."
            )
        base_per_row: List[Tuple[float, int, bool]] = []
        adapter_per_row: List[Tuple[float, int, bool]] = []
        for row, entry in zip(rows, mock_entries):
            base_per_row.append(
                (
                    float(entry["base_logprob_sum"]),
                    int(entry["base_n_tokens"]),
                    False,  # base never has pad in our scenarios
                )
            )
            adapter_per_row.append(
                (
                    float(entry["adapter_logprob_sum"]),
                    int(entry["adapter_n_tokens"]),
                    bool(entry.get("has_pad", False)),
                )
            )
    else:
        # Real path. Will raise NotImplementedError unless the bridge ships.
        base_per_row = _real_compute_logprob(base_model, None, rows)
        adapter_per_row = _real_compute_logprob(base_model, adapter_path, rows)

    base_mean_ll, _ = aggregate(base_per_row, "base")
    adapter_mean_ll, adapter_pad_rate = aggregate(adapter_per_row, "adapter")
    decision, notes = decide_gate(base_mean_ll, adapter_mean_ll, adapter_pad_rate)

    return YntpResult(
        base_mean_ll=base_mean_ll,
        adapter_mean_ll=adapter_mean_ll,
        delta_ll=adapter_mean_ll - base_mean_ll,
        n_pairs=len(rows),
        pad_rate=adapter_pad_rate,
        fixture_path=str(fixture_path),
        gate_decision=decision,
        notes=notes,
    )


# ── CLI ───────────────────────────────────────────────────────────────────


def _build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="yntp_eval.py",
        description=(
            "Sprint 11 / US-11.6 — held-out next-utterance log-likelihood "
            "evaluator (YNTP-100 protocol). Compares an adapter against the "
            "base+system-prompt baseline on a held-out fixture of "
            "(prompt, continuation) pairs."
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
        "--fixture",
        type=str,
        default=None,
        help="Override the fixture path. Default precedence: "
        "$HU_YNTP_HOLDOUT -> tests/fixtures/yntp_synthetic_5.jsonl.",
    )
    p.add_argument(
        "--mock-from-jsonl",
        type=str,
        default=None,
        help="Read pre-recorded log-prob sums from this JSONL (CI/test path). "
        "When set, bypasses MLX entirely.",
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
    mock_path = Path(args.mock_from_jsonl).expanduser() if args.mock_from_jsonl else None

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
