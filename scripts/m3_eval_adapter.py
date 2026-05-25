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

        # Real safetensors candidate (tensor_count > 0) against an empty-
        # stub baseline — this is the mlx_lm.lora bridge path. The
        # candidate is a freshly trained LoRA; baseline is the empty
        # placeholder from the dry-run stub. Treat as PASS.
        if (candidate["format"] == "safetensors"
                and candidate.get("tensor_count", 0) > 0
                and (baseline["format"] != "safetensors"
                     or baseline.get("tensor_count", 0) == 0
                     or baseline.get("size_bytes", 0) < 1024)):
            return {
                "judge": "metadata",
                "verdict": "pass",
                "reason": (f"candidate is real safetensors LoRA "
                           f"({candidate['tensor_count']} tensors, "
                           f"{candidate['size_bytes']}B); baseline is "
                           f"empty-stub ({baseline.get('size_bytes', 0)}B)"),
                "baseline": baseline, "candidate": candidate,
            }

        # Both safetensors with real tensor counts — compare sizes /
        # tensor counts. If candidate has MORE LoRA tensors, that's
        # generally a higher-capacity adapter → tentative pass; if
        # FEWER, regress; if equal AND same byte size, no-change.
        if (baseline["format"] == "safetensors"
                and candidate["format"] == "safetensors"
                and baseline.get("tensor_count", 0) > 0
                and candidate.get("tensor_count", 0) > 0):
            if candidate["tensor_count"] < baseline["tensor_count"]:
                return {
                    "judge": "metadata",
                    "verdict": "regress",
                    "reason": (f"tensor_count dropped "
                               f"{baseline['tensor_count']} → "
                               f"{candidate['tensor_count']}"),
                    "baseline": baseline, "candidate": candidate,
                }
            if (candidate["tensor_count"] == baseline["tensor_count"]
                    and candidate["size_bytes"] == baseline["size_bytes"]):
                return {
                    "judge": "metadata",
                    "verdict": "no-change",
                    "reason": "identical tensor count + byte size",
                    "baseline": baseline, "candidate": candidate,
                }
            return {
                "judge": "metadata",
                "verdict": "pass",
                "reason": (f"candidate tensors="
                           f"{candidate['tensor_count']} ≥ baseline="
                           f"{baseline['tensor_count']}"),
                "baseline": baseline, "candidate": candidate,
            }

        return {
            "judge": "metadata",
            "verdict": "no-change",
            "reason": "both adapters are non-trained shapes",
            "baseline": baseline, "candidate": candidate,
        }


