#!/usr/bin/env python3
"""
Longitudinal on-device personalization TRAJECTORY harness (SOTA spec C1).

Where eval_fidelity_nightly.py answers "is THIS adapter better than base right
now?", this answers the SOTA question: "is the personalization loop producing a
measured, guard-railed IMPROVEMENT over successive generations?"

For each adapter generation (gen 0 = base, gen 1..K = successive retrains) it
measures two orthogonal axes on held-out data:
  - persona fidelity  : the deterministic shape classifier (reused verbatim)
  - base capability   : the deterministic probe set (eval_base_capability.py)
appends a row to a time-series, and runs the pure curve-gate
(trajectory_gate.py) over the whole history.

Per-generation results are CACHED keyed by (adapter_path, fixture_sha,
probeset_sha) so a nightly run computes only the NEWEST generation and appends —
it never re-runs the whole history (compute constraint, design D6).

Inference is injected via `generate_fn` so the orchestration is fully unit-
testable without a model (see test_eval_personalization_trajectory.py).

Usage:
  python3 scripts/eval_personalization_trajectory.py \\
    --manifest ~/.human/training-data/trajectory-manifest.json \\
    --model-id mlx-community/gemma-4-31b-it-4bit \\
    --output-json ~/.human/logs/trajectory.json

Manifest schema (ordered generations):
  {"generations": [
     {"gen": 0, "label": "base",      "adapter_path": null,           "train_pairs": 0,    "ts": "..."},
     {"gen": 1, "label": "v4-repair", "adapter_path": "/abs/adapter", "train_pairs": 1963, "ts": "..."}
  ]}

Exit codes (mirror eval_fidelity_nightly.py):
  0 = PASS or SKIP   1 = FAIL   2 = DEFERRED (mlx_lm/model unavailable)
"""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from eval_base_capability import load_probes, probes_sha256, score_base_capability
from eval_fidelity_helpers import (
    bootstrap_ci,
    compute_persona_fidelity_scores,
    load_held_out_prompts_from_jsonl,
)
from trajectory_gate import TrajectoryGateConfig, evaluate_trajectory_gate

DEFAULT_FIXTURE = (
    Path(__file__).parent.parent
    / "docs/plans/2026-05-26-sprint-56-gemma-as-seth/data/heldout-prompts.jsonl"
)
DEFAULT_MODEL = "mlx-community/gemma-4-31b-it-4bit"
DEFAULT_LOG_DIR = Path.home() / ".human" / "logs"
BOOTSTRAP_CONFIDENCE = 0.95


def _sha256_file(path: Path | str) -> str:
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def default_generate_fn(model_id: str, prompt: str, adapter_path: str | None) -> str:
    """Real inference path — thin wrapper over eval_fidelity_nightly.generate so
    the two harnesses share one subprocess contract. Imported lazily so this
    module loads (and is testable) even where mlx_lm isn't installed."""
    import eval_fidelity_nightly  # noqa: PLC0415

    return eval_fidelity_nightly.generate(model_id, prompt, adapter_path=adapter_path)


def cache_key(adapter_path: str | None, fixture_sha: str, probeset_sha: str) -> str:
    return f"{adapter_path or 'base'}::{fixture_sha}::{probeset_sha}"


def _demo_answer_for_probe(check: dict) -> str:
    """Best-effort correct answer derived from a probe's check spec, for the
    no-model --demo path. Deterministic checker types get a real answer; regex
    types fall through to empty (those probes fail in the demo — illustrative
    numbers only)."""
    t = check.get("type")
    if t == "numeric_equals":
        return str(check["value"])
    if t == "json_equals":
        return json.dumps(check["value"])
    if t == "exact":
        return str(check["value"])
    if t == "json_keys":
        return json.dumps({k: (True if k == "active" else "x") for k in check["keys"]})
    return ""


