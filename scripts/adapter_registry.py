#!/usr/bin/env python3
"""
Adapter experiment registry for h-uman.

Records training, evaluation, and promotion decisions for LoRA adapters
in a structured JSON registry. Provides atomicity, append-only history,
and status reporting that flags stale evaluations and never-promoted
live adapters.

Usage:
  # Record training
  adapter_registry.record_training(
      registry_path=Path.home() / ".human" / "training-data" / "adapters" / "registry.json",
      adapter_id="seth-lora-v5",
      metrics={"n_pairs": 1000, "train_loss": 0.45, "val_loss": 0.52}
  )

  # Record evaluation
  adapter_registry.record_eval(
      registry_path=...,
      adapter_id="seth-lora-v5",
      eval_name="fidelity-nightly",
      score=0.78,
      verdict="PASS"
  )

  # Record promotion
  adapter_registry.record_promotion(
      registry_path=...,
      adapter_id="seth-lora-v5",
      evidence="blind-a-b-gate-pass"
  )

  # Show status
  print(adapter_registry.status(registry_path=..., live_adapter_id="seth-lora-v5"))

Schema (registry.json):
  {
    "schema_version": 1,
    "timestamp": "2026-07-11T...",
    "adapters": {
      "<adapter_id>": {
        "created": "2026-07-11T...",
        "training": [
          {"timestamp": "...", "metrics": {...}}
        ],
        "eval": [
          {"timestamp": "...", "eval_name": "fidelity-nightly", "score": 0.78, "verdict": "PASS"}
        ],
        "promotion": {
          "timestamp": "...",
          "evidence": "blind-a-b-gate-pass"
        },
        "demotion": {
          "timestamp": "...",
          "reason": "eval-regression"
        },
        "status": "active" | "retired" | "rejected",   # absent == "active"
        "retired_reason": "...",                        # only when status != active
        "retired_at": "..."
      }
    }
  }

Lifecycle status is a TOP-LEVEL field on the entry, not something buried in
training[*].metrics free text: readers that enumerate the registry use
iter_adapters() and skip retired/rejected entries unless they ask for them.
"""

import json
import os
import tempfile
from datetime import datetime, timedelta
from pathlib import Path


SCHEMA_VERSION = 1
DEFAULT_REGISTRY_PATH = Path.home() / ".human" / "training-data" / "adapters" / "registry.json"
STALE_EVAL_DAYS = 7

STATUS_ACTIVE = "active"
STATUS_RETIRED = "retired"      # was real, superseded / no longer a candidate
STATUS_REJECTED = "rejected"    # never valid (no-op weights, bad scale, ...)
VALID_STATUSES = (STATUS_ACTIVE, STATUS_RETIRED, STATUS_REJECTED)


def adapter_status(entry: dict) -> str:
    """An entry with no `status` key is active -- every row written before the
    field existed (2026-09-05) is a live candidate unless retired since."""
    return entry.get("status") or STATUS_ACTIVE


def is_active(entry: dict) -> bool:
    return adapter_status(entry) == STATUS_ACTIVE


def iter_adapters(registry: dict, include_retired: bool = False):
    """Yield (adapter_id, entry) in sorted order, hiding retired/rejected rows
    by default. Every enumerating reader goes through here so a retired
    adapter cannot be re-surfaced as a candidate by a reader that forgot."""
    for adapter_id in sorted(registry.get("adapters", {})):
        entry = registry["adapters"][adapter_id]
        if include_retired or is_active(entry):
            yield adapter_id, entry


def load_registry(registry_path: Path = DEFAULT_REGISTRY_PATH) -> dict:
    """Load the registry, or return a blank schema if it doesn't exist."""
    if not registry_path.exists():
        return {
            "schema_version": SCHEMA_VERSION,
            "timestamp": datetime.now().isoformat(),
            "adapters": {},
        }

    try:
        text = registry_path.read_text()
        return json.loads(text)
    except (json.JSONDecodeError, OSError):
        # Return blank schema if file is corrupted
        return {
            "schema_version": SCHEMA_VERSION,
            "timestamp": datetime.now().isoformat(),
            "adapters": {},
        }


