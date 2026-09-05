#!/usr/bin/env python3
"""score_candidate_offline.py — offline LUAR authorship-gap scoring for a
mlx-tune candidate adapter vs the currently SERVING adapter.

WHY OFFLINE: contract C4's authorship_nightly.sh regenerates trial replies
through the LIVE production head + :8741 (gen_classifier_trials.py). The
mlx-tune candidate-training stage in scripts/nightly-retrain.sh runs INSIDE
the nightly retrain window, where :8741 is intentionally stopped (never two
loaders beside the 56 GB base — see never_two_llm_instances /
.claude/rules/lora-scale-default-or-die.md). This script generates both the
candidate's and the serving adapter's replies DIRECTLY via mlx_lm — one
adapter resident at a time, unloaded between — then scores each resulting
trial set against the same LUAR 5v5 PersonalBench protocol authorship_gap.py
already implements, by re-invoking that script rather than reimplementing
its embedding/bootstrap logic.

Two DISJOINT venvs are required and are never both imported into one
process:
  - a GENERATION python with mlx_lm     (default: ~/.human/venvs/train312/bin/python)
  - a SCORING python with torch/transformers for LUAR
                                          (default: ~/.human/venvs/eval312/bin/python)

This file is the ORCHESTRATOR. At import time it touches only the stdlib, so
--dry-run and every refusal path run under any python3 — no mlx_lm, no
torch. It dispatches the actual generation to itself in "worker mode"
(re-invoked as a subprocess under --gen-python) and reuses authorship_gap.py
verbatim for scoring (subprocess under --eval-python), so torch/mlx_lm are
only ever imported inside the venv that actually has them.

--dry-run MUST NOT load any model weights. It validates: the trial contexts
file, both adapter directories (safetensors HEADER + lora_b tensors only,
via adapter_is_real.py — zero weight loading, see that script's own
docstring), and the presence of both venv pythons and chat.db — then reports
what a real run WOULD do. See .claude/rules/reports-success-does-nothing.md
and .claude/rules/no-number-without-a-measurement.md: a refusal must be
loud and logged, never fabricated as a pass.

Usage:
    score_candidate_offline.py --candidate <adapter_dir> [--serving <adapter_dir>]
        [--trials ~/blind_ab_run/classifier_trials.json]
        [--base mlx-community/GLM-4.5-Air-4bit]
        [--out ~/.human/logs/candidate-authorship-<date>.json]
        [--dry-run]

If --serving is omitted, it is resolved from ~/.human/config.json's
personalization.lora_adapter_path — the same field the daemon itself reads
(see scripts/blind_ab/classifier_gate.py's served_adapter(), and MEMORY.md:
"Daemon needs personalization.lora_adapter_path, NOT mlx_local.adapter_path").

Refuses (nonzero exit, nothing written to --out) when: the trial contexts
file is missing/too small, either adapter directory fails the real-adapter
check, either venv python is missing, chat.db is missing, generation
produces fewer than --min-ok replies, or authorship_gap.py itself refuses.
Never promotes anything — this script only measures.

The report also carries a `casing_gate` field (see casing_probe.py):
PASS/FAIL plus the candidate's lowercase-start / terminal-punct rates
against the human side of the same generated trials. This is a narrow,
deterministic, LUAR-independent check for the 2026-09-04 finding that a
persona twin can score fine on authorship while still being trivially
distinguishable by casing habits alone. It is informational only — a FAIL
does not change `comparison.candidate_closer_to_seth` or refuse the run.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS_DIR = os.path.dirname(HERE)
sys.path.insert(0, HERE)
sys.path.insert(0, SCRIPTS_DIR)

DEFAULT_TRIALS = os.path.expanduser("~/blind_ab_run/classifier_trials.json")
DEFAULT_BASE = "mlx-community/GLM-4.5-Air-4bit"
DEFAULT_GEN_PY = os.path.expanduser("~/.human/venvs/train312/bin/python")
DEFAULT_EVAL_PY = os.path.expanduser("~/.human/venvs/eval312/bin/python")
DEFAULT_CHATDB = os.path.expanduser("~/Library/Messages/chat.db")
DEFAULT_CONFIG = os.path.expanduser("~/.human/config.json")
MIN_TRIALS = 20


def served_adapter_path(config_path=DEFAULT_CONFIG):
    """Same field the daemon reads. See scripts/blind_ab/classifier_gate.py's
    served_adapter() (this mirrors it) and the mlx_local.adapter_path fallback
    for older configs."""
    try:
        c = json.load(open(config_path))
    except Exception:
        return None
    return (c.get("personalization", {}).get("lora_adapter_path")
            or c.get("mlx_local", {}).get("adapter_path"))


def load_trial_contexts(path, min_trials=MIN_TRIALS):
    """Load {context, real_seth} pairs — the SAME base file
    gen_classifier_trials.py reads (contract C4). Raises SystemExit with a
    REFUSING message rather than fabricating a smaller/partial set."""
    if not os.path.isfile(path):
        raise SystemExit(f"REFUSING: trial contexts file not found: {path}; nothing written")
    try:
        data = json.load(open(path))
    except Exception as e:
        raise SystemExit(f"REFUSING: could not parse {path} ({type(e).__name__}: {e}); nothing written")
    trials = data.get("trials") if isinstance(data, dict) else data
    if not trials:
        raise SystemExit(f"REFUSING: {path} has zero trials; nothing written")
    clean = [t for t in trials if isinstance(t, dict) and t.get("context") and t.get("real_seth")]
    if len(clean) < min_trials:
        raise SystemExit(
            f"REFUSING: {len(clean)} usable trials < --min-trials {min_trials}; nothing written")
    return clean


def check_adapter_dir(label, path):
    """Zero-weight-loading validity check: adapter_is_real.py reads only the
    safetensors HEADER + the lora_b tensor bytes, never loads a model. Safe
    to run during --dry-run — see adapter_is_real.py's own docstring."""
    if not path or not os.path.isdir(path):
        return False, f"{label} adapter dir missing or not a directory: {path}"
    from adapter_is_real import adapter_is_real  # local import: only reachable with a path
    ok, why = adapter_is_real(path)
    return ok, f"{label}: {why}"


