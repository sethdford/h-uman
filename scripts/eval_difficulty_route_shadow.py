#!/usr/bin/env python3
"""
US-8: Difficulty-based routing SHADOW measurement.

Compares on-device (mlx_local+CONVERSATIONAL) vs cloud-shadow (ANALYTICAL)
for substantive CONVERSATIONAL turns, measuring humanness composite and
LUAR-MUD twin score (authorship-gap reuse).

Paired arms design: both arms receive identical contexts, run through their
respective model pipelines, and return replies scored on the same axes.
"""

import argparse
import json
import os
import sys
import hashlib
from typing import Optional, List, Dict, Tuple, Any
from dataclasses import dataclass, asdict


@dataclass
class PairedResult:
    """Result from one paired evaluation."""
    context_id: int
    on_device_humanness: Optional[float]
    cloud_shadow_humanness: Optional[float]
    on_device_twin: Optional[float]
    cloud_shadow_twin: Optional[float]
    on_device_recall_bytes: int
    cloud_shadow_recall_bytes: int


def select_contexts(corpus_path: str, n: int = 20, min_len: int = 50, max_len: int = 2000) -> List[str]:
    """
    Load n substantive contexts from eval corpus.

    Reuses eval_semantic_live_gate.py pattern: load from corpus, filter
    to len(text.split()) > 12 (HU_DIFFICULTY_ROUTE_SUBSTANTIVE_WORDS).
    """
    contexts = []
    if os.path.exists(corpus_path):
        try:
            with open(corpus_path, 'r') as f:
                for line in f:
                    if len(contexts) >= n:
                        break
                    try:
                        row = json.loads(line.strip())
                        msg = row.get("msg", "")
                        if min_len <= len(msg) <= max_len and len(msg.split()) > 12:
                            contexts.append(msg)
                    except (json.JSONDecodeError, ValueError):
                        continue
        except IOError:
            pass
    return contexts


def generate_on_device(msg: str, server_url: str = "http://127.0.0.1:8741") -> Tuple[Optional[str], int]:
    """
    Generate reply via on-device server (mlx_local).

    Returns (reply_text, recall_bytes) or (None, 0) on failure/timeout.
    """
    # Placeholder: in real implementation, POST to server_url with msg
    # For now, return stub for dry-run testing
    return None, 0


def generate_cloud_shadow(msg: str, project_id: str = "johnb-2025",
                        model: str = "gemini-3.1-pro-preview") -> Tuple[Optional[str], int]:
    """
    Generate reply via Vertex (ANALYTICAL treatment, thinking_budget=4096).

    Returns (reply_text, recall_bytes) or (None, 0) on failure/timeout.
    """
    # Placeholder: in real implementation, POST to Vertex API with
    # ADC bearer token, never ?key=, always thinkingConfig.thinkingBudget=4096
    # For now, return stub for dry-run testing
    return None, 0


def score_humanness_composite(reply: str) -> Optional[float]:
    """
    Score humanness composite via humanness_compose.compute_composite().

    Expects axes dict with keys like 'coherence', 'engagement', etc.
    """
    # Placeholder: real implementation calls the humanness scorer
    return None


def score_twin(reply: str, model: str = "mlx-community/LUAR-MUD") -> Optional[float]:
    """
    Score LUAR-MUD twin similarity vs Seth profile.

    Extracted from authorship_gap.py's twin_score() helper.
    """
    # Placeholder: real implementation loads LUAR and scores
    return None


def decide_gate(composite_on_device: Optional[float],
                composite_cloud: Optional[float],
                twin_on_device: Optional[float],
                twin_cloud: Optional[float],
                n_paired: int,
                tolerance: float = 0.02) -> Dict[str, Any]:
    """
    AC-8.4 gate: PROMOTE only if composite_cloud >= composite_on_device - tolerance
    AND twin_cloud >= twin_on_device (no tolerance).

    Otherwise HOLD and record exact numbers.
    """
    result = {
        "verdict": "INCONCLUSIVE",
        "reason": "insufficient paired results",
        "n_paired": n_paired,
    }

    if n_paired < 20:
        return result

    if composite_on_device is None or composite_cloud is None:
        result["reason"] = "missing composite scores"
        return result

    if twin_on_device is None or twin_cloud is None:
        result["reason"] = "missing twin scores"
        return result

    composite_delta = composite_cloud - composite_on_device
    twin_delta = twin_cloud - twin_on_device

    if composite_delta >= -tolerance and twin_delta >= 0:
        result["verdict"] = "PROMOTE"
    else:
        result["verdict"] = "HOLD"

    result["reason"] = f"composite_delta={composite_delta:.4f} (tol={tolerance}), twin_delta={twin_delta:.4f}"
    result["composite_on_device"] = composite_on_device
    result["composite_cloud"] = composite_cloud
    result["twin_on_device"] = twin_on_device
    result["twin_cloud"] = twin_cloud

    return result


def main():
    parser = argparse.ArgumentParser(description="US-8: Shadow difficulty routing measurement")
    parser.add_argument("--corpus", default="~/.human/eval-contexts-2026.jsonl",
                       help="Path to context corpus (default: ~/.human/eval-contexts-2026.jsonl)")
    parser.add_argument("-n", "--num-contexts", type=int, default=20,
                       help="Number of substantive contexts to sample (default: 20)")
    parser.add_argument("--server", default="http://127.0.0.1:8741",
                       help="URL of on-device server (default: http://127.0.0.1:8741)")
    parser.add_argument("--project-id", default="johnb-2025",
                       help="GCP project ID for Vertex (default: johnb-2025)")
    parser.add_argument("--model", default="gemini-3.1-pro-preview",
                       help="Vertex model (default: gemini-3.1-pro-preview)")
    parser.add_argument("--dry-run", action="store_true",
                       help="Dry-run: load contexts, report counts, no generation")
    parser.add_argument("--output", required=True,
                       help="Output JSON file (e.g., sprints/sprint-better-than-human-2026-09-05/evidence/US-8-shadow-route.json)")

    args = parser.parse_args()

    corpus_path = os.path.expanduser(args.corpus)

    # Load contexts
    contexts = select_contexts(corpus_path, n=args.num_contexts)

    if not contexts:
        print(f"ERROR: No contexts loaded from {corpus_path}", file=sys.stderr)
        sys.exit(1)

    print(f"Loaded {len(contexts)} substantive contexts", file=sys.stderr)

    if args.dry_run:
        # Dry-run: report counts, no generation
        result = {
            "verdict": "DRY_RUN",
            "reason": "dry-run mode: contexts loaded but no generation performed",
            "contexts_loaded": len(contexts),
            "min_words_threshold": 12,
        }
        os.makedirs(os.path.dirname(args.output), exist_ok=True)
        with open(args.output, 'w') as f:
            json.dump(result, f, indent=2)
        print(f"Dry-run report: {args.output}", file=sys.stderr)
        return 0

    # Real run: generate and score both arms
    # (Placeholder for now — full implementation requires server access)
    result = {
        "verdict": "INCONCLUSIVE",
        "reason": "real generation not implemented in stub",
        "contexts_attempted": len(contexts),
        "n_paired": 0,
    }

    os.makedirs(os.path.dirname(args.output), exist_ok=True)
    with open(args.output, 'w') as f:
        json.dump(result, f, indent=2)

    print(f"Evaluation complete: {args.output}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
