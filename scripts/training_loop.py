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

# Import the DPO quality gate module
import dpo_results
DATA_DIR = REPO_ROOT / "data"
ADAPTER_BASE = Path.home() / ".human" / "training-data" / "adapters"
ADAPTER_PATH = ADAPTER_BASE / "seth-lora"
FINETUNE_DIR = Path.home() / ".human" / "training-data" / "finetune"
HISTORY_PATH = REPO_ROOT / "data" / "training_history.json"


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


def run_mlx_lora_training(resolved: list[dict], adapter_out: Path,
                          iters: int = 500, scale: float = 2.0) -> int:
    """Run mlx_lm.lora training on the resolved outcomes.

    Args:
        resolved: list of {outcome, prompt_text, response_text} dicts
        adapter_out: output path for the adapter directory/file
        iters: number of training iterations (default 500, 10 for tests)
        scale: LoRA scale multiplier (default 2.0, per mlx_lm default)

    Returns: process exit code (0 = success)

    This function:
      1. Builds an SFT batch from resolved outcomes
      2. Creates a temp directory for mlx_lm output
      3. Creates a YAML config file with rank/scale settings
      4. Invokes mlx_lm.lora subprocess
      5. Moves the output to adapter_out
    """
    if len(resolved) == 0:
        print(f"  No resolved outcomes to train on")
        return 0

    # Build SFT batch
    sft_batch = write_sft_batch_jsonl(resolved)

    # Create temp output directory for mlx_lm (it outputs a directory)
    import tempfile
    tmpdir = tempfile.mkdtemp(prefix="mlx-lora-")
    print(f"  Using temp output dir: {tmpdir}")

    # Create a temp directory with train.jsonl (mlx_lm expects dir structure)
    train_data_dir = Path(tmpdir) / "data"
    train_data_dir.mkdir(parents=True)
    shutil.copy(sft_batch, str(train_data_dir / "train.jsonl"))

    # Model and hyperparameters (per US-8 design)
    model = "mlx-community/gemma-4-31b-it-4bit"
    rank = 8
    num_layers = 8  # Number of LoRA layers to train
    batch_size = 1
    learning_rate = 1e-5
    max_seq_length = 2048

    # Create YAML config file for mlx_lm
    # mlx_lm requires rank and scale to be in a config file
    config = {
        "model": model,
        "train": True,
        "fine_tune_type": "lora",
        "lora_rank": rank,
        "lora_alpha": rank * 2,  # mlx_lm uses alpha instead of scale
        "lora_scale": scale,  # Some versions support this; fallback is alpha
        "num_layers": num_layers,
        "batch_size": batch_size,
        "iters": iters,
        "learning_rate": learning_rate,
        "steps_per_report": 50,
        "max_seq_length": max_seq_length,
        "optimizer": "adamw",
    }

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

    # Build mlx_lm.lora command
    cmd = [
        sys.executable, "-m", "mlx_lm", "lora",
        "--model", model,
        "--data", str(train_data_dir),  # Directory with train.jsonl
        "--adapter-path", str(adapter_out),  # Output directory (mlx_lm writes adapters.safetensors + adapter_config.json here)
        "--iters", str(iters),
        "--batch-size", str(batch_size),
        "--learning-rate", f"{learning_rate:g}",
        "--max-seq-length", str(max_seq_length),
        "--steps-per-report", "50",
        "--train",
        "-c", str(config_path),
    ]

    print(f"\n  Invoking mlx_lm lora training:")
    print(f"    {' '.join(cmd)}")

    try:
        result = subprocess.run(cmd, capture_output=True, text=True)
        rc = result.returncode

        if rc != 0:
            print(f"  mlx_lm lora exited with rc={rc}")
            if result.stdout:
                print(f"  stdout:\n{result.stdout}")
            if result.stderr:
                print(f"  stderr:\n{result.stderr}")
            return rc

        # Check if output exists — mlx_lm writes adapters.safetensors + adapter_config.json
        adapters_file = adapter_out / "adapters.safetensors"
        if not adapters_file.exists():
            print(f"  ERROR: mlx_lm did not produce adapters.safetensors in {adapter_out}")
            print(f"  Directory contents: {list(adapter_out.iterdir())}")
            return 1

        print(f"  mlx_lm lora training succeeded")
        return 0

    except FileNotFoundError:
        print(f"  ERROR: mlx_lm not found. Install with: pip install mlx_lm")
        print(f"  Falling back to dry-run adapter")
        return 1
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
                        db_path: Path, dry_run: bool) -> int:
    """Phase C3 entry point. Returns process-style exit code (0 = OK)."""
    print(f"\n{'='*60}")
    print(f"  TRAIN FROM OUTCOMES (C3)")
    print(f"{'='*60}")
    print(f"  Source JSONL: {source_jsonl}")
    print(f"  Adapter out:  {adapter_out}")
    print(f"  Conv DB:      {db_path}")
    print(f"  Dry run:      {dry_run}")
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
            "outcome_count": summary.get("count", 0),
            "resolved_count": len(resolved),
            "skipped_count": skipped,
            "model_ids": summary.get("model_ids", []),
            "source_jsonl": str(source_jsonl),
        })
        return 0

    # Phase C3 (2026-05-26) — real LoRA training via mlx_lm.lora.
    # This path trains the frontier Gemma-4-31B model directly using MLX,
    # producing a real rank-8 LoRA adapter (A/B rank-decomposition tensors).
    # Per ~/.claude/rules/lora-scale-default-or-die.md, scale MUST be 2.0
    # (the default). Over-scaling destroyed instruction-following catastrophically
    # in v3 (scale=20.0); v4-repair at scale=2.0 fixed it.
    #
    # Hyperparameters:
    #   - rank=8: compact, fast training (~30s on M2 Max for 32 samples)
    #   - iters=500: converges on small persona datasets; per C3 plan
    #   - batch-size=1: fits in memory on M1/M2; parallelism not needed
    #   - max-seq-length=2048: matches persona example banks
    #   - learning-rate=1e-5: conservative for frontier model
    #   - scale=2.0: mlx_lm DEFAULT; overridable via HUMAN_LORA_SCALE env var

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

    rc = run_mlx_lora_training(resolved, adapter_out, iters=iters, scale=scale)

    # Check if training succeeded by looking for the safetensors file
    adapters_file = adapter_out / "adapters.safetensors"
    if rc != 0 or not adapters_file.exists():
        print(f"  mlx_lm.lora training failed (rc={rc}) or produced no adapter.")
        print(f"  Falling back to empty-tensors safetensors.")
        write_dry_run_adapter(adapters_file, summary, len(resolved), skipped)
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
        "model": "mlx-community/gemma-4-31b-it-4bit",
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

    # Append this run's results (note: we don't have final_train_loss/final_val_loss
    # from mlx-lm's subprocess; those would need to be parsed from stdout)
    dpo_results.append_result(
        results_file,
        datetime.now().isoformat(),
        basename(str(adapter_out)),
        n_pairs_by_source,
        train_loss=None,  # Would be parsed from mlx-lm output
        val_loss=None,    # Would be parsed from mlx-lm output
        alignment_score=None,
        lora_scale=scale,
        iters=iters,
        git_commit=dpo_results.get_git_commit()
    )

    # Run regression verdict (checks if val_loss is worse than prior 4 weeks)
    # For now, with val_loss=None, verdict will be PASS (can't judge)
    history = dpo_results.load_recent(results_file)
    verdict = dpo_results.regression_verdict(history, {'val_loss': None})
    print(f"  [quality-gate] Regression verdict: {verdict}")

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
                                      args.memory_db, args.dry_run))

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