def make_demo_generate_fn(probes: list):
    """A deterministic, no-model generate_fn for --demo. Answers probes from
    their check specs; for fidelity prompts, the base model sounds more 'AI'
    (lower shape score) and the adapter sounds casual (higher) so the demo
    shows a rising curve. Numbers are illustrative, not measured."""
    probe_ans = {p["prompt"]: _demo_answer_for_probe(p["check"]) for p in probes}

    def fn(model_id, prompt, adapter_path):
        if prompt in probe_ans:
            return probe_ans[prompt]
        if adapter_path:
            return "haha yeah for sure, what's the move tonight"
        return "Certainly! I can help with that."

    return fn


# Built-in 2-generation manifest for --demo when --manifest is omitted.
DEMO_MANIFEST = [
    {"gen": 0, "label": "base", "adapter_path": None, "train_pairs": 0, "ts": None},
    {"gen": 1, "label": "demo-adapter", "adapter_path": "/demo/adapter",
     "train_pairs": 500, "ts": None},
]


def measure_generation(
    gen: dict,
    prompts: list,
    probes: list,
    model_id: str,
    generate_fn,
) -> dict:
    """Run both axes for one generation. Returns the measured fields merged onto
    the generation's manifest metadata. No caching here — that's the caller's
    job so this stays a pure measurement step."""
    adapter_path = gen.get("adapter_path")

    # Fidelity axis — per-prompt shape-classifier scores → mean + bootstrap CI.
    fid_responses = [
        generate_fn(model_id, (p["prompt"] if isinstance(p, dict) else p), adapter_path)
        for p in prompts
    ]
    classifications, fid_mean = compute_persona_fidelity_scores(
        fid_responses, channel="imessage"
    )
    per_prompt_scores = [c["score"] for c in classifications]
    _, fid_lo, fid_hi = bootstrap_ci(
        per_prompt_scores, n_resamples=100, confidence=BOOTSTRAP_CONFIDENCE
    )

    # Base-capability axis — deterministic probes, no LLM judge.
    probe_responses = [
        generate_fn(model_id, probe["prompt"], adapter_path) for probe in probes
    ]
    _, base_cap = score_base_capability(probe_responses, probes)

    return {
        "gen": int(gen["gen"]),
        "label": gen.get("label", f"gen{gen['gen']}"),
        "adapter_path": adapter_path,
        "train_pairs": gen.get("train_pairs", 0),
        "ts": gen.get("ts"),
        "fidelity_mean": round(fid_mean, 4),
        "fidelity_ci": [round(fid_lo, 4), round(fid_hi, 4)],
        "base_capability": round(base_cap, 4),
    }


def run_trajectory(
    manifest_gens: list[dict],
    prompts: list,
    probes: list,
    fixture_sha: str,
    probeset_sha: str,
    model_id: str,
    generate_fn,
    cache: dict | None = None,
    gate_cfg: TrajectoryGateConfig | None = None,
) -> tuple[dict, dict]:
    """Measure every generation (using `cache` to skip already-measured ones),
    run the gate, and return (trajectory_dict, updated_cache).

    Pure w.r.t. I/O: callers supply the cache and persist it. `generate_fn` is
    injected, so this is fully testable without a model.
    """
    cache = dict(cache or {})
    measured: list[dict] = []
    for gen in sorted(manifest_gens, key=lambda g: int(g["gen"])):
        key = cache_key(gen.get("adapter_path"), fixture_sha, probeset_sha)
        if key in cache:
            row = dict(cache[key])
            row["cached"] = True
        else:
            row = measure_generation(gen, prompts, probes, model_id, generate_fn)
            cache[key] = {
                k: row[k]
                for k in ("gen", "label", "adapter_path", "train_pairs", "ts",
                          "fidelity_mean", "fidelity_ci", "base_capability")
            }
            row["cached"] = False
        measured.append(row)

    gate = evaluate_trajectory_gate(measured, gate_cfg)
    trajectory = {
        "timestamp": datetime.now().isoformat(),
        "model_id": model_id,
        "fixture_sha": fixture_sha,
        "probeset_sha": probeset_sha,
        "generations": measured,
        "gate": gate.to_dict(),
        "verdict": gate.verdict,
        "exit_code": {"PASS": 0, "SKIP": 0, "FAIL": 1}.get(gate.verdict, 1),
    }
    return trajectory, cache