def _save_registry_atomic(registry: dict, registry_path: Path) -> None:
    """Save registry atomically: write to tmp file, then rename."""
    registry_path.parent.mkdir(parents=True, exist_ok=True)

    # Write to a temporary file in the same directory (ensures same filesystem)
    fd, tmp_path = tempfile.mkstemp(
        suffix=".tmp",
        prefix=f".registry-{datetime.now().strftime('%s')}-",
        dir=registry_path.parent,
        text=True
    )

    try:
        # Write JSON to the temp file
        with os.fdopen(fd, "w") as f:
            json.dump(registry, f, indent=2)

        # Atomic rename
        Path(tmp_path).replace(registry_path)
    except Exception:
        # Clean up temp file on error
        try:
            Path(tmp_path).unlink()
        except FileNotFoundError:
            pass
        raise


def record_training(
    registry_path: Path = DEFAULT_REGISTRY_PATH,
    adapter_id: str = None,
    metrics: dict = None,
    timestamp: str = None,
) -> None:
    """Record a training event for an adapter.

    Args:
        registry_path: Path to registry.json
        adapter_id: Identifier for the adapter
        metrics: Dict with training metadata (n_pairs, train_loss, val_loss, etc.)
        timestamp: ISO-format timestamp (default: now)
    """
    if timestamp is None:
        timestamp = datetime.now().isoformat()

    registry = load_registry(registry_path)

    if adapter_id not in registry["adapters"]:
        registry["adapters"][adapter_id] = {
            "created": timestamp,
            "training": [],
            "eval": [],
        }

    training_record = {
        "timestamp": timestamp,
        "metrics": metrics or {},
    }

    registry["adapters"][adapter_id]["training"].append(training_record)
    registry["timestamp"] = datetime.now().isoformat()

    _save_registry_atomic(registry, registry_path)


def record_eval(
    registry_path: Path = DEFAULT_REGISTRY_PATH,
    adapter_id: str = None,
    eval_name: str = None,
    score: float = None,
    verdict: str = None,
    timestamp: str = None,
) -> None:
    """Record an evaluation result for an adapter.

    Args:
        registry_path: Path to registry.json
        adapter_id: Identifier for the adapter
        eval_name: Name of the eval (e.g., "fidelity-nightly")
        score: Numerical score from the evaluation
        verdict: Verdict string (e.g., "PASS", "FAIL", "SKIP")
        timestamp: ISO-format timestamp (default: now)
    """
    if timestamp is None:
        timestamp = datetime.now().isoformat()

    registry = load_registry(registry_path)

    if adapter_id not in registry["adapters"]:
        registry["adapters"][adapter_id] = {
            "created": timestamp,
            "training": [],
            "eval": [],
        }

    eval_record = {
        "timestamp": timestamp,
        "eval_name": eval_name,
        "score": score,
        "verdict": verdict,
    }

    registry["adapters"][adapter_id]["eval"].append(eval_record)
    registry["timestamp"] = datetime.now().isoformat()

    _save_registry_atomic(registry, registry_path)


def record_promotion(
    registry_path: Path = DEFAULT_REGISTRY_PATH,
    adapter_id: str = None,
    evidence: str = None,
    timestamp: str = None,
) -> None:
    """Record a promotion event for an adapter.

    Args:
        registry_path: Path to registry.json
        adapter_id: Identifier for the adapter
        evidence: Gate/evidence that justified promotion (e.g., "blind-a-b-gate-pass")
        timestamp: ISO-format timestamp (default: now)
    """
    if timestamp is None:
        timestamp = datetime.now().isoformat()

    registry = load_registry(registry_path)

    if adapter_id not in registry["adapters"]:
        registry["adapters"][adapter_id] = {
            "created": timestamp,
            "training": [],
            "eval": [],
        }

    promotion_record = {
        "timestamp": timestamp,
        "evidence": evidence,
    }

    registry["adapters"][adapter_id]["promotion"] = promotion_record
    registry["timestamp"] = datetime.now().isoformat()

    _save_registry_atomic(registry, registry_path)


