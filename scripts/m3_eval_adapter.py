#!/usr/bin/env python3
"""
Phase C5 — A/B eval harness for M3 adapters.

Compares a CANDIDATE adapter (just produced by the M3 loop) against a
BASELINE (the last known-good). Asks: did the new training meaningfully
improve persona fidelity, or did it regress, or no-change?

Pluggable judge interface — the deliverable is the HARNESS plus one
deterministic judge. An LLM-based judge (the eventual production gate)
drops in by implementing the same `Judge` protocol.

  - 'metadata' (default, deterministic)
    Inspects the adapter file headers. Verdict shape:
      - file format valid (parses as LoRA bin or safetensors)
      - rank, max_steps, trained params count
      - newer adapter with same-or-larger structure → tentative pass
    This is not a true persona-fidelity test — it's the floor: we
    confirm a real adapter was produced. Useful as a CI smoke gate.

  - 'sft-prompts' (requires live inference server, optional)
    Loads a JSONL of held-out test prompts with expected style markers,
    runs inference against an MLX server with each adapter loaded,
    counts marker matches. Verdict = candidate marker count vs baseline.
    Skipped (with exit 0) if the inference server isn't reachable —
    same soft-fail pattern as scripts/test_mlx_adapter_swap.py.

The verdict is a structured record so downstream tooling (the auto-promote
step in training_loop.py's full cycle, or a future trainer dashboard) can
consume it without re-implementing the comparison.

Usage:
    python3 scripts/m3_eval_adapter.py \\
        --baseline ~/.human/training-data/adapters/seth-lora-current \\
        --candidate ~/.human/training-data/adapters/m3-driver-X.bin \\
        --judge metadata

Exit codes:
    0 — verdict produced (regardless of pass/fail/no-change)
    2 — input error (file missing, parse failure)
"""
from __future__ import annotations

import argparse
import json
import os
import struct
import sys
import urllib.error
import urllib.request
from pathlib import Path
from typing import Protocol


# ─────────────────────────────────────────────────────────────────────
# Adapter inspectors — read metadata from both supported formats
# ─────────────────────────────────────────────────────────────────────

LORA_MAGIC = b"LORA"          # hu_ml_checkpoint magic (lora-persona output)
SAFETENSORS_MIN_HEADER = 8     # 8-byte LE length prefix


def inspect_lora_binary(path: Path) -> dict | None:
    """Inspect a `lora-persona`-produced binary. Format (from
    src/ml/checkpoint.c):
      4 bytes: magic "LORA"
      4 bytes: rank (uint32 LE)
      8 bytes: learning_rate (double LE)
      4 bytes: num_layers (uint32 LE)
      ... layer-tensor data ...

    Returns None if not a LoRA binary (caller may try safetensors)."""
    try:
        with open(path, "rb") as f:
            magic = f.read(4)
            if magic != LORA_MAGIC:
                return None
            rank = struct.unpack("<I", f.read(4))[0]
            lr = struct.unpack("<d", f.read(8))[0]
            num_layers = struct.unpack("<I", f.read(4))[0]
        return {
            "format": "lora-bin",
            "path": str(path),
            "size_bytes": path.stat().st_size,
            "rank": rank,
            "learning_rate": lr,
            "num_layers": num_layers,
        }
    except (OSError, struct.error):
        return None


def inspect_safetensors(path: Path) -> dict | None:
    """Inspect a safetensors-shaped file (8-byte LE length + JSON header).
    Returns None on parse failure."""
    try:
        with open(path, "rb") as f:
            header_len_bytes = f.read(SAFETENSORS_MIN_HEADER)
            if len(header_len_bytes) < SAFETENSORS_MIN_HEADER:
                return None
            header_len = struct.unpack("<Q", header_len_bytes)[0]
            if header_len > 16_000_000:  # 16 MB sanity cap
                return None
            header = json.loads(f.read(header_len).decode("utf-8"))
        tensor_count = sum(1 for k in header if not k.startswith("__"))
        meta = header.get("__metadata__", {})
        return {
            "format": "safetensors",
            "path": str(path),
            "size_bytes": path.stat().st_size,
            "tensor_count": tensor_count,
            "metadata": meta,
            "outcome_count": int(meta.get("outcome_count", 0)) if meta else 0,
        }
    except (OSError, struct.error, ValueError, json.JSONDecodeError):
        return None


def inspect_adapter(path: Path) -> dict | None:
    """Try both formats. Returns None if neither parses."""
    if not path.exists():
        return None
    return inspect_lora_binary(path) or inspect_safetensors(path)