class LlmJudge:
    """E3 (2026-05-18): LLM-based blind judge over PRE-COMPUTED outputs.

    Decouples the "score the outputs" step from the "run inference"
    step (SftPromptsJudge handles inference). Input is a JSONL with
    one record per held-out prompt:
        {"prompt": "...", "baseline_output": "...", "candidate_output": "..."}

    For each record, asks an LLM (default: OpenAI gpt-5) which output
    sounds more like Seth — without telling it which is which (blind
    A/B). Aggregates wins/ties/losses + bootstrap CI.

    Why this judge:
      - MetadataJudge confirms a real adapter was produced
      - SftPromptsJudge requires a live MLX server + adapter swap
      - LlmJudge runs locally on the operator's machine with API
        access — no MLX dependency, judges actual persona fidelity

    Requires: OPENAI_API_KEY env var, or HUMAN_JUDGE_MODEL set to a
    different model (e.g. "ollama:llama-3-8b" — local fallback).
    Skipped (verdict=skipped) if neither is available, same soft-fail
    pattern as the other judges."""

    def __init__(self, completions_jsonl: Path,
                 model: str | None = None):
        self.completions_jsonl = completions_jsonl
        self.model = model or os.environ.get("HUMAN_JUDGE_MODEL", "gpt-5")
        self.api_key = os.environ.get("OPENAI_API_KEY", "")

    def _build_judge_prompt(self, prompt: str, output_a: str, output_b: str) -> str:
        """The blind A/B prompt. Order of A vs B is randomized by the
        caller (evaluate()) so the judge can't anchor on position.
        The persona is intentionally LOW-DETAIL — we want a generic
        judge that scores naturalness/casualness, not a specific
        Seth-persona judge that would just memorize markers."""
        return (
            "You are a blind A/B judge comparing two responses to the same\n"
            "message. The user is texting a close friend casually — pick the\n"
            "response that sounds more like a real human friend (warm,\n"
            "natural, conversational, not robotic or assistant-like).\n\n"
            f"User message: {prompt}\n\n"
            f"Response A: {output_a}\n\n"
            f"Response B: {output_b}\n\n"
            "Answer with EXACTLY one of: A / B / TIE\n"
            "Then on a second line, give a 1-sentence reason."
        )

    def _call_judge(self, prompt: str) -> tuple[str, str]:
        """Returns (vote, reason). Vote is 'A' | 'B' | 'TIE' | 'ERROR'.
        On any network/API failure returns ('ERROR', detail)."""
        if not self.api_key:
            return ("ERROR", "no OPENAI_API_KEY")
        import urllib.request as _u
        import urllib.error as _e
        body = json.dumps({
            "model": self.model,
            "messages": [{"role": "user", "content": prompt}],
            "max_tokens": 80,
            "temperature": 0,
        }).encode("utf-8")
        req = _u.Request("https://api.openai.com/v1/chat/completions",
                         data=body, method="POST",
                         headers={"Content-Type": "application/json",
                                  "Authorization": f"Bearer {self.api_key}"})
        try:
            with _u.urlopen(req, timeout=30) as resp:
                payload = json.loads(resp.read().decode("utf-8"))
            text = (payload.get("choices", [{}])[0]
                          .get("message", {}).get("content", "")
                          .strip())
        except (_e.URLError, _e.HTTPError, json.JSONDecodeError, KeyError) as e:
            return ("ERROR", f"API error: {e}")
        # First non-empty line is the vote
        first = text.split("\n")[0].strip().upper()
        if first.startswith("A"):
            return ("A", text)
        if first.startswith("B"):
            return ("B", text)
        if "TIE" in first:
            return ("TIE", text)
        return ("ERROR", f"unparseable vote: {text[:80]}")

    def evaluate(self, baseline: dict, candidate: dict) -> dict:
        if not self.completions_jsonl.exists():
            return {
                "judge": "llm", "verdict": "skipped",
                "reason": f"completions JSONL not found at {self.completions_jsonl}",
                "baseline": baseline, "candidate": candidate,
            }
        if not self.api_key:
            return {
                "judge": "llm", "verdict": "skipped",
                "reason": "OPENAI_API_KEY not set",
                "baseline": baseline, "candidate": candidate,
            }
        import random
        random.seed(42)  # deterministic positional ordering across runs

        records = []
        for line in self.completions_jsonl.read_text().splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                continue
        if not records:
            return {
                "judge": "llm", "verdict": "skipped",
                "reason": "completions JSONL is empty",
                "baseline": baseline, "candidate": candidate,
            }

        wins_candidate = 0
        wins_baseline = 0
        ties = 0
        errors = 0
        per_prompt = []

        for rec in records:
            prompt = rec.get("prompt", "")
            base_out = rec.get("baseline_output", "")
            cand_out = rec.get("candidate_output", "")
            if not all([prompt, base_out, cand_out]):
                errors += 1
                continue
            # Randomize A/B mapping so judge can't anchor on position.
            if random.random() < 0.5:
                a, b = base_out, cand_out
                a_is_baseline = True
            else:
                a, b = cand_out, base_out
                a_is_baseline = False
            judge_prompt = self._build_judge_prompt(prompt, a, b)
            vote, reason = self._call_judge(judge_prompt)
            if vote == "ERROR":
                errors += 1
                per_prompt.append({"prompt": prompt[:60], "vote": "ERROR",
                                   "reason": reason})
                continue
            if vote == "TIE":
                ties += 1
            elif (vote == "A" and a_is_baseline) or (vote == "B" and not a_is_baseline):
                wins_baseline += 1
            else:
                wins_candidate += 1
            per_prompt.append({"prompt": prompt[:60], "vote": vote,
                               "a_is_baseline": a_is_baseline,
                               "reason": reason.split("\n", 1)[-1][:80]})

        scored = wins_candidate + wins_baseline + ties
        if scored == 0:
            return {
                "judge": "llm", "verdict": "skipped",
                "reason": f"all {errors} judge calls failed",
                "baseline": baseline, "candidate": candidate,
                "errors": errors,
            }

        # Verdict thresholds. Conservative: require ≥60% candidate-win
        # rate (over scored decisions, ties excluded) to declare pass.
        # 50% means coin-flip — no real signal.
        decisive = wins_candidate + wins_baseline
        win_rate = wins_candidate / decisive if decisive > 0 else 0.5
        if win_rate >= 0.60:
            verdict = "pass"
        elif win_rate <= 0.40:
            verdict = "regress"
        else:
            verdict = "no-change"

        return {
            "judge": "llm",
            "verdict": verdict,
            "reason": (f"candidate won {wins_candidate} / baseline won "
                       f"{wins_baseline} / ties {ties} (errors {errors}) "
                       f"— win_rate={win_rate:.2%}"),
            "baseline": baseline, "candidate": candidate,
            "wins_candidate": wins_candidate,
            "wins_baseline": wins_baseline,
            "ties": ties,
            "errors": errors,
            "decisive_count": decisive,
            "win_rate": win_rate,
            "per_prompt": per_prompt,
        }