def record_demotion(
    registry_path: Path = DEFAULT_REGISTRY_PATH,
    adapter_id: str = None,
    reason: str = None,
    timestamp: str = None,
) -> None:
    """Record a demotion event for an adapter.

    Args:
        registry_path: Path to registry.json
        adapter_id: Identifier for the adapter
        reason: Reason for demotion (e.g., "eval-regression")
        timestamp: ISO-format timestamp (default: now)
    """
    if timestamp is None:
        timestamp = datetime.now().isoformat()

    registry = load_registry(registry_path)

    if adapter_id not in registry["adapters"]:
        registry["adapters"][adapter_id] = {
            "created": timestamp,
            "training": [],
            "eval": [],
        }

    demotion_record = {
        "timestamp": timestamp,
        "reason": reason,
    }

    registry["adapters"][adapter_id]["demotion"] = demotion_record
    registry["timestamp"] = datetime.now().isoformat()

    _save_registry_atomic(registry, registry_path)


def record_retirement(
    registry_path: Path = DEFAULT_REGISTRY_PATH,
    adapter_id: str = None,
    status: str = STATUS_RETIRED,
    reason: str = None,
    timestamp: str = None,
) -> None:
    """Mark an adapter retired or rejected. Additive: training/eval/promotion
    history is left in place so the row still explains what happened.

    Unlike the record_* writers above this REFUSES to create an entry --
    retiring an adapter the registry never heard of would fabricate history.

    Raises:
        ValueError: status is not retired/rejected, or reason is empty
        KeyError: adapter_id is not in the registry
    """
    if status not in (STATUS_RETIRED, STATUS_REJECTED):
        raise ValueError(f"status must be one of {STATUS_RETIRED!r}/{STATUS_REJECTED!r}, got {status!r}")
    if not reason or not reason.strip():
        raise ValueError("a retirement needs a reason -- an unexplained retired row is as useless as a no-op adapter")
    if timestamp is None:
        timestamp = datetime.now().isoformat()

    registry = load_registry(registry_path)
    if adapter_id not in registry["adapters"]:
        raise KeyError(f"adapter {adapter_id!r} is not in the registry; refusing to invent an entry to retire")

    entry = registry["adapters"][adapter_id]
    entry["status"] = status
    entry["retired_reason"] = reason.strip()
    entry["retired_at"] = timestamp
    registry["timestamp"] = datetime.now().isoformat()

    _save_registry_atomic(registry, registry_path)