# ─────────────────────────────────────────────────────────────────────
# Judge protocol + implementations
# ─────────────────────────────────────────────────────────────────────

class Judge(Protocol):
    """A judge takes two adapter metadata dicts and produces a verdict."""

    def evaluate(self, baseline: dict, candidate: dict) -> dict: ...


class MetadataJudge:
    """Deterministic, no-inference-server-required judge. The verdict
    is intentionally CONSERVATIVE — without running real inference we
    can only attest to: 'a real adapter was produced and its structural
    parameters meet or exceed baseline.'

    Outputs:
      pass — candidate is well-formed AND has matching-or-better
             rank/tensors than baseline
      no-change — candidate matches baseline exactly (deterministic
                   re-training produced the same file)
      regress — candidate has fewer tensors or smaller rank
      fail   — candidate failed to parse OR is the empty-tensors stub

    This is the floor of A/B eval. Real persona-fidelity scoring
    requires the sft-prompts judge below."""

    def evaluate(self, baseline: dict, candidate: dict) -> dict:
        # First contract: candidate must be a REAL adapter, not the
        # empty-tensors stub that C3's fallback path emits.
        if candidate["format"] == "safetensors":
            if candidate.get("tensor_count", 0) == 0:
                return {
                    "judge": "metadata",
                    "verdict": "fail",
                    "reason": "candidate is empty-tensors safetensors "
                              "(no real training happened)",
                    "baseline": baseline,
                    "candidate": candidate,
                }

        # Compare structural params when both are lora-bin format
        if baseline["format"] == "lora-bin" and candidate["format"] == "lora-bin":
            if candidate["rank"] < baseline["rank"]:
                return {
                    "judge": "metadata",
                    "verdict": "regress",
                    "reason": f"rank dropped {baseline['rank']} → {candidate['rank']}",
                    "baseline": baseline, "candidate": candidate,
                }
            if (candidate["rank"] == baseline["rank"]
                    and candidate["num_layers"] == baseline["num_layers"]
                    and candidate["size_bytes"] == baseline["size_bytes"]):
                return {
                    "judge": "metadata",
                    "verdict": "no-change",
                    "reason": "identical structure + file size — "
                              "either deterministic re-train or stale candidate",
                    "baseline": baseline, "candidate": candidate,
                }
            return {
                "judge": "metadata",
                "verdict": "pass",
                "reason": f"candidate rank={candidate['rank']} layers={candidate['num_layers']}",
                "baseline": baseline, "candidate": candidate,
            }

        # Mixed formats — treat as pass if candidate is a real lora-bin
        # (baseline being something else means we're upgrading from
        # empty-tensors/scaffolding to real training).
        if candidate["format"] == "lora-bin":
            return {
                "judge": "metadata",
                "verdict": "pass",
                "reason": (f"candidate is real LoRA ({candidate['size_bytes']}B), "
                           f"baseline format={baseline['format']}"),
                "baseline": baseline, "candidate": candidate,
            }

        return {
            "judge": "metadata",
            "verdict": "no-change",
            "reason": "both adapters are non-trained shapes",
            "baseline": baseline, "candidate": candidate,
        }