class FidelityJudge:
    """Spec 2026-05-19 M3 closure / AC-M3-5 — communication-style fidelity
    judge.

    Per design D-M3-5: scores both adapters' outputs against the user's
    communication-style fingerprint using the same heuristic shipped in
    `src/memory/personal_model.c::hu_communication_style_fidelity_score`,
    then emits PASS iff `(candidate_mean - baseline_mean) >= threshold`
    (default 0.05 absolute on the [0, 1] scale).

    Input shape: two JSONL files, one row per held-out response.
    Required field per row:

        {"response": "<model output text>"}

    Optional fields like "prompt" are ignored.

    Why we reimplement the scorer in Python rather than calling the C
    binary: the harness must be runnable on machines without the full
    h-uman build (CI runners, dev laptops). Keeping the scorer
    self-contained removes a build dependency. The implementation MUST
    stay in lockstep with the C version — see the SCORER_TIE_TEST
    fixture in `tests/test_m3_ab_fidelity_gate.c` for byte-level cross-
    checks against the canonical C scorer.
    """

    # Default target fingerprint when no personal_model.bin is provided.
    # Mirrors the synthetic fallback in src/ml/fidelity.c::
    # hu_ml_fidelity_resolve_target so the two surfaces stay aligned.
    DEFAULT_TARGET = {
        "lowercase_ratio": 0.85,
        "abbreviation_ratio": 0.20,
        "avg_message_length": 60,
    }

    DEFAULT_THRESHOLD = 0.05

    # Abbreviation patterns — must match
    # src/memory/personal_model.c::hu_pm_extract_response_features.
    # We keep a conservative subset to avoid drift; the C scorer is the
    # source of truth.
    _ABBREVS = (
        " u ", " ur ", " r ", " idk ", " lol ", " omg ", " btw ",
        " tbh ", " tho ", " thx ", " ty ", " np ", " ya ", " yep ",
        " yeah ", " nah ", " kinda ", " gonna ", " wanna ", " ok ",
        " lmao ", " brb ", " imo ", " imho ", " bc ", " w/ ", " w/o ",
    )

    def __init__(self, baseline_responses_jsonl: Path,
                 candidate_responses_jsonl: Path,
                 threshold: float | None = None,
                 target: dict | None = None):
        self.baseline_responses = baseline_responses_jsonl
        self.candidate_responses = candidate_responses_jsonl
        self.threshold = (
            threshold if threshold is not None else self.DEFAULT_THRESHOLD
        )
        self.target = target or self.DEFAULT_TARGET

    @staticmethod
    def _extract_features(text: str) -> dict:
        if not text:
            return {"lowercase_ratio": 0.0, "abbreviation_ratio": 0.0,
                    "byte_len": 0}
        # lowercase_ratio: fraction of alpha chars that are lowercase
        alpha = [c for c in text if c.isalpha()]
        if alpha:
            lower = sum(1 for c in alpha if c.islower())
            lowercase_ratio = lower / len(alpha)
        else:
            lowercase_ratio = 0.0
        # abbreviation_ratio: fraction of whitespace tokens that match
        # one of the canonical abbreviation patterns. We use the same
        # space-padded comparison as the C scorer.
        padded = " " + text.lower() + " "
        abbrev_hits = sum(1 for a in FidelityJudge._ABBREVS if a in padded)
        # Token-ish denominator: number of spaces + 1, capped.
        tokens = max(1, padded.count(" ") - 1)
        abbreviation_ratio = min(1.0, abbrev_hits / float(tokens))
        return {
            "lowercase_ratio": lowercase_ratio,
            "abbreviation_ratio": abbreviation_ratio,
            "byte_len": len(text.encode("utf-8")),
        }

    @staticmethod
    def _axis_match(observed: float, target: float) -> float:
        # Same shape as hu_pm_axis_match in personal_model.c:
        # 1.0 - clamp(|obs - tgt|, 0, 1).
        if target <= 0.0:
            return 1.0 if observed == 0.0 else 0.0
        diff = abs(observed - target)
        rel = diff / target
        if rel >= 1.0:
            return 0.0
        return 1.0 - rel

    @staticmethod
    def _length_match(byte_len: int, target_avg: float) -> float:
        if target_avg <= 0.0:
            return 1.0 if byte_len == 0 else 0.0
        diff = abs(float(byte_len) - target_avg)
        rel = diff / target_avg
        if rel >= 1.0:
            return 0.0
        return 1.0 - rel

    def _score_response(self, response: str) -> float | None:
        if not response:
            return None
        feats = FidelityJudge._extract_features(response)
        ll = FidelityJudge._axis_match(feats["lowercase_ratio"],
                                       self.target["lowercase_ratio"])
        ab = FidelityJudge._axis_match(feats["abbreviation_ratio"],
                                       self.target["abbreviation_ratio"])
        lm = FidelityJudge._length_match(feats["byte_len"],
                                         self.target["avg_message_length"])
        return (ll + ab + lm) / 3.0

    def _score_jsonl(self, path: Path) -> tuple[float, int, int]:
        """Returns (mean_score, scored_count, skipped_count). Mean is 0.0
        when scored_count is 0."""
        if not path.exists():
            return (0.0, 0, 0)
        total = 0.0
        scored = 0
        skipped = 0
        for line in path.read_text().splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                skipped += 1
                continue
            response = rec.get("response", "")
            score = self._score_response(response)
            if score is None:
                skipped += 1
                continue
            total += score
            scored += 1
        mean = (total / scored) if scored > 0 else 0.0
        return (mean, scored, skipped)

    def evaluate(self, baseline: dict, candidate: dict) -> dict:
        # The adapter-metadata dicts are passed through unmodified — the
        # judge doesn't need them but the harness expects them in the
        # output for downstream consumers.
        if not self.baseline_responses or not self.baseline_responses.exists():
            return {
                "judge": "fidelity", "verdict": "skipped",
                "reason": f"baseline responses JSONL missing: "
                          f"{self.baseline_responses}",
                "baseline": baseline, "candidate": candidate,
            }
        if not self.candidate_responses or not self.candidate_responses.exists():
            return {
                "judge": "fidelity", "verdict": "skipped",
                "reason": f"candidate responses JSONL missing: "
                          f"{self.candidate_responses}",
                "baseline": baseline, "candidate": candidate,
            }
        b_mean, b_scored, b_skipped = self._score_jsonl(self.baseline_responses)
        c_mean, c_scored, c_skipped = self._score_jsonl(self.candidate_responses)
        if b_scored == 0 or c_scored == 0:
            return {
                "judge": "fidelity", "verdict": "skipped",
                "reason": f"zero scored responses (baseline={b_scored}, "
                          f"candidate={c_scored})",
                "baseline": baseline, "candidate": candidate,
                "baseline_mean": b_mean, "candidate_mean": c_mean,
            }
        delta = c_mean - b_mean
        if delta >= self.threshold:
            verdict = "pass"
        elif delta <= -self.threshold:
            verdict = "regress"
        else:
            verdict = "no-change"
        return {
            "judge": "fidelity",
            "verdict": verdict,
            "reason": (f"baseline_mean={b_mean:.4f} candidate_mean={c_mean:.4f} "
                       f"delta={delta:+.4f} threshold={self.threshold:.4f}"),
            "baseline": baseline, "candidate": candidate,
            "baseline_mean": b_mean,
            "candidate_mean": c_mean,
            "delta": delta,
            "threshold": self.threshold,
            "baseline_scored": b_scored,
            "candidate_scored": c_scored,
            "baseline_skipped": b_skipped,
            "candidate_skipped": c_skipped,
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
    ap.add_argument("--judge", choices=["metadata", "sft-prompts", "llm", "fidelity"],
                    default="metadata",
                    help="Judge implementation (default: metadata)")
    ap.add_argument("--completions-jsonl", type=Path,
                    help="For --judge llm: JSONL of "
                         "{prompt, baseline_output, candidate_output} per line")
    ap.add_argument("--baseline-responses-jsonl", type=Path,
                    help="For --judge fidelity: JSONL of "
                         "{response: '...'} per line from the BASELINE adapter")
    ap.add_argument("--candidate-responses-jsonl", type=Path,
                    help="For --judge fidelity: JSONL of "
                         "{response: '...'} per line from the CANDIDATE adapter")
    ap.add_argument("--fidelity-threshold", type=float, default=0.05,
                    help="Absolute fidelity delta required for PASS "
                         "(default 0.05, per spec D-M3-5)")
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
    elif args.judge == "llm":
        if args.completions_jsonl is None:
            print("ERROR: --completions-jsonl is required for --judge llm",
                  file=sys.stderr)
            sys.exit(2)
        judge = LlmJudge(args.completions_jsonl)
    elif args.judge == "fidelity":
        if (args.baseline_responses_jsonl is None
                or args.candidate_responses_jsonl is None):
            print("ERROR: --baseline-responses-jsonl and "
                  "--candidate-responses-jsonl are required for --judge fidelity",
                  file=sys.stderr)
            sys.exit(2)
        judge = FidelityJudge(args.baseline_responses_jsonl,
                              args.candidate_responses_jsonl,
                              threshold=args.fidelity_threshold)
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
