#!/usr/bin/env python3
"""
M3 Outcome Driver — closes the dormant LoRA loop (B3 v1).

Polls the human daemon's GET /v1/m3/outcomes endpoint for new inference
outcomes since a stored watermark, applies a user-defined selection
policy, deduplicates by prompt_hash, and appends accepted outcomes to a
JSONL training-data file. A separate downstream step (training_loop.py
or a manual kick) consumes the JSONL.

Architecture sits between two existing pieces:

    daemon ring (B1, src/ml/m3_frontier_adapter.c)
        ↓ GET /v1/m3/outcomes?turn_kind=1&since_ms=<watermark>
    THIS DRIVER (poll + select + dedup + append)
        ↓ ~/.human/training-data/m3-outcomes.jsonl
    training_loop.py / mlx_lora_entry.py (existing)
        ↓ POST /v1/adapters/swap
    MLX server hot-loads the new adapter (B2 Stream B, scripts/mlx-server.py)

Usage:
    python3 scripts/m3_outcome_driver.py                 # one poll pass
    python3 scripts/m3_outcome_driver.py --dry-run       # poll but don't write
    python3 scripts/m3_outcome_driver.py --since 0       # backfill from epoch
    python3 scripts/m3_outcome_driver.py --gateway URL   # override default

Env:
    HUMAN_GATEWAY_URL   default http://127.0.0.1:3006
    HUMAN_API_TOKEN     bearer token if gateway has auth_token set

Idempotent: rerunning the driver after a successful pass is a no-op if
no new outcomes have landed in the ring. The watermark file lives at
~/.human/m3_driver_state.json and survives daemon restarts.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path
from typing import Iterable

# ─────────────────────────────────────────────────────────────────────
# Paths and defaults
# ─────────────────────────────────────────────────────────────────────

DEFAULT_GATEWAY = os.environ.get("HUMAN_GATEWAY_URL", "http://127.0.0.1:3006")
AUTH_TOKEN = os.environ.get("HUMAN_API_TOKEN", "")

HUMAN_HOME = Path.home() / ".human"
STATE_PATH = HUMAN_HOME / "m3_driver_state.json"
OUTCOMES_JSONL = HUMAN_HOME / "training-data" / "m3-outcomes.jsonl"


def load_state() -> dict:
    """Watermark + seen-hash set. JSON file, safe to delete to force backfill."""
    if not STATE_PATH.exists():
        return {"last_ts_ms": 0, "seen_prompt_hashes": []}
    try:
        return json.loads(STATE_PATH.read_text())
    except (json.JSONDecodeError, OSError):
        return {"last_ts_ms": 0, "seen_prompt_hashes": []}


def save_state(state: dict) -> None:
    STATE_PATH.parent.mkdir(parents=True, exist_ok=True)
    # Atomic write: tmp + rename (same pattern as personal_model.c save).
    tmp = STATE_PATH.with_suffix(".tmp")
    tmp.write_text(json.dumps(state, indent=2))
    tmp.replace(STATE_PATH)


# ─────────────────────────────────────────────────────────────────────
# Gateway poll
# ─────────────────────────────────────────────────────────────────────

def poll_outcomes(gateway: str, since_ms: int, turn_kind: int = 1,
                  limit: int = 4096) -> list[dict]:
    """Fetch outcomes from /v1/m3/outcomes. Returns parsed JSONL records.

    NDJSON response (one outcome per line) — handles empty body correctly.
    """
    url = (f"{gateway.rstrip('/')}/v1/m3/outcomes"
           f"?since_ms={since_ms}&turn_kind={turn_kind}&limit={limit}")
    req = urllib.request.Request(url)
    if AUTH_TOKEN:
        req.add_header("Authorization", f"Bearer {AUTH_TOKEN}")
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            body = resp.read().decode("utf-8")
    except urllib.error.HTTPError as e:
        print(f"[m3-driver] gateway {gateway} returned HTTP {e.code}: {e.reason}",
              file=sys.stderr)
        sys.exit(2)
    except urllib.error.URLError as e:
        print(f"[m3-driver] could not reach gateway {gateway}: {e.reason}",
              file=sys.stderr)
        sys.exit(2)

    if not body.strip():
        return []
    out: list[dict] = []
    for line in body.splitlines():
        line = line.strip()
        if line:
            out.append(json.loads(line))
    return out


# ─────────────────────────────────────────────────────────────────────
# Selection policy — USER CONTRIBUTION GOES HERE
# ─────────────────────────────────────────────────────────────────────

# Selection-policy constants. Tunable, but the defaults follow the
# agent-tuner playbook's "narrow then loosen" principle: start strict,
# loosen only after measuring.
GUARD_PASS = 1
GUARD_REWRITE = 2
ACCEPTED_GUARD_DECISIONS = {GUARD_PASS}  # add GUARD_REWRITE only when the
                                          # rewrite target is captured separately
MIN_LATENCY_MS = 50      # below this → cached/stub path; not representative
MAX_LATENCY_MS = 30_000  # above this → cold-start outlier; not representative
PER_CONTACT_CAP = 64     # max samples per contact in one selection pass


def select_training_outcomes(outcomes: list[dict]) -> list[dict]:
    """Decide which outcomes are training-worthy.

    INPUT keys (from src/ml/m3_frontier_adapter.c::hu_m3_outcomes_to_jsonl):
        "t":  timestamp_unix_ms   "l":  latency_ms
        "ph": prompt_hash         "rh": response_hash
        "ch": contact_id_hash     "pt": prompt_tokens
        "ct": completion_tokens   "m":  model_id
        "a":  adapter_id (0 = base)
        "g":  guard_decision (0=UNKNOWN 1=PASS 2=REWRITE 3=REJECT)
        "k":  turn_kind (1=stream 2=batch 3=proactive)

    Policy applied here:
      1. Guard PASS only — drop REWRITE / REJECT / UNKNOWN. The model never
         learns to imitate output that the guard had to fix; the rewrite
         CORPUS lives in a separate channel (planned: B3 v2).
      2. Base-model only (adapter_id == 0) — prevents the self-amplification
         feedback loop where each adapter overfits to its predecessor.
      3. Latency window [MIN, MAX] — drops cached/stub paths and cold-start
         outliers that don't represent normal inference.
      4. Both prompt and completion must have at least one token — degenerate
         empty turns are skipped.
      5. Per-contact cap (PER_CONTACT_CAP) — protects against the chattiest
         contact dominating the LoRA. Cap=64 means ~4× the contact volume
         needed to make a meaningful per-relationship signal without letting
         one contact swamp the dataset.

    Outcomes are processed oldest-first (matches the server's snapshot
    order), so when capping we keep the OLDEST samples per contact — that
    way the dataset's contact distribution reflects the natural cadence,
    not just whoever happened to message last.
    """
    accepted: list[dict] = []
    per_contact: dict[int, int] = {}
    for o in outcomes:
        if o.get("g", 0) not in ACCEPTED_GUARD_DECISIONS:
            continue
        if o.get("a", 0) != 0:
            continue
        latency = o.get("l", 0)
        if latency < MIN_LATENCY_MS or latency > MAX_LATENCY_MS:
            continue
        if o.get("pt", 0) <= 0 or o.get("ct", 0) <= 0:
            continue
        ch = o.get("ch", 0)
        if per_contact.get(ch, 0) >= PER_CONTACT_CAP:
            continue
        per_contact[ch] = per_contact.get(ch, 0) + 1
        accepted.append(o)
    return accepted


# ─────────────────────────────────────────────────────────────────────
# Dedup + append
# ─────────────────────────────────────────────────────────────────────

# D6 (2026-05-18): rotation threshold for the outcomes JSONL. The
# selection policy already aggressively filters incoming outcomes
# (PASS only, base adapter only, latency window, token threshold,
# per-contact cap) so growth is slow — but unbounded over months. 8 MB
# = ~25k outcomes at 320 B/line, which is well past the threshold
# where training_loop.py wants to consume a "recent window" rather
# than the full corpus.
OUTCOMES_ROTATE_BYTES = 8 * 1024 * 1024


def rotate_outcomes_if_needed() -> bool:
    """If the outcomes JSONL exceeds OUTCOMES_ROTATE_BYTES, archive it.
    Returns True if rotation happened. Errors are non-fatal — the
    driver MUST continue functioning even if rotation fails."""
    if not OUTCOMES_JSONL.exists():
        return False
    if OUTCOMES_JSONL.stat().st_size < OUTCOMES_ROTATE_BYTES:
        return False
    archive = OUTCOMES_JSONL.with_name(
        f"{OUTCOMES_JSONL.name}.{int(time.time())}"
    )
    try:
        OUTCOMES_JSONL.rename(archive)
        return True
    except OSError:
        return False


def dedup_and_append(selected: list[dict], seen_hashes: set[int],
                     dry_run: bool) -> tuple[int, int]:
    """Skip outcomes whose prompt_hash we've already trained on. Append the
    rest as JSONL. Returns (appended_count, skipped_dup_count).

    Dedup is by prompt_hash only — two responses to the same prompt count
    as ONE training sample (the latest wins implicitly; we keep the first
    one we see in the polling order, which is oldest-first).
    """
    appended = 0
    skipped = 0
    if not dry_run:
        OUTCOMES_JSONL.parent.mkdir(parents=True, exist_ok=True)
        # D6: rotate BEFORE appending so the next write starts fresh.
        # Doesn't fire on the hot path — only when the JSONL has grown
        # past the rotation threshold.
        if rotate_outcomes_if_needed():
            print(f"[m3-driver] rotated outcomes JSONL "
                  f"(was > {OUTCOMES_ROTATE_BYTES // 1024 // 1024} MB)")
    with open(OUTCOMES_JSONL, "a") if not dry_run else _devnull() as f:
        for outcome in selected:
            ph = outcome.get("ph", 0)
            if ph in seen_hashes:
                skipped += 1
                continue
            seen_hashes.add(ph)
            f.write(json.dumps(outcome) + "\n")
            appended += 1
    return appended, skipped


class _devnull:
    """Context manager that swallows writes — used in --dry-run mode."""
    def __enter__(self): return self
    def __exit__(self, *args): return False
    def write(self, _): pass


# ─────────────────────────────────────────────────────────────────────
# Training trigger + adapter swap (closes the loop)
# ─────────────────────────────────────────────────────────────────────

ADAPTER_OUT_DIR = HUMAN_HOME / "training-data" / "adapters"
MLX_SERVER_URL = os.environ.get("HUMAN_MLX_URL", "http://127.0.0.1:8741")
DEFAULT_TRAIN_THRESHOLD = 32  # outcomes-in-JSONL threshold to kick a train


def jsonl_sample_count(path: Path) -> int:
    """Count non-empty lines in the outcomes JSONL. 0 if file missing."""
    if not path.exists():
        return 0
    n = 0
    with open(path) as f:
        for line in f:
            if line.strip():
                n += 1
    return n


def run_training(samples: int, simulate: bool) -> Path:
    """Kick off a LoRA fine-tune. In --simulate-train mode, writes a tiny
    placeholder safetensors-shaped file in seconds so the loop is testable
    without GPU time. Real mode hands off to training_loop.py.

    Returns the path of the new adapter artifact.
    """
    ADAPTER_OUT_DIR.mkdir(parents=True, exist_ok=True)
    stamp = int(time.time())
    out_path = ADAPTER_OUT_DIR / f"m3-driver-{stamp}"

    if simulate:
        # Placeholder. The real artifact is an MLX-compatible safetensors
        # checkpoint stored in {out_path}/adapters.safetensors (per training_loop.py US-8
        # Phase C3 contract). This fake file lets the swap call exercise its
        # path-validation code without spending wall-clock on training.
        out_path.mkdir(parents=True, exist_ok=True)
        fake_adapter = out_path / "adapters.safetensors"
        fake_adapter.write_bytes(b"FAKE_MLX_LORA_ADAPTER_FROM_M3_DRIVER_v0\n"
                                + f"trained_on_samples={samples}\n".encode()
                                + f"timestamp={stamp}\n".encode())
        print(f"[m3-driver] (simulate-train) wrote placeholder adapter "
              f"({fake_adapter.stat().st_size} bytes) → {out_path}")
        return out_path

    # Real training. Delegates to training_loop.py — that script knows how
    # to discover the JSONL, merge with prior corpora, run SFT, and emit
    # the adapter. Do NOT inline its logic here; it's deliberately separate
    # so this driver stays under 400 lines.
    import subprocess
    script = Path(__file__).resolve().parent / "training_loop.py"
    cmd = [sys.executable, str(script),
           "--source-jsonl", str(OUTCOMES_JSONL),
           "--adapter-out", str(out_path)]
    print(f"[m3-driver] launching real training: {' '.join(cmd)}")
    rc = subprocess.call(cmd)
    if rc != 0:
        raise RuntimeError(f"training_loop.py exited with rc={rc}")
    if not out_path.exists():
        raise RuntimeError(f"training did not produce adapter at {out_path}")
    return out_path


def swap_adapter(mlx_url: str, adapter_path: Path) -> tuple[bool, str]:
    """POST /v1/adapters/swap on the MLX server. Returns (ok, detail).

    Soft-fail if the MLX server is not reachable — that's expected on
    machines without a loaded MLX model. The driver's job is to PRODUCE
    the adapter; whether it gets loaded is the server's responsibility.
    """
    url = f"{mlx_url.rstrip('/')}/v1/adapters/swap"
    body = json.dumps({"adapter_path": str(adapter_path)}).encode("utf-8")
    req = urllib.request.Request(url, data=body, method="POST",
                                 headers={"Content-Type": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            payload = json.loads(resp.read().decode("utf-8"))
        ok = resp.status == 200
        return ok, json.dumps(payload)
    except urllib.error.HTTPError as e:
        return False, f"HTTP {e.code}: {e.reason}"
    except urllib.error.URLError as e:
        # Soft-fail: server probably not running. Print clearly.
        return False, f"MLX server unreachable at {mlx_url}: {e.reason}"


# ─────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────

def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--gateway", default=DEFAULT_GATEWAY,
                    help="Gateway base URL (default %(default)s)")
    ap.add_argument("--since", type=int, default=None,
                    help="Override watermark (ms since epoch). 0 = full backfill.")
    ap.add_argument("--turn-kind", type=int, default=0,
                    help="Filter outcomes by turn_kind (0=any, 1=stream, "
                         "2=batch, 3=proactive). Default 0 — both stream "
                         "(channel messages) and batch (gateway /v1/chat/"
                         "completions) are legitimate training signal; "
                         "filtering to one kind excludes a major workload.")
    ap.add_argument("--limit", type=int, default=4096,
                    help="Max outcomes per poll (server caps at ring size)")
    ap.add_argument("--dry-run", action="store_true",
                    help="Poll + select but do not append to JSONL or save state")
    ap.add_argument("--run-loop", action="store_true",
                    help="After append, kick off training + adapter swap if "
                         "the JSONL has >= --threshold samples")
    ap.add_argument("--threshold", type=int, default=DEFAULT_TRAIN_THRESHOLD,
                    help="Min samples in JSONL before --run-loop trains "
                         "(default %(default)s)")
    ap.add_argument("--simulate-train", action="store_true",
                    help="With --run-loop: produce a placeholder adapter "
                         "instead of running real LoRA training (for testing)")
    ap.add_argument("--mlx-url", default=MLX_SERVER_URL,
                    help="MLX server URL for /v1/adapters/swap (default %(default)s)")
    args = ap.parse_args()

    state = load_state()
    since_ms = args.since if args.since is not None else state["last_ts_ms"]
    seen_hashes: set[int] = set(state.get("seen_prompt_hashes", []))

    print(f"[m3-driver] polling {args.gateway} since_ms={since_ms} "
          f"turn_kind={args.turn_kind} dry_run={args.dry_run}")
    t0 = time.time()
    raw = poll_outcomes(args.gateway, since_ms, args.turn_kind, args.limit)
    poll_ms = int((time.time() - t0) * 1000)
    print(f"[m3-driver] fetched {len(raw)} outcomes in {poll_ms}ms")

    if not raw:
        print("[m3-driver] no new outcomes — exit")
        return 0

    selected = select_training_outcomes(raw)
    appended, skipped = dedup_and_append(selected, seen_hashes, args.dry_run)

    # Advance watermark to newest outcome's ts (raw is oldest-first per server).
    new_high = max(o.get("t", 0) for o in raw)
    print(f"[m3-driver] selected={len(selected)} appended={appended} "
          f"dedup_skipped={skipped} new_watermark={new_high}")

    if not args.dry_run:
        # Cap the seen-hash list to avoid unbounded growth — last 100k is
        # plenty for dedup over the ring's 4096-record window.
        seen_list = list(seen_hashes)[-100_000:]
        save_state({"last_ts_ms": new_high, "seen_prompt_hashes": seen_list})
        print(f"[m3-driver] state saved → {STATE_PATH}")
        print(f"[m3-driver] outcomes appended → {OUTCOMES_JSONL}")

    # ── Optional: close the loop ──────────────────────────────────────
    if args.run_loop and not args.dry_run:
        total = jsonl_sample_count(OUTCOMES_JSONL)
        print(f"[m3-driver] JSONL holds {total} samples (threshold={args.threshold})")
        if total < args.threshold:
            print("[m3-driver] below threshold — skip train + swap")
            return 0
        try:
            adapter_path = run_training(total, simulate=args.simulate_train)
        except Exception as e:
            print(f"[m3-driver] training failed: {e}", file=sys.stderr)
            return 3
        ok, detail = swap_adapter(args.mlx_url, adapter_path)
        if ok:
            print(f"[m3-driver] adapter swap OK: {detail}")
        else:
            # Soft-fail: producer's job is done; consumer (MLX server) is
            # often offline in tests. Make this loud but non-fatal.
            print(f"[m3-driver] adapter swap skipped/failed: {detail}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