def _load_manifest(path: Path) -> list[dict]:
    data = json.loads(Path(path).read_text())
    gens = data.get("generations", data) if isinstance(data, dict) else data
    if not isinstance(gens, list):
        raise ValueError("manifest must be a list or {'generations': [...]}")
    return gens


def main() -> int:
    ap = argparse.ArgumentParser(description="Longitudinal personalization trajectory harness")
    ap.add_argument("--manifest", type=Path,
                    help="JSON manifest of ordered generations (optional under --demo)")
    ap.add_argument("--model-id", default=DEFAULT_MODEL)
    ap.add_argument("--held-out-fixture", type=Path, default=DEFAULT_FIXTURE)
    ap.add_argument("--cache-json", type=Path,
                    help="Per-generation measurement cache (read+write)")
    ap.add_argument("--output-json", type=Path, help="Write trajectory.json here")
    ap.add_argument("--log-dir", type=Path, default=DEFAULT_LOG_DIR)
    ap.add_argument("--demo", action="store_true",
                    help="no-model smoke: deterministic stub inference + a built-in 2-gen "
                         "manifest if --manifest is omitted. Exercises the full pipeline "
                         "end-to-end (AC-5) without mlx_lm; numbers are illustrative only.")
    args = ap.parse_args()

    args.log_dir.mkdir(parents=True, exist_ok=True)

    if not args.held_out_fixture.exists():
        v = {"timestamp": datetime.now().isoformat(), "verdict": "DEFERRED",
             "reason": f"fixture missing: {args.held_out_fixture}", "exit_code": 2}
        print(f"[DEFERRED] {v['reason']}")
        if args.output_json:
            args.output_json.write_text(json.dumps(v, indent=2))
        return 2

    prompts = load_held_out_prompts_from_jsonl(str(args.held_out_fixture))
    if len(prompts) < 20:
        v = {"timestamp": datetime.now().isoformat(), "verdict": "SKIP",
             "reason": f"insufficient held-out prompts: {len(prompts)} < 20", "exit_code": 0}
        print(f"[SKIP] {v['reason']}")
        if args.output_json:
            args.output_json.write_text(json.dumps(v, indent=2))
        return 0

    probes = load_probes()
    fixture_sha = _sha256_file(args.held_out_fixture)
    probeset_sha = probes_sha256()

    if not args.manifest and not args.demo:
        print("error: --manifest is required (or pass --demo for a no-model smoke)",
              file=sys.stderr)
        return 2
    manifest_gens = _load_manifest(args.manifest) if args.manifest else DEMO_MANIFEST
    generate_fn = make_demo_generate_fn(probes) if args.demo else default_generate_fn

    cache = {}
    if args.cache_json and args.cache_json.exists():
        try:
            cache = json.loads(args.cache_json.read_text())
        except (json.JSONDecodeError, ValueError):
            cache = {}

    trajectory, cache = run_trajectory(
        manifest_gens, prompts, probes, fixture_sha, probeset_sha,
        args.model_id, generate_fn, cache=cache,
    )

    print(f"\n=== TRAJECTORY ({len(trajectory['generations'])} gens) ===", flush=True)
    for g in trajectory["generations"]:
        print(f"  gen {g['gen']} ({g['label']}): fidelity={g['fidelity_mean']:.3f} "
              f"base_cap={g['base_capability']:.3f} cached={g.get('cached')}", flush=True)
    print(f"=== VERDICT: {trajectory['verdict']} ===", flush=True)
    for d in trajectory["gate"].get("details", []):
        print(f"  - {d}", flush=True)

    if args.cache_json:
        args.cache_json.write_text(json.dumps(cache, indent=2))
    if args.output_json:
        args.output_json.write_text(json.dumps(trajectory, indent=2))
    log_file = args.log_dir / f"trajectory-{datetime.now().strftime('%Y-%m-%d')}.json"
    log_file.write_text(json.dumps(trajectory, indent=2))

    return trajectory["exit_code"]


if __name__ == "__main__":
    sys.exit(main())
