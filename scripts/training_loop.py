#!/usr/bin/env python3
"""
Automated Training Loop — the full cycle that makes Seth more human.

Orchestrates: data extraction -> merge -> prepare -> SFT -> DPO -> eval -> promote/rollback

Each cycle:
  1. Extract fresh data (iMessage, Facebook, Apple Photos)
  2. Merge all sources + deduplicate
  3. Prepare fine-tuning splits with style augmentation
  4. Run SFT on Gemma 4 31B (LoRA)
  5. Run DPO pass (if pairs exist)
  6. Run convo-trainer sessions to generate new training data + scores
  7. Run red team eval
  8. Run blinded A/B eval
  9. Compare to previous best adapter
  10. Promote if improved, rollback if degraded

Usage:
  python3 scripts/training_loop.py                     # full cycle
  python3 scripts/training_loop.py --skip-extract       # skip data extraction
  python3 scripts/training_loop.py --eval-only          # just run eval on current adapter
  python3 scripts/training_loop.py --cycles 3           # run 3 improvement cycles
  python3 scripts/training_loop.py --dry-run            # simulate without training
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
import sys
import time
import urllib.request
from datetime import datetime
from os.path import basename
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPTS = REPO_ROOT / "scripts"

# ── Training resource preflight (2026-07-26 crash-loop fix) ──────────────────
#
# Four reboots on 2026-07-26 (04:01, 05:02, 06:45, 14:38) traced to LoRA
# training running CONCURRENTLY with the mlx-server holding the same base
# resident. C3 flipped this loop to target the SERVING base, which is correct
# for adapter validity, but made every run compete for the same ~56 GB. Eleven
# runs fired that day, six inside 28 minutes (06:09-06:37), driving a 128 GB
# machine to 154 MB free / 28 GB compressed / 13.2 GB swap. The trainer exiting
# recovered 53 GB instantly, which is what pinned the cause.
#
# lora_training_runner.c has no cooldown, no lock, and no memory precondition —
# its only trigger is a pair-count threshold crossing. These guards live HERE
# because every path (daemon runner, m3_loop_cycle.sh, manual) funnels through
# run_mlx_lora_training.
LORA_LOCK_PATH = Path.home() / ".human" / "lora_training.lock"

# Peak training residency as a multiple of the base's on-disk size: training
# holds the weights PLUS optimizer state, gradients and activations. Measured
# on GLM-4.5-Air-4bit (56 GB on disk): peak demand exceeded 70 GB while a
# 57 GB server was resident on a 128 GB box.
TRAIN_MEM_FACTOR = 1.25
TRAIN_MEM_OVERHEAD_BYTES = 6 * 1024 ** 3

# Empty default = no window restriction, so manual and CI runs are unaffected.
# Set learning.training_window (config) or HU_TRAIN_WINDOW (env) to "02:00-05:00"
# to confine automated retraining to hours when serving can be stopped.
DEFAULT_TRAIN_WINDOW = ""


def mlx_python(env: dict | None = None) -> str:
    """Interpreter to run `-m mlx_lm` with — the pinned venv, not whatever
    `python3` resolves to on PATH.

    2026-07-26: the daemon spawns `python3 scripts/training_loop.py`, PATH
    resolved that to python@3.14, and sys.executable therefore carried 3.14 into
    the mlx_lm subprocess. scripts/human-serve.sh deliberately avoids 3.14
    ("Python 3.14 has loky semaphore crash bug; python@3.13 was uninstalled
    2026-07-25") and pins .venv312 — the training path silently did not.

    Mirrors human-serve.sh's VENV_PYTHON choice so serving and training run the
    same interpreter and the same mlx/mlx_lm pins. Falls back to sys.executable
    when the venv is absent, so CI and non-mac checkouts still work.
    """
    env = os.environ if env is None else env
    override = (env.get("HU_MLX_PYTHON") or "").strip()
    if override and Path(override).exists():
        return override
    venv = Path.home() / "Documents" / "gemma-realtime-1" / ".venv312" / "bin" / "python3.12"
    if venv.exists():
        return str(venv)
    return sys.executable


def training_preflight_decision(need_bytes: int, available_bytes: int,
                                now_minutes: int | None = None,
                                window: tuple[int, int] | None = None,
                                lock_held: bool = False,
                                serving_conflict: bool = False) -> tuple[bool, str]:
    """Pure predicate: may a LoRA training run start? Returns (ok, reason).

    Takes FACTS, not the machine — no vm_stat, no clock, no filesystem — so the
    whole truth table is unit-testable without a 56 GB model or a real lock
    (.claude/rules/security-predicate-extraction.md). The impure fact-gathering
    lives in training_preflight().
    """
    if lock_held:
        return False, "another LoRA training run holds the lock (single-flight)"
    # Checked BEFORE the memory heuristic on purpose: a just-restarted server's
    # pages still count as reclaimable in vm_stat, so available_memory_bytes()
    # reads optimistically high while the server is about to fault all 56 GB
    # back in. Same-base co-residency is the exact crash condition, so assert it
    # structurally rather than inferring it from a byte count.
    if serving_conflict:
        return False, ("the production mlx-server is already serving this base; two copies "
                       "of the same multi-GB base cannot co-reside — stop serving for the "
                       "training window or train off-peak")
    if window is not None and now_minutes is not None:
        start, end = window
        inside = (start <= now_minutes < end) if start <= end \
            else (now_minutes >= start or now_minutes < end)  # window crosses midnight
        if not inside:
            return False, (f"outside the training window "
                           f"{start // 60:02d}:{start % 60:02d}-{end // 60:02d}:{end % 60:02d} "
                           f"(now {now_minutes // 60:02d}:{now_minutes % 60:02d})")
    if need_bytes > 0 and available_bytes < need_bytes:
        return False, (f"insufficient memory: need ~{need_bytes / 1024 ** 3:.0f} GB, "
                       f"only {available_bytes / 1024 ** 3:.0f} GB available "
                       f"(is the serving model resident?)")
    return True, "ok"


def parse_train_window(spec: str) -> tuple[int, int] | None:
    """"02:00-05:00" -> (120, 300) minutes-since-midnight. None when unset/invalid.

    A window crossing midnight ("22:00-04:00") is represented as start > end and
    handled by training_preflight_decision.
    """
    if not spec or "-" not in spec:
        return None
    try:
        lo, hi = spec.split("-", 1)
        lh, lm = (int(x) for x in lo.strip().split(":"))
        hh, hm = (int(x) for x in hi.strip().split(":"))
    except (ValueError, AttributeError):
        return None
    if not (0 <= lh < 24 and 0 <= hh < 24 and 0 <= lm < 60 and 0 <= hm < 60):
        return None
    start, end = lh * 60 + lm, hh * 60 + hm
    return None if start == end else (start, end)


def available_memory_bytes() -> int:
    """Reclaimable memory: free + inactive + purgeable + speculative.

    Deliberately NOT just "Pages free": macOS keeps free low by design, and
    inactive/purgeable pages are reclaimable under pressure. Counting only free
    would refuse every run on a healthy machine.
    """
    try:
        out = subprocess.run(["vm_stat"], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return 0
    page = 16384
    m = re.search(r"page size of (\d+) bytes", out)
    if m:
        page = int(m.group(1))
    total = 0
    for label in ("Pages free", "Pages inactive", "Pages purgeable", "Pages speculative"):
        m = re.search(rf"{label}:\s+(\d+)", out)
        if m:
            total += int(m.group(1))
    return total * page


def model_disk_bytes(model: str) -> int:
    """On-disk size of the HuggingFace snapshot for `model`, or 0 if unknown.

    0 means "can't estimate" and the memory check is skipped rather than
    guessed — refusing on an unknown model would break local/test bases.
    """
    if not model:
        return 0
    repo = "models--" + model.replace("/", "--")
    root = Path.home() / ".cache" / "huggingface" / "hub" / repo / "snapshots"
    if not root.is_dir():
        return 0
    best = 0
    for snap in root.iterdir():
        size = 0
        for f in snap.rglob("*"):
            try:
                if f.is_file():
                    size += f.stat().st_size
            except OSError:
                continue
        best = max(best, size)
    return best


def estimated_training_bytes(model: str) -> int:
    """Peak memory a LoRA run on `model` is expected to need. 0 if unknown."""
    disk = model_disk_bytes(model)
    return 0 if disk == 0 else int(disk * TRAIN_MEM_FACTOR) + TRAIN_MEM_OVERHEAD_BYTES


def training_window_spec(env: dict | None = None) -> str:
    """Window from HU_TRAIN_WINDOW, else config learning.training_window, else ''."""
    env = os.environ if env is None else env
    spec = (env.get("HU_TRAIN_WINDOW") or "").strip()
    if spec:
        return spec
    try:
        cfg = json.loads((Path.home() / ".human" / "config.json").read_text())
        return str(cfg.get("learning", {}).get("training_window", DEFAULT_TRAIN_WINDOW) or "")
    except (OSError, ValueError):
        return DEFAULT_TRAIN_WINDOW


def serving_base_from_ps(ps_output: str | None = None, port: str | None = None) -> str | None:
    """The --model of the PRODUCTION mlx-server, or None if it isn't running.

    Reuses the port-filtered scanner so a spare eval server (:8743/:8747) is
    never mistaken for production — the same footgun documented on
    production_mlx_port(). Distinct from resolve_serving_base_model(), which
    falls back to config when no server is up; here "no server" must read as
    None so the conflict check stays honest.
    """
    if ps_output is None:
        try:
            ps_output = subprocess.run(["ps", "-eo", "command"], capture_output=True,
                                       text=True, timeout=10).stdout
        except (OSError, subprocess.SubprocessError):
            return None
    for tokens in _iter_production_mlx_server_tokens(ps_output, port or production_mlx_port()):
        if "--model" in tokens:
            try:
                return tokens[tokens.index("--model") + 1]
            except IndexError:
                continue
    return None


def training_preflight(model: str) -> tuple[bool, str, object]:
    """Gather facts, apply training_preflight_decision, hold the lock on success.

    Returns (ok, reason, lock_handle). The caller MUST keep lock_handle alive for
    the duration of training; dropping it releases the flock.
    """
    import fcntl
    lock_handle = None
    lock_held = False
    try:
        LORA_LOCK_PATH.parent.mkdir(parents=True, exist_ok=True)
        lock_handle = open(LORA_LOCK_PATH, "w")
        fcntl.flock(lock_handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        lock_held = True  # someone else holds it
        if lock_handle:
            lock_handle.close()
        lock_handle = None

    now = datetime.now()
    ok, reason = training_preflight_decision(
        need_bytes=estimated_training_bytes(model),
        available_bytes=available_memory_bytes(),
        now_minutes=now.hour * 60 + now.minute,
        window=parse_train_window(training_window_spec()),
        lock_held=lock_held,
        serving_conflict=(serving_base_from_ps() == model and bool(model)),
    )
    if not ok and lock_handle:
        lock_handle.close()
        lock_handle = None
    return ok, reason, lock_handle

# Import the DPO quality gate module and adapter registry
import dpo_results
import adapter_registry
DATA_DIR = REPO_ROOT / "data"
ADAPTER_BASE = Path.home() / ".human" / "training-data" / "adapters"
ADAPTER_PATH = ADAPTER_BASE / "seth-lora"
FINETUNE_DIR = Path.home() / ".human" / "training-data" / "finetune"
HISTORY_PATH = REPO_ROOT / "data" / "training_history.json"

# C3 serving-base resolution (2026-07-26). Production flipped to
# GLM-4.5-Air-4bit on 2026-07-26; a hardcoded gemma base here meant every
# auto-training run produced adapters that cannot load on the serving model.
# The default remains gemma only as the last-resort fallback when neither a
# live mlx-server nor config.json can be consulted.
DEFAULT_BASE_MODEL = "mlx-community/gemma-4-31b-it-4bit"
HUMAN_CONFIG_PATH = Path.home() / ".human" / "config.json"

# Fewest resolved outcomes worth spending GPU on. Below this the 90/10
# train/valid split in run_mlx_lora_training cannot produce a valid.jsonl, so
# mlx_lm reports no Val loss, val_loss parses as None, and the regression gate
# returns INCONCLUSIVE — a verdict known before the run starts. Raise this if
# a 1-sample valid set turns out to be too thin to judge against (it is the
# floor for "splittable", not a claim about statistical adequacy).
MIN_TRAINABLE_OUTCOMES = 2


def run_script(script: str, args: list[str] | None = None, check: bool = True) -> int:
    cmd = [sys.executable, str(SCRIPTS / script)]
    if args:
        cmd.extend(args)
    print(f"\n  $ python3 scripts/{script} {' '.join(args or [])}")
    result = subprocess.run(cmd, cwd=str(REPO_ROOT))
    if check and result.returncode != 0:
        print(f"  WARNING: {script} exited with code {result.returncode}")
    return result.returncode


def load_history() -> dict:
    if HISTORY_PATH.exists():
        with open(HISTORY_PATH) as f:
            return json.load(f)
    return {"cycles": [], "best_score": 0, "best_adapter": None}


def save_history(history: dict):
    HISTORY_PATH.parent.mkdir(parents=True, exist_ok=True)
    with open(HISTORY_PATH, "w") as f:
        json.dump(history, f, indent=2)


def get_adapter_version() -> int:
    """Find the current highest adapter version."""
    version = 0
    if ADAPTER_BASE.exists():
        for d in ADAPTER_BASE.iterdir():
            if d.name.startswith("seth-lora-v"):
                try:
                    v = int(d.name.split("-v")[1])
                    version = max(version, v)
                except (ValueError, IndexError):
                    pass
    return version


def read_eval_score(eval_path: Path) -> float:
    """Read overall score from an eval results file."""
    if not eval_path.exists():
        return 0.0
    try:
        with open(eval_path) as f:
            data = json.load(f)
        if "fool_rate" in data:
            return data["fool_rate"]
        if "categories" in data:
            scores = [c["aggregate"]["overall"] for c in data["categories"].values()
                      if "aggregate" in c]
            return sum(scores) / len(scores) if scores else 0
        if "contacts" in data:
            scores = [c["aggregate"]["overall"] for c in data["contacts"].values()
                      if "aggregate" in c]
            return sum(scores) / len(scores) if scores else 0
        return 0.0
    except Exception:
        return 0.0


def phase_extract(args):
    """Phase 1: Extract fresh data from all sources."""
    print(f"\n{'='*60}")
    print(f"  Phase 1: DATA EXTRACTION")
    print(f"{'='*60}")

    run_script("extract_imessage_pairs.py", check=False)
    run_script("extract_facebook_data.py", check=False)
    run_script("extract_apple_photos.py", check=False)


def phase_merge():
    """Phase 2: Merge all data sources."""
    print(f"\n{'='*60}")
    print(f"  Phase 2: MERGE + DEDUPLICATE")
    print(f"{'='*60}")

    run_script("merge_training_sources.py")


def phase_prepare():
    """Phase 3: Generate targeted synthetic data + prepare fine-tuning data."""
    print(f"\n{'='*60}")
    print(f"  Phase 3: PREPARE FINE-TUNE DATA")
    print(f"{'='*60}")

    run_script("generate_targeted_synthetic.py", check=False)

    run_script("prepare-finetune.py", [
        "--persona", "seth",
        "--output", str(FINETUNE_DIR),
    ])


def phase_train(args) -> int:
    """Phase 4: SFT + DPO training."""
    print(f"\n{'='*60}")
    print(f"  Phase 4: FINE-TUNE (SFT + DPO)")
    print(f"{'='*60}")

    train_args = [
        "--data", str(FINETUNE_DIR),
        "--adapter-path", str(ADAPTER_PATH),
        "--iters", str(args.iters),
        "--rank", "16",
        "--max-seq-length", "2048",
        "--no-restart-server",
    ]
    if not args.no_dpo:
        train_args.append("--dpo")

    return run_script("finetune-gemma.py", train_args, check=False)


def phase_generate_data(args):
    """Phase 5: Run convo-trainer to generate new training data."""
    print(f"\n{'='*60}")
    print(f"  Phase 5: GENERATE TRAINING DATA (convo-trainer)")
    print(f"{'='*60}")

    timestamp = time.strftime("%Y%m%d-%H%M%S")
    for scenario in ["mixed", "emotional_support", "casual_hangout"]:
        output_dir = f"convo-training-{scenario}-{timestamp}"
        convo_args = [
            "--turns", str(args.convo_turns),
            "--scenario", scenario,
            "--output", output_dir,
        ]
        if args.dry_run:
            convo_args.append("--dry-run")
        convo_args.append("--verbose")

        run_script("convo-trainer.py", convo_args, check=False)


def phase_eval(args) -> dict:
    """Phase 6: Run all evals and return combined score."""
    print(f"\n{'='*60}")
    print(f"  Phase 6: EVALUATION SUITE")
    print(f"{'='*60}")

    scores = {}

    # Red team
    print(f"\n  --- Red Team ---")
    run_script("redteam_persona.py", [
        "--model-url", f"http://127.0.0.1:{args.port}/v1/chat/completions",
    ], check=False)
    redteam_path = DATA_DIR / "eval_redteam_persona.json"
    scores["redteam"] = read_eval_score(redteam_path)
    print(f"  Red team score: {scores['redteam']:.1f}")

    # Per-contact
    print(f"\n  --- Per-Contact ---")
    run_script("eval_per_contact.py", [
        "--model-url", f"http://127.0.0.1:{args.port}/v1/chat/completions",
    ], check=False)
    contact_path = DATA_DIR / "eval_per_contact.json"
    scores["per_contact"] = read_eval_score(contact_path)
    print(f"  Per-contact score: {scores['per_contact']:.1f}")

    # Blinded A/B
    print(f"\n  --- Blinded A/B ---")
    run_script("eval_blinded_ab.py", ["--mlx", "--synthetic", "--max-trials=50"], check=False)
    ab_path = DATA_DIR / "eval_blinded_ab.json"
    scores["blinded_ab"] = read_eval_score(ab_path)
    print(f"  Blinded A/B fool rate: {scores['blinded_ab']:.1f}%")

    # Combined score (weighted)
    combined = (
        scores.get("redteam", 0) * 0.3 +
        scores.get("per_contact", 0) * 0.3 +
        scores.get("blinded_ab", 0) * 0.4
    )
    scores["combined"] = round(combined, 1)

    return scores


def phase_promote_or_rollback(scores: dict, history: dict) -> bool:
    """Phase 7: Promote if improved, rollback if degraded."""
    print(f"\n{'='*60}")
    print(f"  Phase 7: PROMOTE / ROLLBACK")
    print(f"{'='*60}")

    combined = scores["combined"]
    best = history.get("best_score", 0)

    print(f"  Current score:  {combined:.1f}")
    print(f"  Previous best:  {best:.1f}")

    if combined > best:
        print(f"  IMPROVED by {combined - best:.1f} points!")
        print(f"  Promoting adapter...")
        history["best_score"] = combined
        version = get_adapter_version()
        history["best_adapter"] = f"seth-lora-v{version}"
        return True
    elif combined < best - 5:
        print(f"  DEGRADED by {best - combined:.1f} points.")
        print(f"  Rolling back to {history.get('best_adapter', 'none')}...")
        best_adapter = history.get("best_adapter")
        if best_adapter:
            best_path = ADAPTER_BASE / best_adapter
            if best_path.exists():
                if ADAPTER_PATH.exists():
                    shutil.rmtree(ADAPTER_PATH)
                shutil.copytree(best_path, ADAPTER_PATH)
                print(f"  Rolled back to {best_adapter}")
        return False
    else:
        print(f"  Within margin ({abs(combined - best):.1f} points). Keeping new adapter.")
        history["best_score"] = max(combined, best)
        version = get_adapter_version()
        history["best_adapter"] = f"seth-lora-v{version}"
        return True


def run_cycle(args, cycle_num: int, history: dict) -> dict:
    """Run a single improvement cycle."""
    print(f"\n{'#'*60}")
    print(f"  TRAINING CYCLE {cycle_num}")
    print(f"  {time.strftime('%Y-%m-%d %H:%M:%S')}")
    print(f"{'#'*60}")

    cycle_result = {
        "cycle": cycle_num,
        "timestamp": time.strftime("%Y-%m-%dT%H:%M:%S"),
        "phases": {},
    }

    t0 = time.time()

    # Extract
    if not args.skip_extract and not args.eval_only:
        phase_extract(args)
        cycle_result["phases"]["extract"] = "done"

    # Merge
    if not args.eval_only:
        phase_merge()
        cycle_result["phases"]["merge"] = "done"

    # Prepare
    if not args.eval_only:
        phase_prepare()
        cycle_result["phases"]["prepare"] = "done"

    # Train
    if not args.eval_only and not args.dry_run:
        rc = phase_train(args)
        cycle_result["phases"]["train"] = "done" if rc == 0 else f"failed ({rc})"
        if rc != 0:
            print(f"\n  Training failed. Skipping eval and promotion.")
            return cycle_result

    # Start server for eval
    if not args.dry_run:
        print(f"\n  Starting MLX server for evaluation...")
        server_cmd = [
            sys.executable, str(SCRIPTS / "mlx-server.py"),
            "--model", "mlx-community/gemma-4-31b-it-4bit",
            "--port", str(args.port),
        ]
        adapter_safetensors = ADAPTER_PATH / "adapters.safetensors"
        if adapter_safetensors.exists():
            server_cmd.extend(["--adapter-path", str(ADAPTER_PATH)])
        server = subprocess.Popen(
            server_cmd, start_new_session=True,
        )
        # Wait for server to load model and start serving
        for _ in range(30):
            time.sleep(2)
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{args.port}/health", timeout=3)
                print(f"  MLX server ready on port {args.port}")
                break
            except Exception:
                pass
        else:
            print(f"  WARNING: MLX server may not be ready (timeout after 60s)")

    # Generate training data
    if not args.eval_only and not args.skip_convo:
        phase_generate_data(args)
        cycle_result["phases"]["generate"] = "done"

    # Eval
    scores = phase_eval(args) if not args.dry_run else {
        "redteam": 60, "per_contact": 60, "blinded_ab": 40, "combined": 52,
    }
    cycle_result["scores"] = scores
    cycle_result["phases"]["eval"] = "done"

    # Kill server
    if not args.dry_run:
        server.terminate()
        try:
            server.wait(timeout=10)
        except subprocess.TimeoutExpired:
            server.kill()

    # Promote or rollback
    if not args.dry_run and not args.eval_only:
        promoted = phase_promote_or_rollback(scores, history)
        cycle_result["promoted"] = promoted

    elapsed = time.time() - t0
    cycle_result["elapsed_minutes"] = round(elapsed / 60, 1)

    # Summary
    print(f"\n{'='*60}")
    print(f"  CYCLE {cycle_num} COMPLETE")
    print(f"{'='*60}")
    print(f"  Time: {elapsed/60:.1f} minutes")
    print(f"  Scores:")
    for k, v in scores.items():
        unit = "%" if k == "blinded_ab" else "/100"
        print(f"    {k}: {v:.1f}{unit}")
    if "promoted" in cycle_result:
        status = "PROMOTED" if cycle_result["promoted"] else "ROLLED BACK"
        print(f"  Adapter: {status}")
    print(f"{'='*60}")

    return cycle_result


# ─────────────────────────────────────────────────────────────────────
# Phase C3 (2026-05-18): JSONL-driven training entry point
#
# The M3 outcome driver (scripts/m3_outcome_driver.py) accumulates a
# JSONL of selected outcomes (~/.human/training-data/m3-outcomes.jsonl)
# and wants a single subprocess to convert that into a LoRA adapter.
# That entry point lives here so there's one canonical "produce an
# adapter" path in the codebase.
#
# Privacy-by-design constraint: the JSONL holds only HASHES of prompts
# and responses (the C ring buffer never holds raw text — see B1
# privacy notes in m3_frontier_adapter.h). To train on real content
# we have to re-hash every user-turn row in the conversations DB and
# look up the matching hash.
# ─────────────────────────────────────────────────────────────────────

# FNV-1a 64-bit. Must match `hu_m3_outcome_hash_bytes` in
# src/ml/m3_frontier_adapter.c byte-for-byte. The agent on the C side
# hashes user-turn message bytes with this; we hash the same bytes
# here so a match means the same text. Tested cross-language in
# scripts/test_m3_train_from_outcomes.py.
FNV_OFFSET_BASIS_64 = 0xcbf29ce484222325
FNV_PRIME_64 = 0x100000001b3
FNV_64_MOD = 1 << 64


def read_adapter_scale(adapter_dir: Path):
    """Return lora_parameters.scale from an adapter's adapter_config.json,
    or None if the file/key is missing or unreadable.

    This is the machine-checked half of rules/lora-scale-default-or-die.md:
    what mlx_lm RECORDS is the truth about the scale the adapter was trained
    at, regardless of what any config requested."""
    try:
        cfg = json.loads((Path(adapter_dir) / "adapter_config.json").read_text())
        return cfg.get("lora_parameters", {}).get("scale")
    except (OSError, json.JSONDecodeError, AttributeError):
        return None


def fnv1a_64(data: bytes) -> int:
    """FNV-1a 64-bit hash of the given bytes. Mirrors the C
    implementation including the "0 → 1 sentinel" adjustment so the
    "no value" case in outcome.contact_id_hash distinguishes from a
    legitimate hash that happens to land on 0."""
    if not data:
        return 0
    h = FNV_OFFSET_BASIS_64
    for byte in data:
        h ^= byte
        h = (h * FNV_PRIME_64) % FNV_64_MOD
    return h if h != 0 else 1


def parse_outcomes_jsonl(path: Path) -> list[dict]:
    """Parse one outcome per line from a JSONL file. Skips blank lines
    and lines that fail to JSON-decode (logged warning, not fatal —
    a single corrupt line shouldn't poison a 1000-outcome batch)."""
    outcomes = []
    with open(path) as f:
        for line_no, line in enumerate(f, start=1):
            line = line.strip()
            if not line:
                continue
            try:
                outcomes.append(json.loads(line))
            except json.JSONDecodeError as e:
                print(f"  WARN: {path.name}:{line_no} skipping malformed JSON ({e})")
    return outcomes


def summarize_outcomes(outcomes: list[dict]) -> dict:
    """Compute statistics about an outcome batch for logging + the
    dry-run artifact's metadata block. Pure function — no I/O."""
    if not outcomes:
        return {"count": 0}
    ts = [o.get("t", 0) for o in outcomes]
    lat = [o.get("l", 0) for o in outcomes]
    pt = [o.get("pt", 0) for o in outcomes]
    ct = [o.get("ct", 0) for o in outcomes]
    model_ids = sorted({o.get("m", 0) for o in outcomes})
    adapter_ids = sorted({o.get("a", 0) for o in outcomes})
    guards = {}
    for o in outcomes:
        g = o.get("g", 0)
        guards[g] = guards.get(g, 0) + 1
    return {
        "count": len(outcomes),
        "ts_min": min(ts), "ts_max": max(ts),
        "latency_min_ms": min(lat), "latency_max_ms": max(lat),
        "latency_avg_ms": sum(lat) // max(1, len(lat)),
        "prompt_tokens_total": sum(pt),
        "completion_tokens_total": sum(ct),
        "model_ids": model_ids,
        "adapter_ids": adapter_ids,
        "guards": guards,
    }


def resolve_hashes_against_db(outcomes: list[dict], db_path: Path) -> tuple[list[dict], int]:
    """For each outcome, look up its prompt_hash in the messages table.
    Returns (resolved, skipped) where:
      resolved = list of {outcome, prompt_text, response_text or None}
      skipped  = count of outcomes whose hash had no DB match

    Hash collisions are theoretically possible (FNV-1a 64-bit space)
    but vanishingly unlikely at our scale. We take the FIRST matching
    row by id-desc (newest), which is also what the agent's record
    path saw most recently.

    DB schema (from src/memory/engines/sqlite.c):
      messages(id, session_id, role, content, created_at)
    """
    import sqlite3
    if not db_path.exists():
        return [], len(outcomes)
    conn = sqlite3.connect(str(db_path))
    # Real-world conversation DBs sometimes contain rows with non-UTF8
    # bytes (truncated emoji, mojibake from old iMessage imports). The
    # default text_factory raises on these and kills the whole scan.
    # Switch to a tolerant decoder so one bad row doesn't poison the
    # batch. The C side hashed the raw bytes the agent saw; we want to
    # hash the same bytes here, so we do the encoding round-trip
    # explicitly below.
    conn.text_factory = bytes  # type: ignore[assignment]
    try:
        cur = conn.cursor()
        # Build a hash → message-content index over user-turn rows.
        # 1209 messages at the time of writing — full scan is fine;
        # if this gets large we can persist a hash column.
        hash_index: dict[int, str] = {}
        response_index: dict[int, str] = {}  # response_hash → content
        for role_b, content_b in cur.execute("SELECT role, content FROM messages "
                                              "ORDER BY id DESC"):
            if not content_b:
                continue
            role = (role_b.decode("utf-8", "replace")
                    if isinstance(role_b, (bytes, bytearray)) else role_b)
            # Hash the RAW bytes (matches what the C side did). Decode
            # for display only — the index value is the decoded text
            # because that's what training consumes.
            content_bytes = bytes(content_b) if not isinstance(content_b, bytes) else content_b
            h = fnv1a_64(content_bytes)
            content = content_bytes.decode("utf-8", "replace")
            if role == "user" and h not in hash_index:
                hash_index[h] = content
            elif role == "assistant" and h not in response_index:
                response_index[h] = content
        resolved = []
        skipped = 0
        for o in outcomes:
            prompt_text = hash_index.get(o.get("ph", 0))
            response_text = response_index.get(o.get("rh", 0))
            if prompt_text is None:
                skipped += 1
                continue
            resolved.append({
                "outcome": o,
                "prompt_text": prompt_text,
                "response_text": response_text,  # may be None if assistant row missing
            })
        return resolved, skipped
    finally:
        conn.close()


# Lineage manifest path. JSONL — one record per produced adapter.
# Append-only so the file's an auditable history. Rotation logic at end
# of train_from_outcomes keeps the directory bounded.
LINEAGE_PATH = Path.home() / ".human" / "training-data" / "adapter_lineage.jsonl"

# Retention: keep at most N most-recent driver-produced adapters.
# Older ones are logged in the lineage manifest and then unlinked.
# 16 is enough to roll back several iterations without unbounded growth.
ADAPTER_RETENTION_LIMIT = 16


# D6 (2026-05-18): JSONL rotation. The lineage manifest is append-only;
# unbounded. We rotate when it crosses LINEAGE_ROTATE_BYTES — keep the
# most-recent half, archive the rest. The training loop's interest is
# in recent training runs; older runs are still readable from the
# rotated archive but don't load on every status call.
LINEAGE_ROTATE_BYTES = 4 * 1024 * 1024  # 4 MB → ~10k entries; well over a year of weekly trains


def rotate_jsonl_if_needed(path: Path, max_bytes: int) -> bool:
    """If `path` exceeds `max_bytes`, archive it to `<path>.<ts>` and
    truncate. Returns True if rotation happened. Atomic-ish — the
    rename + open-for-write window is brief but not zero; that's
    acceptable because losing one in-flight write at rotation time
    is preferable to unbounded growth.

    The archive name embeds the rotation timestamp so retention
    tooling can age them out later. We do NOT delete archives here —
    that's a separate operator decision."""
    if not path.exists():
        return False
    if path.stat().st_size < max_bytes:
        return False
    archive = path.with_name(f"{path.name}.{int(time.time())}")
    try:
        path.rename(archive)
        return True
    except OSError as e:
        print(f"  WARN: rotation failed for {path}: {e}")
        return False


def append_lineage_entry(entry: dict) -> None:
    """Append a one-line JSON record to the lineage manifest. Best-effort
    — failures here must NOT break the training run."""
    try:
        LINEAGE_PATH.parent.mkdir(parents=True, exist_ok=True)
        # D6: rotate when the manifest crosses 4 MB. Free space + faster
        # status reads. The rotated archive stays on disk under
        # `adapter_lineage.jsonl.<ts>` for forensic recovery.
        if rotate_jsonl_if_needed(LINEAGE_PATH, LINEAGE_ROTATE_BYTES):
            print(f"  Rotated lineage manifest (was > {LINEAGE_ROTATE_BYTES // 1024 // 1024} MB)")
        with open(LINEAGE_PATH, "a") as f:
            f.write(json.dumps(entry, default=str) + "\n")
    except OSError as e:
        print(f"  WARN: could not append lineage manifest: {e}")


def prune_old_adapters(adapter_dir: Path, keep: int = ADAPTER_RETENTION_LIMIT) -> int:
    """Delete driver-produced adapters older than the N most-recent.
    Returns count of files deleted. The lineage manifest is the
    auditable history — the actual files on disk are short-lived.

    Only touches files matching the driver's naming convention
    (`m3-driver-*.safetensors`, `m3-driver-*.bin`, `candidate-real.bin`).
    Hand-placed adapters (`seth-lora-*`) are protected from rotation."""
    if not adapter_dir.exists():
        return 0
    pruneable = []
    for p in adapter_dir.iterdir():
        # Driver-produced m3-driver-<stamp> adapters are now DIRECTORIES (each
        # holds adapters.safetensors); candidate-real.bin remains a file. Match
        # both kinds — the prior `is_file()` skip meant the new dirs never rotated.
        if p.name.startswith("m3-driver-") or p.name == "candidate-real.bin":
            pruneable.append(p)
    pruneable.sort(key=lambda f: f.stat().st_mtime, reverse=True)
    deleted = 0
    for old in pruneable[keep:]:
        try:
            if old.is_dir():
                shutil.rmtree(old)
            else:
                old.unlink()
            deleted += 1
        except OSError:
            pass
    return deleted


def write_sft_batch_jsonl(resolved: list[dict]) -> str:
    """Build an SFT JSONL batch from resolved outcomes.

    Input: list of {outcome, prompt_text, response_text} dicts
    Output: path to temporary JSONL file
    Format: one line per sample with {"text": "<prompt>\n<response>"}

    Returns the temp file path so the caller can pass it to mlx_lm.lora.
    """
    import tempfile
    tmpdir = tempfile.gettempdir()
    tmp_batch = Path(tmpdir) / f"sft-batch-{int(time.time() * 1000)}.jsonl"

    with open(tmp_batch, "w") as f:
        for item in resolved:
            prompt = item["prompt_text"]
            response = item.get("response_text") or ""
            # Join prompt and response with newline for simple single-text format
            combined = f"{prompt}\n{response}".strip()
            line = json.dumps({"text": combined})
            f.write(line + "\n")

    print(f"  Wrote {len(resolved)} samples to {tmp_batch}")
    return str(tmp_batch)


def production_mlx_port(env: dict | None = None) -> str:
    """Port of the PRODUCTION mlx-server — the daemon's post-train hot-swap
    target. Mirrors lora_training_runner.c resolve_mlx_base_url():
    HU_MLX_BASE_URL env override, else 8741.

    This filter matters: multiple mlx-servers run concurrently (observed
    2026-07-26: gemma-8bit realtime on :8747 alongside production GLM on
    :8741), and `ps` order is arbitrary — first-match would train against
    whichever server happened to be listed first.
    """
    env = os.environ if env is None else env
    url = env.get("HU_MLX_BASE_URL", "")
    m = re.search(r"//[^/]*:(\d+)", url)
    return m.group(1) if m else "8741"


def _iter_production_mlx_server_tokens(ps_output: str, port: str):
    """Yield shlex-token lists for mlx-server processes on `port` only.
    A process with no --port flag is assumed to be on the default 8741."""
    for line in ps_output.splitlines():
        if "mlx-server" not in line:
            continue
        try:
            tokens = shlex.split(line)
        except ValueError:
            continue
        line_port = "8741"
        if "--port" in tokens:
            try:
                line_port = tokens[tokens.index("--port") + 1]
            except IndexError:
                continue
        if line_port == port:
            yield tokens


def resolve_serving_base_model(
    ps_output: str | None = None,
    config_path: Path = HUMAN_CONFIG_PATH,
    override: str | None = None,
) -> tuple[str, str]:
    """Resolve the base model that is ACTUALLY serving, not a hardcoded name.

    Pattern copied from scripts/eval_fidelity_nightly.py (f66863e15): an
    adapter's delta is only meaningful against the base it was trained on.
    Training gemma while :8741 serves GLM produces dead-weight adapters
    (gemma-shaped LoRA cannot load on GLM).

    Priority:
      1. Explicit --model-id override — operator knows best.
      2. The live mlx-server process's --model argument — ground truth for
         what is serving right now.
      3. config.json mlx_local.model — what will serve after next restart.
      4. DEFAULT_BASE_MODEL as last resort.

    Args:
        ps_output: process listing to scan (injectable for tests);
                   None = run `ps ax -o command` here
        config_path: path to ~/.human/config.json (injectable for tests)
        override: explicit model id from --model-id (wins outright)

    Returns:
        (model_id, source_description)
    """
    if override:
        return override, "explicit --model-id"

    if ps_output is None:
        try:
            ps_output = subprocess.run(
                ["ps", "ax", "-o", "command"],
                capture_output=True, text=True, timeout=10,
            ).stdout
        except Exception:
            ps_output = ""

    port = production_mlx_port()
    for tokens in _iter_production_mlx_server_tokens(ps_output, port):
        try:
            candidate = tokens[tokens.index("--model") + 1]
        except (ValueError, IndexError):
            continue
        if candidate:
            return candidate, f"live mlx-server process --model (port {port})"

    try:
        config = json.loads(Path(config_path).read_text())
        raw = config.get("mlx_local", {}).get("model")
        if raw:
            return raw, "config.json mlx_local.model"
    except (OSError, json.JSONDecodeError):
        pass

    return DEFAULT_BASE_MODEL, "hardcoded default (no live server, no config)"


def resolve_serving_adapter(
    ps_output: str | None = None,
    config_path: Path = HUMAN_CONFIG_PATH,
) -> tuple[Path | None, str]:
    """Resolve the adapter that is ACTUALLY serving (reference for lineage).

    Same priority as eval_fidelity_nightly.resolve_serving_adapter: live
    mlx-server --adapter-path first, then config.json
    personalization.lora_adapter_path. Each candidate must exist on disk.

    Returns (adapter_path, source_description); (None, reason) when
    unresolvable.
    """
    if ps_output is None:
        try:
            ps_output = subprocess.run(
                ["ps", "ax", "-o", "command"],
                capture_output=True, text=True, timeout=10,
            ).stdout
        except Exception:
            ps_output = ""

    port = production_mlx_port()
    for tokens in _iter_production_mlx_server_tokens(ps_output, port):
        try:
            candidate = Path(tokens[tokens.index("--adapter-path") + 1])
        except (ValueError, IndexError):
            continue
        if candidate.exists():
            return (candidate,
                    f"live mlx-server process --adapter-path (port {port})")

    try:
        config = json.loads(Path(config_path).read_text())
        raw = config.get("personalization", {}).get("lora_adapter_path")
        if raw:
            candidate = Path(raw).expanduser()
            if candidate.exists():
                return (candidate, "config.json personalization.lora_adapter_path")
    except (OSError, json.JSONDecodeError):
        pass

    return (None, "no live mlx-server and no usable personalization.lora_adapter_path")


def base_model_tag(model_id: str) -> str:
    """Short tag identifying the base-model family, for adapter naming.

    auto-<ts>-glm vs auto-<ts>-gemma lets the registry and promotion
    tooling refuse cross-base swaps by inspecting the name alone. Model
    ids are structural strings, so plain substring matching is safe here
    (per ~/.claude/rules/substring-classifier-pitfalls.md scope).
    """
    low = model_id.lower()
    if "glm" in low:
        return "glm"
    if "gemma" in low:
        return "gemma"
    tail = low.rsplit("/", 1)[-1]
    tag = re.sub(r"[^a-z0-9]+", "-", tail).strip("-")
    return tag[:24] or "unknown"


def suffix_adapter_name(adapter_out: Path, tag: str) -> Path:
    """Append -<tag> to the adapter directory name (idempotent)."""
    if adapter_out.name.endswith(f"-{tag}"):
        return adapter_out
    return adapter_out.with_name(f"{adapter_out.name}-{tag}")


def training_config_for_model(model: str, iters: int, scale: float) -> dict:
    """Build the mlx_lm.lora YAML config for the given base model.

    mlx_lm only honors the NESTED `lora_parameters` form. The old flat
    lora_rank/lora_alpha/lora_scale keys were silently IGNORED, so scale
    fell back to mlx_lm's catastrophic 20.0 default — the proven-e2e run
    auto-manual-1785053731 trained at scale 20.0 and is dead weight
    (renamed *-INVALID-scale20-gemma on disk). Per
    ~/.claude/rules/lora-scale-default-or-die.md the scale must be pinned
    here AND verified in adapter_config.json after every real run.
    """
    config = {
        "model": model,
        "train": True,
        "fine_tune_type": "lora",
        "lora_parameters": {"rank": 8, "scale": scale, "dropout": 0.0},
        "num_layers": 8,
        "batch_size": 1,
        "iters": iters,
        "learning_rate": 1e-5,
        "steps_per_report": 50,
        "max_seq_length": 2048,
        "optimizer": "adamw",
    }
    if base_model_tag(model) == "glm":
        # Mirror the proven GLM recipe (glm-v5-config.yaml, adapter
        # seth-glm-air-v5-20260725-093742: val 7.08→2.94, 0.88 it/s,
        # peak 62.9 GB): grad checkpointing keeps the 106B MoE inside
        # memory, plain adam + fixed seed match the validated run.
        config.update({
            "grad_checkpoint": True,
            "optimizer": "adam",
            "steps_per_report": 10,
            "seed": 42,
        })
    return config


def run_mlx_lora_training(resolved: list[dict], adapter_out: Path,
                          iters: int = 500, scale: float = 2.0,
                          model: str | None = None) -> Tuple[int, Optional[float], Optional[float]]:
    """Run mlx_lm.lora training on the resolved outcomes.

    Args:
        resolved: list of {outcome, prompt_text, response_text} dicts
        adapter_out: output path for the adapter directory/file
        iters: number of training iterations (default 500, 10 for tests)
        scale: LoRA scale multiplier (default 2.0 — mlx_lm's own default is
               the catastrophic 20.0, see lora-scale-default-or-die.md)
        model: HuggingFace base model id; None falls back to
               DEFAULT_BASE_MODEL (callers should pass the SERVING base
               from resolve_serving_base_model)

    Returns: tuple of (exit_code, train_loss, val_loss)
        exit_code: process exit code (0 = success)
        train_loss: final training loss parsed from stdout (or None if unparseable)
        val_loss: final validation loss parsed from stdout (or None if unparseable)

    This function:
      1. Builds an SFT batch from resolved outcomes
      2. Creates a temp directory for mlx_lm output
      3. Creates a YAML config file with rank/scale settings
      4. Invokes mlx_lm.lora subprocess
      5. Parses stdout to extract final losses
      6. Moves the output to adapter_out
    """
    if len(resolved) == 0:
        print(f"  No resolved outcomes to train on")
        return 0, None, None

    # NOTE: callers that reach here have already passed the
    # MIN_TRAINABLE_OUTCOMES guard in train_from_outcomes. This len == 0 check
    # stays as a defensive floor for direct callers (tests import this
    # function directly), but a refusal here CANNOT block the adapter swap —
    # see the placement note on that guard for why it has to live upstream.

    # Resource preflight — the 2026-07-26 crash-loop guard. Refusing here is a
    # SUCCESS path (rc=0, no losses): a skipped retrain is a no-op, whereas
    # thrashing the machine into a reboot loses the whole session. Set
    # HU_TRAIN_SKIP_PREFLIGHT=1 to bypass (manual runs on a quiet machine).
    _preflight_lock = None
    if os.environ.get("HU_TRAIN_SKIP_PREFLIGHT", "").strip() not in ("1", "true", "yes"):
        _model_for_check = model or DEFAULT_BASE_MODEL
        _ok, _why, _preflight_lock = training_preflight(_model_for_check)
        if not _ok:
            print(f"  [preflight] REFUSING to train: {_why}")
            print(f"  [preflight] model={_model_for_check}")
            print(f"  [preflight] set HU_TRAIN_WINDOW / learning.training_window to allow a "
                  f"nightly slot, or HU_TRAIN_SKIP_PREFLIGHT=1 to override")
            return 0, None, None

    try:
        return _run_mlx_lora_training_inner(resolved, adapter_out, iters, scale, model)
    finally:
        if _preflight_lock is not None:
            _preflight_lock.close()


def _run_mlx_lora_training_inner(resolved: list[dict], adapter_out: Path,
                                 iters: int, scale: float,
                                 model: str | None) -> Tuple[int, Optional[float], Optional[float]]:
    """Body of run_mlx_lora_training, split out so the preflight lock is held
    across the whole run via try/finally rather than leaking on every return."""
    # Build SFT batch
    sft_batch = write_sft_batch_jsonl(resolved)

    # Create temp output directory for mlx_lm (it outputs a directory)
    import tempfile
    tmpdir = tempfile.mkdtemp(prefix="mlx-lora-")
    print(f"  Using temp output dir: {tmpdir}")

    # Create a temp directory with train.jsonl + valid.jsonl (mlx_lm expects
    # dir structure). The valid split matters: without it mlx_lm reports no
    # "Val loss" lines, val_loss parses as None, and the regression gate has
    # nothing to judge (the 2026-07-26 recovery run shipped exactly that).
    train_data_dir = Path(tmpdir) / "data"
    train_data_dir.mkdir(parents=True)
    _all_lines = Path(sft_batch).read_text().splitlines()
    _val = _all_lines[::10][:64]          # every 10th sample, capped
    _train = [l for i, l in enumerate(_all_lines) if i % 10 != 0]
    if not _train:                        # tiny batches: don't starve training
        _train, _val = _all_lines, []
    (train_data_dir / "train.jsonl").write_text("\n".join(_train) + "\n")
    if _val:
        (train_data_dir / "valid.jsonl").write_text("\n".join(_val) + "\n")

    # Base model + hyperparameters. Model comes from the serving resolution
    # (train_from_outcomes passes it); config keys use the nested
    # lora_parameters form mlx_lm actually honors. The post-run
    # read_adapter_scale() check below is the machine-verified other half of
    # that contract — the config only REQUESTS a scale.
    if model is None:
        model = DEFAULT_BASE_MODEL
    config = training_config_for_model(model, iters, scale)

    config_path = Path(tmpdir) / "config.yaml"
    with open(config_path, "w") as f:
        import yaml
        try:
            yaml.dump(config, f)
        except ImportError:
            # Fallback: write a simple YAML-like format
            for key, value in config.items():
                if isinstance(value, str):
                    f.write(f"{key}: '{value}'\n")
                elif isinstance(value, bool):
                    f.write(f"{key}: {str(value).lower()}\n")
                else:
                    f.write(f"{key}: {value}\n")

    # Ensure adapter output directory exists
    adapter_out.mkdir(parents=True, exist_ok=True)

    # Build mlx_lm.lora command. Scalar flags mirror the config values so
    # the two can never diverge; lora_parameters/grad_checkpoint/optimizer
    # ride in via -c (no stable CLI flags for the nested form).
    # --save-every: mlx_lm defaults to 100, and on a 56 GB MoE base each
    # checkpoint is ~556 MB — a 500-iter run left 5.4 GB of intermediates in one
    # adapter dir (measured 2026-07-26 on auto-1785091280-glm). prune_old_adapters
    # rotates whole DIRECTORIES but never the checkpoints inside one, so the
    # intermediates were pure accumulation. These runs take ~4 minutes, so
    # mid-run crash-recovery checkpoints buy almost nothing; save once at the
    # end. Overridable for long runs where resumability does matter.
    save_every = int(os.environ.get("HU_TRAIN_SAVE_EVERY") or max(1, iters))

    cmd = [
        mlx_python(), "-m", "mlx_lm", "lora",
        "--model", model,
        "--data", str(train_data_dir),  # Directory with train.jsonl + valid.jsonl
        "--adapter-path", str(adapter_out),  # Output directory (mlx_lm writes adapters.safetensors + adapter_config.json here)
        "--iters", str(iters),
        "--batch-size", str(config["batch_size"]),
        "--learning-rate", f"{config['learning_rate']:g}",
        "--max-seq-length", str(config["max_seq_length"]),
        "--steps-per-report", str(config["steps_per_report"]),
        "--save-every", str(save_every),
        "--train",
        "-c", str(config_path),
    ]

    print(f"\n  Invoking mlx_lm lora training:")
    print(f"    {' '.join(cmd)}")

    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
        rc = result.returncode

        # Print the training output to preserve visibility
        if result.stdout:
            print(result.stdout)

        # Parse losses from stdout (works regardless of exit code)
        train_loss, val_loss = dpo_results.parse_mlx_losses(result.stdout)

        if rc != 0:
            print(f"  mlx_lm lora exited with rc={rc}")
            if result.stderr:
                print(f"  stderr:\n{result.stderr}")
            return rc, train_loss, val_loss

        # Check if output exists — mlx_lm writes adapters.safetensors + adapter_config.json
        adapters_file = adapter_out / "adapters.safetensors"
        if not adapters_file.exists():
            print(f"  ERROR: mlx_lm did not produce adapters.safetensors in {adapter_out}")
            print(f"  Directory contents: {list(adapter_out.iterdir())}")
            return 1, train_loss, val_loss

        print(f"  mlx_lm lora training succeeded (train_loss={train_loss}, val_loss={val_loss})")

        # Rule-as-code (lora-scale-default-or-die): the scale the adapter was
        # ACTUALLY trained at is whatever mlx_lm wrote to adapter_config.json.
        # If it drifted from what we requested (schema change, ignored key),
        # the adapter is invalid — a scale-20 adapter destroyed production
        # instruction-following for ~2 weeks in May 2026. Fail loudly here.
        actual = read_adapter_scale(adapter_out)
        if actual is None or abs(actual - scale) > 1e-6:
            print(f"  ERROR: adapter trained at scale={actual}, requested {scale} "
                  f"— config not honored by mlx_lm; adapter is INVALID "
                  f"(see rules/lora-scale-default-or-die.md)", file=sys.stderr)
            return 1, train_loss, val_loss

        return 0, train_loss, val_loss

    except FileNotFoundError:
        print(f"  ERROR: mlx_lm not found. Install with: pip install mlx_lm")
        print(f"  Falling back to dry-run adapter")
        return 1, None, None
    finally:
        # Clean up temp SFT batch and any leftover tmpdir
        try:
            Path(sft_batch).unlink()
        except:
            pass
        try:
            if Path(tmpdir).exists():
                shutil.rmtree(tmpdir)
        except:
            pass


def write_dry_run_adapter(adapter_out: Path, summary: dict,
                          resolved_count: int, skipped_count: int) -> None:
    """Write a richer-than-placeholder safetensors-shaped artifact for
    --dry-run. Real safetensors starts with: 8-byte little-endian
    header-length, then a UTF-8 JSON header, then raw tensor bytes.
    We emit ONLY the header (zero tensors), which any safetensors
    reader will parse as an empty file — but the metadata block
    carries our training-summary so downstream tooling can show what
    WOULD have been trained.

    Reference: https://github.com/huggingface/safetensors#format
    """
    adapter_out.parent.mkdir(parents=True, exist_ok=True)
    # Safetensors header: a JSON object where the special "__metadata__"
    # key carries arbitrary string→string metadata. We stash the
    # outcome summary there.
    header = {
        "__metadata__": {
            "format": "m3-driver-dry-run-v1",
            "produced_by": "scripts/training_loop.py --source-jsonl",
            "outcome_count": str(summary.get("count", 0)),
            "resolved_count": str(resolved_count),
            "skipped_count": str(skipped_count),
            "ts_min": str(summary.get("ts_min", 0)),
            "ts_max": str(summary.get("ts_max", 0)),
            "latency_avg_ms": str(summary.get("latency_avg_ms", 0)),
            "model_ids": ",".join(str(m) for m in summary.get("model_ids", [])),
            "adapter_ids": ",".join(str(a) for a in summary.get("adapter_ids", [])),
            "guards_json": json.dumps(summary.get("guards", {})),
        }
    }
    header_bytes = json.dumps(header).encode("utf-8")
    with open(adapter_out, "wb") as f:
        f.write(len(header_bytes).to_bytes(8, "little"))
        f.write(header_bytes)
        # Zero tensor bytes — empty safetensors. A real training pass
        # (C4) writes LoRA A/B rank-decomposition tensors after the
        # header.


def train_from_outcomes(source_jsonl: Path, adapter_out: Path,
                        db_path: Path, dry_run: bool,
                        model_id_override: str | None = None) -> int:
    """Phase C3 entry point. Returns process-style exit code (0 = OK)."""
    # Resolve the SERVING base + adapter up front (2026-07-26): production
    # flipped to GLM while this path hardcoded gemma, so every auto-trained
    # adapter was un-loadable dead weight.
    model, model_source = resolve_serving_base_model(override=model_id_override)
    tag = base_model_tag(model)
    serving_adapter, serving_adapter_source = resolve_serving_adapter()

    print(f"\n{'='*60}")
    print(f"  TRAIN FROM OUTCOMES (C3)")
    print(f"{'='*60}")
    print(f"  Source JSONL: {source_jsonl}")
    print(f"  Adapter out:  {adapter_out}")
    print(f"  Conv DB:      {db_path}")
    print(f"  Dry run:      {dry_run}")
    print(f"  Base model:   {model} (via {model_source})")
    print(f"  Base tag:     {tag}")
    print(f"  Serving adapter (reference): {serving_adapter} "
          f"(via {serving_adapter_source})")
    print(f"{'='*60}")

    if not source_jsonl.exists():
        print(f"  ERROR: source JSONL not found")
        return 2

    outcomes = parse_outcomes_jsonl(source_jsonl)
    summary = summarize_outcomes(outcomes)
    print(f"  Parsed {summary['count']} outcomes")
    if summary["count"] == 0:
        print(f"  Nothing to train on — exiting cleanly")
        if dry_run:
            write_dry_run_adapter(adapter_out, summary, 0, 0)
        return 0

    print(f"  Time range:   {summary['ts_min']} → {summary['ts_max']} (ms)")
    print(f"  Latency:      {summary['latency_min_ms']}-{summary['latency_max_ms']}ms "
          f"(avg {summary['latency_avg_ms']}ms)")
    print(f"  Token totals: prompt={summary['prompt_tokens_total']} "
          f"completion={summary['completion_tokens_total']}")
    print(f"  Model ids:    {summary['model_ids']}")
    print(f"  Adapter ids:  {summary['adapter_ids']}")
    print(f"  Guard mix:    {summary['guards']}")

    resolved, skipped = resolve_hashes_against_db(outcomes, db_path)
    print(f"  Resolved:     {len(resolved)} prompt hashes against {db_path.name}")
    print(f"  Skipped:      {skipped} unresolved (conversation rotated out of DB?)")

    if dry_run:
        write_dry_run_adapter(adapter_out, summary, len(resolved), skipped)
        print(f"  Dry-run adapter written: {adapter_out} ({adapter_out.stat().st_size} bytes)")
        # D2 lineage: even dry-run adapters get logged so the trail is
        # complete. Verdict comes from the optional follow-up eval.
        append_lineage_entry({
            "timestamp": int(time.time() * 1000),
            "adapter_path": str(adapter_out),
            "size_bytes": adapter_out.stat().st_size,
            "kind": "dry-run",
            "model": model,
            "model_source": model_source,
            "base_tag": tag,
            "outcome_count": summary.get("count", 0),
            "resolved_count": len(resolved),
            "skipped_count": skipped,
            "model_ids": summary.get("model_ids", []),
            "source_jsonl": str(source_jsonl),
        })
        return 0

    # Refuse batches too small to train on, BEFORE spending any GPU. At
    # len(resolved) == 1 the 90/10 split in run_mlx_lora_training degenerates:
    # index 0 is the only index and 0 % 10 == 0, so the train side comes out
    # empty and the tiny-batch fallback drops the val split entirely. mlx_lm
    # then emits no "Val loss" line, val_loss parses as None, and the
    # regression gate returns INCONCLUSIVE — an outcome fully determined
    # before the run starts, bought with minutes-to-hours of GLM-4.5-Air GPU.
    #
    # Two placement constraints make this the only correct site:
    #
    #   1. It must be HERE, not in run_mlx_lora_training. That function's
    #      caller below treats any non-zero rc as "training failed", writes a
    #      ZERO-TENSOR dry-run adapter over adapters.safetensors, and returns
    #      0 regardless — so a refusal down there is laundered into success.
    #   2. It must return NON-ZERO. main() does sys.exit(train_from_outcomes(...)),
    #      and lora_training_runner.c:407 POSTs /v1/adapters/swap on any
    #      exit 0 with no check on adapter contents. Exiting 0 here would hot-
    #      swap an empty adapter onto the live mlx-server, stripping the
    #      persona weights off production. Non-zero makes the C dispatcher
    #      bail with HU_ERR_IO before it reaches the swap.
    if len(resolved) < MIN_TRAINABLE_OUTCOMES:
        print(f"  REFUSED: {len(resolved)} resolved outcome(s) is below the "
              f"{MIN_TRAINABLE_OUTCOMES}-outcome minimum for a train/valid split.")
        print(f"  No adapter could be judged, so none was trained and no swap "
              f"will be attempted. Exiting non-zero so the C dispatcher stops here.")
        return 1

    # Phase C3 — real LoRA training via mlx_lm.lora against the SERVING
    # base model (resolved above), producing a real rank-8 LoRA adapter.
    # Per ~/.claude/rules/lora-scale-default-or-die.md, scale MUST be 2.0
    # explicitly — mlx_lm's own default is the catastrophic 20.0 that
    # destroyed instruction-following in v3; v4-repair at scale=2.0 fixed it.
    # Hyperparameters live in training_config_for_model (GLM recipe parity
    # with the proven glm-v5-config.yaml when the resolved base is GLM).

    # The adapter directory name carries the base tag (auto-<ts>-glm vs
    # auto-<ts>-gemma) so registry/promotion tooling can refuse cross-base
    # swaps. The C dispatcher (lora_training_runner.c) hot-swaps
    # <requested_out>/adapters.safetensors after we exit, so when we rename
    # we leave a symlink at the requested path pointing at the real dir.
    requested_out = adapter_out
    adapter_out = suffix_adapter_name(adapter_out, tag)
    if adapter_out != requested_out:
        print(f"  Adapter dir renamed for base tag: {adapter_out.name}")
        try:
            requested_out.parent.mkdir(parents=True, exist_ok=True)
            if not requested_out.exists() and not requested_out.is_symlink():
                os.symlink(adapter_out.name, requested_out)
        except OSError as e:
            print(f"  WARN: could not create compat symlink {requested_out}: {e}")

    # Check for test/dry-run modes where iters should be shorter
    iters = 10 if os.environ.get("HUMAN_LORA_TEST_ITERS") else 500

    # Scale parameter enforcement: per the rule, default to 2.0
    scale = 2.0
    if scale_env := os.environ.get("HUMAN_LORA_SCALE"):
        try:
            scale = float(scale_env)
            if scale != 2.0:
                print(f"  WARN: HUMAN_LORA_SCALE={scale} overrides default 2.0.")
                print(f"        This may destroy instruction-following on 31B models.")
                print(f"        See ~/.claude/rules/lora-scale-default-or-die.md")
        except ValueError:
            print(f"  WARN: HUMAN_LORA_SCALE={scale_env} is not a valid float, using 2.0")

    rc, train_loss, val_loss = run_mlx_lora_training(resolved, adapter_out,
                                                     iters=iters, scale=scale,
                                                     model=model)

    # Check if training succeeded by looking for the safetensors file
    adapters_file = adapter_out / "adapters.safetensors"
    if rc != 0 or not adapters_file.exists():
        print(f"  mlx_lm.lora training failed (rc={rc}) or produced no adapter.")
        print(f"  Falling back to empty-tensors safetensors.")
        write_dry_run_adapter(adapters_file, summary, len(resolved), skipped)
        # Still record the failed training attempt with whatever metrics we have
        return 0

    # Get the size of the safetensors file
    size = adapters_file.stat().st_size
    print(f"  Real LoRA adapter written: {adapter_out} ({size} bytes)")

    # D2 lineage entry for the real-training success path.
    append_lineage_entry({
        "timestamp": int(time.time() * 1000),
        "adapter_path": str(adapter_out),
        "size_bytes": size,
        "kind": "mlx_lm.lora",
        "model": model,
        "model_source": model_source,
        "base_tag": tag,
        "serving_adapter_ref": str(serving_adapter) if serving_adapter else None,
        "requested_adapter_out": str(requested_out),
        "rank": 8,
        "iters": iters,
        "scale": scale,
        "batch_size": 1,
        "learning_rate": 1e-5,
        "outcome_count": summary.get("count", 0),
        "resolved_count": len(resolved),
        "skipped_count": skipped,
        "model_ids": summary.get("model_ids", []),
        "source_jsonl": str(source_jsonl),
        "memory_db": str(db_path),
    })

    # QUALITY GATE: Record training results and check regression verdict
    # This gate prevents training runs that make things worse from silently
    # shipping to inference. If regression is detected, exit non-zero so the
    # C side (lora_training_runner.c:391) sees the failure and blocks the
    # adapter swap.
    print(f"  [quality-gate] Recording training results to DPO results log...")
    results_file = Path.home() / ".human" / "logs" / "dpo-training-results.jsonl"
    n_pairs_by_source = {"outcomes": len(resolved)}

    # Warn if val_loss parsing failed — regression gate cannot judge this run.
    # This is the early heads-up only; the run is BLOCKED further down, where
    # val_loss=None routes to INCONCLUSIVE and returns non-zero. Do not turn
    # that into a fallthrough: warning-then-proceed is how the toothless
    # "PASS (val_loss=None)" verdict shipped on 2026-07-26.
    if val_loss is None:
        print(f"  [quality-gate] WARNING: val loss unparsed from training output "
              f"— regression gate cannot judge this run")

    # Append this run's results with the parsed loss metrics
    dpo_results.append_result(
        results_file,
        datetime.now().isoformat(),
        basename(str(adapter_out)),
        n_pairs_by_source,
        train_loss=train_loss,
        val_loss=val_loss,
        alignment_score=None,
        lora_scale=scale,
        iters=iters,
        git_commit=dpo_results.get_git_commit()
    )

    # Record training result to adapter registry
    try:
        adapter_registry.record_training(
            adapter_id=basename(str(adapter_out)),
            metrics={
                "n_pairs": sum(n_pairs_by_source.values()),
                "n_pairs_by_source": n_pairs_by_source,
                "train_loss": train_loss,
                "val_loss": val_loss,
                "lora_scale": scale,
                "iters": iters,
                "base_model": model,
                "base_tag": tag,
            },
            timestamp=datetime.now().isoformat()
        )
    except Exception as e:
        print(f"  [WARNING] Failed to record training to registry: {e}", file=sys.stderr)

    # Run regression verdict (checks if val_loss is worse than prior 4 weeks).
    # Absent evidence is NOT a pass: val_loss=None means the gate cannot judge,
    # and "cannot judge" must block the swap, not wave it through — the
    # toothless-gate shape from the 2026-07-11 fleet lessons resurfaced on
    # 2026-07-26 ("Regression verdict: PASS (val_loss=None)").
    history = dpo_results.load_recent(results_file)
    if val_loss is None:
        verdict = 'INCONCLUSIVE'
    else:
        verdict = dpo_results.regression_verdict(history, {'val_loss': val_loss})
    print(f"  [quality-gate] Regression verdict: {verdict} (val_loss={val_loss})")

    if verdict == 'INCONCLUSIVE':
        print(f"  [quality-gate] INCONCLUSIVE: no validation loss to judge — "
              f"adapter stays STAGED at {adapter_out}, swap blocked.")
        return 1  # Exit non-zero — blocks adapter swap in C side

    if verdict == 'FAIL':
        print(f"  [quality-gate] FAIL: Training regression detected. "
              f"Adapter will NOT be promoted to inference.")
        print(f"  [quality-gate] Adapter remains at: {adapter_out}")
        return 1  # Exit non-zero — blocks adapter swap in C side

    # D2 retention: prune older driver-produced adapters. Lineage above
    # is the auditable history; disk files beyond ADAPTER_RETENTION_LIMIT
    # are surplus.
    pruned = prune_old_adapters(adapter_out.parent)
    if pruned > 0:
        print(f"  Pruned {pruned} older adapter(s) (keeping last {ADAPTER_RETENTION_LIMIT})")

    return 0


def main():
    parser = argparse.ArgumentParser(
        description="Automated training loop for Seth persona",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--cycles", type=int, default=1, help="Number of improvement cycles (default: 1)")
    parser.add_argument("--iters", type=int, default=800, help="SFT training iterations per cycle")
    parser.add_argument("--convo-turns", type=int, default=15, help="Conversation turns per convo-trainer session")
    parser.add_argument("--port", type=int, default=8741, help="MLX server port for eval")
    parser.add_argument("--skip-extract", action="store_true", help="Skip data extraction phase")
    parser.add_argument("--skip-convo", action="store_true", help="Skip convo-trainer data generation")
    parser.add_argument("--no-dpo", action="store_true", help="Skip DPO training pass")
    parser.add_argument("--eval-only", action="store_true", help="Only run evaluation on current adapter")
    parser.add_argument("--dry-run", action="store_true", help="Simulate without actual training/eval")
    # Phase C3 — JSONL-driven entry point. When --source-jsonl is set,
    # the full cycle pipeline is bypassed and we run train_from_outcomes
    # instead. The driver (scripts/m3_outcome_driver.py) is the primary
    # caller; humans can also use this directly to iterate on the
    # selection policy.
    parser.add_argument("--source-jsonl", type=Path, default=None,
                        help="C3: JSONL of outcomes from the M3 driver. "
                             "When set, skips cycle pipeline and runs the "
                             "outcomes-driven training path.")
    parser.add_argument("--adapter-out", type=Path, default=None,
                        help="C3: directory where to write the produced LoRA adapter "
                             "(will contain adapters.safetensors + adapter_config.json). "
                             "Required when --source-jsonl is set.")
    parser.add_argument("--model-id", type=str, default=None,
                        help="C3: explicit HuggingFace base model id. "
                             "Default: resolve the SERVING base dynamically "
                             "(live mlx-server --model, then config.json "
                             "mlx_local.model, then the gemma default).")
    parser.add_argument("--memory-db", type=Path,
                        default=Path.home() / ".human" / "memory.db",
                        help="C3: path to the conversations DB for hash "
                             "resolution. Defaults to ~/.human/memory.db.")
    args = parser.parse_args()

    # C3 fast path — bypass the full cycle pipeline entirely.
    if args.source_jsonl is not None:
        if args.adapter_out is None:
            print("ERROR: --adapter-out is required when --source-jsonl is set",
                  file=sys.stderr)
            sys.exit(2)
        sys.exit(train_from_outcomes(args.source_jsonl, args.adapter_out,
                                      args.memory_db, args.dry_run,
                                      model_id_override=args.model_id))

    print(f"\n{'#'*60}")
    print(f"  h-uman AUTOMATED TRAINING LOOP")
    print(f"{'#'*60}")
    print(f"  Cycles:       {args.cycles}")
    print(f"  SFT iters:    {args.iters}")
    print(f"  Convo turns:  {args.convo_turns}")
    print(f"  DPO:          {'yes' if not args.no_dpo else 'no'}")
    print(f"  Eval only:    {args.eval_only}")
    print(f"  Dry run:      {args.dry_run}")
    print(f"{'#'*60}")

    history = load_history()
    print(f"\n  Previous cycles: {len(history['cycles'])}")
    print(f"  Best score: {history.get('best_score', 0):.1f}")
    print(f"  Best adapter: {history.get('best_adapter', 'none')}")

    for cycle_num in range(1, args.cycles + 1):
        cycle_result = run_cycle(args, len(history["cycles"]) + 1, history)
        history["cycles"].append(cycle_result)
        save_history(history)

        if cycle_num < args.cycles:
            print(f"\n  Waiting 10s before next cycle...")
            time.sleep(10)

    # Final report
    print(f"\n{'#'*60}")
    print(f"  TRAINING LOOP COMPLETE")
    print(f"{'#'*60}")
    print(f"  Cycles run: {args.cycles}")
    print(f"  Best score: {history.get('best_score', 0):.1f}")
    print(f"  Best adapter: {history.get('best_adapter', 'none')}")
    print(f"  History: {HISTORY_PATH}")

    if len(history["cycles"]) >= 2:
        recent = history["cycles"][-args.cycles:]
        scores = [c.get("scores", {}).get("combined", 0) for c in recent if c.get("scores")]
        if len(scores) >= 2:
            trend = scores[-1] - scores[0]
            direction = "improving" if trend > 2 else ("declining" if trend < -2 else "stable")
            print(f"  Trend: {direction} ({'+' if trend > 0 else ''}{trend:.1f})")

    print(f"\n  Next steps:")
    print(f"    ./scripts/start_seth_model.sh      # serve the best adapter")
    print(f"    python3 scripts/training_loop.py --cycles 3  # run more cycles")
    print(f"{'#'*60}\n")


if __name__ == "__main__":
    main()