def status(
    registry_path: Path = DEFAULT_REGISTRY_PATH,
    live_adapter_id: str = None,
    include_retired: bool = False,
) -> str:
    """Generate a human-readable status report.

    Flags:
      - Adapters with no eval in the last 7 days
      - Live adapters with no promotion record
      - Live adapters with no eval record

    Args:
        registry_path: Path to registry.json
        live_adapter_id: ID of the currently-live adapter (read from config if None)
        include_retired: also list retired/rejected adapters (hidden by default;
            the live adapter is always shown, retired or not, because a retired
            row serving in production is exactly the anomaly this report exists for)

    Returns:
        Formatted status report as a string
    """
    registry = load_registry(registry_path)

    # If live_adapter_id not provided, try to read from config
    if live_adapter_id is None:
        try:
            config_path = Path.home() / ".human" / "config.json"
            if config_path.exists():
                config = json.loads(config_path.read_text())
                live_adapter_id = config.get("personalization", {}).get("lora_adapter_id")
        except Exception:
            pass

    lines = []
    lines.append("=" * 80)
    lines.append("ADAPTER REGISTRY STATUS")
    lines.append(f"Registry timestamp: {registry.get('timestamp', 'unknown')}")
    lines.append(f"Live adapter ID: {live_adapter_id or '(not configured)'}")
    lines.append("=" * 80)

    if live_adapter_id and live_adapter_id not in registry["adapters"]:
        lines.append(f"⚠️  WARNING: live adapter '{live_adapter_id}' has NO registry entry — "
                     "no recorded training metrics, eval, or promotion evidence. "
                     "It is serving on trust, not measurement.")

    if not registry["adapters"]:
        lines.append("No adapters in registry.")
        return "\n".join(lines)

    now = datetime.now()
    stale_threshold = now - timedelta(days=STALE_EVAL_DAYS)

    hidden = 0
    for adapter_id in sorted(registry["adapters"].keys()):
        adapter = registry["adapters"][adapter_id]

        is_live = adapter_id == live_adapter_id
        lifecycle = adapter_status(adapter)
        if lifecycle != STATUS_ACTIVE and not include_retired and not is_live:
            hidden += 1
            continue

        live_marker = " [LIVE]" if is_live else ""
        status_marker = f" [{lifecycle.upper()}]" if lifecycle != STATUS_ACTIVE else ""

        lines.append("")
        lines.append(f"Adapter: {adapter_id}{live_marker}{status_marker}")
        lines.append(f"  Created: {adapter.get('created', 'unknown')}")
        if lifecycle != STATUS_ACTIVE:
            lines.append(f"  Status: {lifecycle} at {adapter.get('retired_at', 'unknown')}")
            lines.append(f"    Reason: {adapter.get('retired_reason', '(none recorded)')}")
            if is_live:
                lines.append(f"    ⚠️  WARNING: LIVE ADAPTER IS MARKED {lifecycle.upper()} -- "
                             "production is serving a row the registry says is dead")

        # Training history
        training = adapter.get("training", [])
        if training:
            latest_train = training[-1]
            lines.append(f"  Latest training: {latest_train['timestamp']}")
            if latest_train.get("metrics"):
                for k, v in latest_train["metrics"].items():
                    lines.append(f"    {k}: {v}")
        else:
            lines.append("  Training: (none recorded)")

        # Eval history
        eval_records = adapter.get("eval", [])
        if eval_records:
            latest_eval = eval_records[-1]
            latest_eval_time = datetime.fromisoformat(latest_eval["timestamp"])
            lines.append(f"  Latest eval: {latest_eval['timestamp']}")
            lines.append(f"    Name: {latest_eval.get('eval_name', '?')}")
            lines.append(f"    Score: {latest_eval.get('score', '?')}")
            lines.append(f"    Verdict: {latest_eval.get('verdict', '?')}")

            # Flag stale evals
            if latest_eval_time < stale_threshold:
                days_old = (now - latest_eval_time).days
                lines.append(f"    ⚠️  WARNING: eval is {days_old} days old (threshold: {STALE_EVAL_DAYS} days)")
        else:
            lines.append("  Eval: (none recorded)")

        # Promotion history
        promotion = adapter.get("promotion")
        if promotion:
            lines.append(f"  Promotion: {promotion['timestamp']}")
            lines.append(f"    Evidence: {promotion.get('evidence', '(none)')}")
        else:
            if is_live:
                lines.append("  Promotion: ⚠️  WARNING: LIVE ADAPTER WITH NO PROMOTION RECORD")
            else:
                lines.append("  Promotion: (none recorded)")

        # Demotion history
        demotion = adapter.get("demotion")
        if demotion:
            lines.append(f"  Demotion: {demotion['timestamp']}")
            lines.append(f"    Reason: {demotion.get('reason', '(none)')}")

    if hidden:
        lines.append("")
        lines.append(f"({hidden} retired/rejected adapter{'s' if hidden != 1 else ''} hidden; "
                     "pass --include-retired to list)")

    lines.append("")
    lines.append("=" * 80)

    return "\n".join(lines)


def main():
    """CLI for adapter registry."""
    import argparse

    parser = argparse.ArgumentParser(description="Adapter experiment registry")
    parser.add_argument(
        "--registry",
        type=Path,
        default=DEFAULT_REGISTRY_PATH,
        help=f"Path to registry.json (default: {DEFAULT_REGISTRY_PATH})",
    )

    sub = parser.add_subparsers(dest="cmd")

    p_status = sub.add_parser("status", help="Show adapter status")
    p_status.add_argument("--live-adapter-id", help="ID of the live adapter")
    p_status.add_argument("--include-retired", action="store_true",
                          help="also list retired/rejected adapters")

    args = parser.parse_args()

    if args.cmd == "status" or not args.cmd:
        output = status(registry_path=args.registry,
                        live_adapter_id=getattr(args, "live_adapter_id", None),
                        include_retired=getattr(args, "include_retired", False))
        print(output)
        return 0

    parser.print_help()
    return 0


if __name__ == "__main__":
    import sys
    sys.exit(main())