class SftPromptsJudge:
    """Live-inference judge. Loads eval prompts from JSONL, runs each
    against an MLX server with both adapters swapped in, scores responses
    by expected style markers, reports the marker-match delta.

    Skipped with exit 0 if the MLX server isn't reachable — same pattern
    as scripts/test_mlx_adapter_swap.py."""

    def __init__(self, mlx_url: str, prompts_jsonl: Path):
        self.mlx_url = mlx_url
        self.prompts_jsonl = prompts_jsonl

    def _server_alive(self) -> bool:
        try:
            with urllib.request.urlopen(f"{self.mlx_url}/health", timeout=2):
                return True
        except (urllib.error.URLError, OSError):
            return False

    def _score_adapter(self, adapter_path: str, prompts: list[dict]) -> tuple[int, int]:
        """Swap to adapter, run each prompt, count marker matches.
        Returns (matched, total). Resilient to per-prompt failures."""
        # Swap to this adapter
        swap_req = urllib.request.Request(
            f"{self.mlx_url}/v1/adapters/swap",
            data=json.dumps({"adapter_path": adapter_path}).encode(),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            urllib.request.urlopen(swap_req, timeout=30)
        except urllib.error.URLError:
            return 0, len(prompts)

        matched = 0
        for p in prompts:
            user = p.get("prompt", "")
            expected_markers = p.get("expected_markers", [])
            req = urllib.request.Request(
                f"{self.mlx_url}/v1/chat/completions",
                data=json.dumps({
                    "model": "mlx_local",
                    "messages": [{"role": "user", "content": user}],
                    "max_tokens": 50,
                }).encode(),
                headers={"Content-Type": "application/json"},
                method="POST",
            )
            try:
                with urllib.request.urlopen(req, timeout=30) as resp:
                    body = json.loads(resp.read().decode())
                text = body.get("choices", [{}])[0].get("message", {}).get("content", "").lower()
            except (urllib.error.URLError, json.JSONDecodeError, KeyError):
                continue
            if any(m.lower() in text for m in expected_markers):
                matched += 1
        return matched, len(prompts)

    def evaluate(self, baseline: dict, candidate: dict) -> dict:
        if not self._server_alive():
            return {
                "judge": "sft-prompts",
                "verdict": "skipped",
                "reason": f"MLX server not reachable at {self.mlx_url}",
                "baseline": baseline, "candidate": candidate,
            }
        if not self.prompts_jsonl.exists():
            return {
                "judge": "sft-prompts", "verdict": "skipped",
                "reason": f"prompts JSONL not found: {self.prompts_jsonl}",
                "baseline": baseline, "candidate": candidate,
            }
        prompts = [json.loads(l) for l in self.prompts_jsonl.read_text().splitlines() if l.strip()]
        b_matched, total = self._score_adapter(baseline["path"], prompts)
        c_matched, _ = self._score_adapter(candidate["path"], prompts)
        delta = c_matched - b_matched
        verdict = "pass" if delta > 0 else ("regress" if delta < 0 else "no-change")
        return {
            "judge": "sft-prompts",
            "verdict": verdict,
            "reason": (f"baseline {b_matched}/{total} markers, "
                       f"candidate {c_matched}/{total} markers (delta={delta:+d})"),
            "baseline": baseline, "candidate": candidate,
            "baseline_score": b_matched, "candidate_score": c_matched,
            "total_prompts": total,
        }


# ─────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--baseline", type=Path, required=True,
                    help="Path to the BASELINE adapter (existing known-good)")
    ap.add_argument("--candidate", type=Path, required=True,
                    help="Path to the CANDIDATE adapter (newly trained)")
    ap.add_argument("--judge", choices=["metadata", "sft-prompts"], default="metadata",
                    help="Judge implementation (default: metadata)")
    ap.add_argument("--prompts-jsonl", type=Path,
                    default=Path(__file__).resolve().parent.parent
                    / "eval_suites" / "m3-personalization" / "prompts.jsonl",
                    help="Held-out prompts for sft-prompts judge")
    ap.add_argument("--mlx-url", default=os.environ.get("HUMAN_MLX_URL",
                                                         "http://127.0.0.1:8741"),
                    help="MLX server URL for sft-prompts judge")
    ap.add_argument("--json-out", type=Path,
                    help="Write verdict as JSON to this path (default: stdout only)")
    args = ap.parse_args()

    baseline_meta = inspect_adapter(args.baseline)
    if baseline_meta is None:
        print(f"ERROR: baseline {args.baseline} not found or not parseable", file=sys.stderr)
        sys.exit(2)
    candidate_meta = inspect_adapter(args.candidate)
    if candidate_meta is None:
        print(f"ERROR: candidate {args.candidate} not found or not parseable", file=sys.stderr)
        sys.exit(2)

    judge: Judge
    if args.judge == "metadata":
        judge = MetadataJudge()
    else:
        judge = SftPromptsJudge(args.mlx_url, args.prompts_jsonl)

    verdict = judge.evaluate(baseline_meta, candidate_meta)

    print(f"\n{'='*60}")
    print(f"  M3 ADAPTER A/B EVAL (judge={verdict['judge']})")
    print(f"{'='*60}")
    print(f"  Baseline:  {baseline_meta['path']} "
          f"({baseline_meta.get('format')}, {baseline_meta.get('size_bytes')}B)")
    print(f"  Candidate: {candidate_meta['path']} "
          f"({candidate_meta.get('format')}, {candidate_meta.get('size_bytes')}B)")
    print(f"  Verdict:   {verdict['verdict'].upper()}")
    print(f"  Reason:    {verdict['reason']}")
    print(f"{'='*60}")

    if args.json_out:
        args.json_out.parent.mkdir(parents=True, exist_ok=True)
        args.json_out.write_text(json.dumps(verdict, indent=2, default=str))
        print(f"  Verdict JSON: {args.json_out}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