def check_python(label, path):
    ok = bool(path) and os.path.isfile(path) and os.access(path, os.X_OK)
    return ok, f"{label}: {path} {'OK' if ok else 'MISSING or not executable'}"


# --------------------------------------------------------------------------
# generation worker — imports mlx_lm ONLY here, and only when actually
# invoked as a subprocess under --gen-python (never under the orchestrator's
# own interpreter, which may not have mlx_lm at all).
# --------------------------------------------------------------------------


def run_gen_worker(args):
    """Load base+adapter ONCE, generate a reply for every context, free the
    model, write a {trials:[{i, context, real_seth, ai_response}]} JSON.

    Mirrors adapter_smoke_test.run_adapter's load-generate-free shape and
    gen_classifier_trials.generate_one's request body (same production
    system-prompt head, same default max_tokens/temperature) — but calls
    mlx_lm directly instead of making an HTTP request to :8741, since :8741
    is down for the whole duration of the nightly retrain window this
    script runs inside.
    """
    import gc
    import mlx.core as mx
    from mlx_lm import generate as mlx_generate
    from mlx_lm import load

    if not args.adapter:
        sys.exit("[score_candidate_offline:gen] FATAL: --adapter is required in --gen-worker mode")
    with open(args.gen_contexts) as f:
        contexts = json.load(f)
    with open(args.gen_head) as f:
        head = f.read()

    print(f"[score_candidate_offline:gen] loading {args.base} + {args.adapter}", file=sys.stderr)
    model, tokenizer = load(args.base, adapter_path=args.adapter)
    sampler = None
    try:
        from mlx_lm.sample_utils import make_sampler
        sampler = make_sampler(temp=args.temperature)
    except Exception:
        pass  # older mlx_lm: fall back to generate()'s own default sampler

    out_trials = []
    n = len(contexts)
    for i, t in enumerate(contexts, 1):
        msgs = [{"role": "system", "content": head}, {"role": "user", "content": t["context"]}]
        try:
            prompt = tokenizer.apply_chat_template(msgs, add_generation_prompt=True)
            kwargs = {"max_tokens": args.max_tokens}
            if sampler is not None:
                kwargs["sampler"] = sampler
            text = mlx_generate(model, tokenizer, prompt=prompt, **kwargs)
        except Exception as e:
            print(f"[score_candidate_offline:gen] [{i}/{n}] ERROR {type(e).__name__}: {e}", file=sys.stderr)
            continue
        text = (text or "").strip()
        if not text:
            print(f"[score_candidate_offline:gen] [{i}/{n}] empty", file=sys.stderr)
            continue
        out_trials.append({
            "i": t.get("i", f"item_{i:02d}"),
            "context": t["context"],
            "real_seth": t["real_seth"],
            "ai_response": text,
        })
        mx.clear_cache()
        if i % 10 == 0:
            print(f"[score_candidate_offline:gen] [{i}/{n}] {len(out_trials)} ok so far", file=sys.stderr)

    del model
    gc.collect()
    mx.clear_cache()

    if len(out_trials) < args.min_ok:
        sys.exit(f"REFUSING: only {len(out_trials)}/{n} generations succeeded "
                 f"(< --min-ok {args.min_ok}); nothing written")

    with open(args.gen_out, "w") as f:
        json.dump({
            "trials": out_trials,
            "adapter": args.adapter,
            "base": args.base,
            "date": time.strftime("%Y-%m-%d"),
        }, f, indent=2)
    print(f"[score_candidate_offline:gen] wrote {len(out_trials)}/{n} -> {args.gen_out}")
    return 0


# --------------------------------------------------------------------------
# orchestrator
# --------------------------------------------------------------------------


def build_parser():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--candidate", help="candidate adapter directory to score")
    ap.add_argument("--serving", default=None,
                    help="serving adapter directory (default: resolved from ~/.human/config.json)")
    ap.add_argument("--base", default=DEFAULT_BASE)
    ap.add_argument("--trials", default=DEFAULT_TRIALS)
    ap.add_argument("--chatdb", default=DEFAULT_CHATDB)
    ap.add_argument("--config", default=DEFAULT_CONFIG, help="~/.human/config.json (read-only)")
    ap.add_argument("--out", default=None)
    ap.add_argument("--gen-python", default=os.environ.get("HU_GEN_PYTHON", DEFAULT_GEN_PY))
    ap.add_argument("--eval-python", default=os.environ.get("HU_EVAL_PYTHON", DEFAULT_EVAL_PY))
    ap.add_argument("--max-tokens", type=int, default=120)
    ap.add_argument("--temperature", type=float, default=0.7)
    ap.add_argument("--min-ok", type=int, default=20)
    ap.add_argument("--min-trials", type=int, default=MIN_TRIALS)
    ap.add_argument("--splits", type=int, default=200)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--dry-run", action="store_true")
    # --- internal worker mode: re-invoked as a subprocess, never call by hand ---
    ap.add_argument("--gen-worker", action="store_true", help=argparse.SUPPRESS)
    ap.add_argument("--adapter", default=None, help=argparse.SUPPRESS)
    ap.add_argument("--gen-contexts", default=None, help=argparse.SUPPRESS)
    ap.add_argument("--gen-head", default=None, help=argparse.SUPPRESS)
    ap.add_argument("--gen-out", default=None, help=argparse.SUPPRESS)
    return ap


def run_preflight(args):
    """Validate every precondition WITHOUT loading a model. Returns
    (ok: bool, trials_or_None, serving_path)."""
    serving = args.serving or served_adapter_path(args.config)
    print(f"candidate:      {args.candidate}")
    print(f"serving:        {serving}")

    ok = True
    trials = None
    try:
        trials = load_trial_contexts(args.trials, args.min_trials)
        print(f"trial contexts: {len(trials)} usable ({args.trials})")
    except SystemExit as e:
        print(str(e))
        ok = False

    for label, path in (("candidate", args.candidate), ("serving", serving)):
        adapter_ok, why = check_adapter_dir(label, path)
        print(f"adapter check:  {why}")
        ok = ok and adapter_ok

    for label, py in (("gen-python", args.gen_python), ("eval-python", args.eval_python)):
        py_ok, why = check_python(label, py)
        print(f"venv check:     {why}")
        ok = ok and py_ok

    if os.path.isfile(args.chatdb):
        print(f"chatdb:         {args.chatdb} OK")
    else:
        print(f"chatdb:         MISSING ({args.chatdb}) — the floor computation would refuse")
        ok = False

    return ok, trials, serving


def main(argv=None):
    args = build_parser().parse_args(argv)

    if args.gen_worker:
        return run_gen_worker(args)

    if not args.candidate:
        sys.exit("FATAL: --candidate is required (unless --gen-worker)")

    date = time.strftime("%Y-%m-%d")
    out_path = args.out or os.path.expanduser(f"~/.human/logs/candidate-authorship-{date}.json")

    print("=" * 70)
    print("score_candidate_offline.py")
    print("=" * 70)
    ok, trials, serving = run_preflight(args)

    if args.dry_run:
        print("-" * 70)
        if ok:
            print("[dry-run] PASS — trials, both adapters, both venvs, and chat.db all validated")
        else:
            print("[dry-run] FAIL — see lines above; a real run would REFUSE at the same point")
        print("[dry-run] NOTE: no model weights were loaded during this dry-run")
        return 0 if ok else 1

    if not ok:
        sys.exit("REFUSING: preconditions failed (see checks above); nothing written")

    # --- real run: build the production head, generate both sides, score ---
    from eval_blinded_ab import production_system_prompt  # lazy: real-run only
    try:
        head = production_system_prompt()
    except SystemExit as e:
        sys.exit(f"REFUSING: {e}")

    tmpdir = tempfile.mkdtemp(prefix="score-candidate-offline-")
    contexts_path = os.path.join(tmpdir, "contexts.json")
    head_path = os.path.join(tmpdir, "head.txt")
    with open(contexts_path, "w") as f:
        json.dump(trials, f)
    with open(head_path, "w") as f:
        f.write(head)

    gen_trials_paths = {}
    # Sequential, on purpose: each subprocess loads its model, generates,
    # frees, and EXITS before the next one starts — process exit is the
    # strongest guarantee mlx/Metal pages are actually reclaimed (see
    # never_two_llm_instances). Never run these two concurrently.
    for label, adapter_dir in (("candidate", args.candidate), ("serving", serving)):
        gen_out = os.path.join(tmpdir, f"{label}_trials.json")
        print(f"generating {label} replies via {args.gen_python} (base={args.base}) ...")
        cmd = [
            args.gen_python, os.path.abspath(__file__), "--gen-worker",
            "--adapter", adapter_dir, "--base", args.base,
            "--gen-contexts", contexts_path, "--gen-head", head_path,
            "--gen-out", gen_out, "--max-tokens", str(args.max_tokens),
            "--temperature", str(args.temperature), "--min-ok", str(args.min_ok),
        ]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=3600)
        sys.stderr.write(r.stderr)
        if r.returncode != 0 or not os.path.isfile(gen_out):
            sys.exit(f"REFUSING: generation failed for {label} (rc={r.returncode}); "
                     f"nothing written\n{r.stdout}\n{r.stderr}")
        print(r.stdout)
        gen_trials_paths[label] = gen_out

    gap_results = {}
    for label, trials_path in gen_trials_paths.items():
        gap_out = os.path.join(tmpdir, f"{label}_gap.json")
        cmd = [
            args.eval_python, os.path.join(HERE, "authorship_gap.py"),
            "--trials", trials_path, "--chatdb", args.chatdb, "--out", gap_out,
            "--splits", str(args.splits), "--min-trials", str(args.min_trials),
            "--seed", str(args.seed),
        ]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=1800)
        sys.stderr.write(r.stderr)
        if r.returncode != 0 or not os.path.isfile(gap_out):
            sys.exit(f"REFUSING: authorship_gap.py failed for {label} (rc={r.returncode}); "
                     f"nothing written\n{r.stdout}\n{r.stderr}")
        with open(gap_out) as f:
            gap_results[label] = json.load(f)

    cand_twin = gap_results["candidate"]["twin_seth_vs_adapter"]["mean"]
    serv_twin = gap_results["serving"]["twin_seth_vs_adapter"]["mean"]

    # casing_gate: a narrow, deterministic, LUAR-independent check for the
    # 2026-09-04 finding that a persona twin can score fine on authorship
    # (vocabulary/register) while still being trivially distinguishable by
    # "does every reply start with a lowercase letter." Runs on the
    # CANDIDATE's own generated trials (gen_trials_paths["candidate"] already
    # has {ai_response, real_seth} per row — the same file authorship_gap.py
    # just scored). Purely informational: a FAIL here does not change
    # candidate_closer_to_seth or refuse anything — this script only measures.
    from casing_probe import compute_casing_gate_from_file  # lazy: real-run only
    try:
        casing_gate = compute_casing_gate_from_file(
            gen_trials_paths["candidate"], min_trials=args.min_trials)
    except SystemExit as e:
        casing_gate = {"pass": None, "error": str(e)}

    out = {
        "date": date,
        "candidate_adapter": args.candidate,
        "serving_adapter": serving,
        "base": args.base,
        "candidate": gap_results["candidate"],
        "serving": gap_results["serving"],
        "comparison": {
            "twin_candidate": cand_twin,
            "twin_serving": serv_twin,
            "delta_candidate_minus_serving": round(cand_twin - serv_twin, 4),
            "candidate_closer_to_seth": cand_twin > serv_twin,
        },
        "casing_gate": casing_gate,
    }
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w") as f:
        json.dump(out, f, indent=2)
    print(json.dumps(out["comparison"], indent=1))
    print(f"casing_gate: {'PASS' if casing_gate.get('pass') else 'FAIL' if casing_gate.get('pass') is False else 'UNKNOWN'}"
          + (f" — {'; '.join(casing_gate.get('reasons', []))}" if casing_gate.get("reasons") else ""))
    print(f"wrote {out_path}")
    print("NEXT: promotion stays scripts/register_v6_adapter.py + a human decision. "
          "This script only measures.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
