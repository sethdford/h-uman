#!/usr/bin/env python3
"""
Phase G2 (2026-05-18) — adapter promotion CLI with rollback.

Operator surface for "load this LoRA into the live MLX server."
Today's automation (the m3_outcome_driver's threshold-trigger) does
swaps without human-in-the-loop. This CLI gives you:

  - Explicit `promote` — pick an adapter, swap it live, record the
    promotion in the lineage manifest
  - Explicit `rollback` — revert to the previous adapter, recorded
    in the lineage manifest
  - `current` — show what's loaded right now
  - `--dry-run` — print the swap call but don't execute

Safety:
  - Reads the current adapter via GET /v1/adapters/current BEFORE
    swapping so rollback knows where to go
  - Records {timestamp, action, from, to, verdict} in the lineage
    manifest so you have an audit trail
  - Soft-fail on MLX server unreachable (exit 2 with clear message)
  - Never swaps without --yes for production paths (any path
    containing "prod" or matching $HUMAN_PRODUCTION_ADAPTERS_REGEX)

Usage:
    python3 scripts/m3_promote.py current
    python3 scripts/m3_promote.py promote --adapter /path/to/lora --yes
    python3 scripts/m3_promote.py rollback --yes
    make m3-promote ADAPTER=/path

Exit codes:
    0 — success (swap landed or rollback succeeded)
    2 — MLX server unreachable / bad input
    3 — confirmation required but not provided
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
from pathlib import Path

HUMAN_HOME = Path.home() / ".human"
LINEAGE_PATH = HUMAN_HOME / "training-data" / "adapter_lineage.jsonl"
DEFAULT_MLX_URL = os.environ.get("HUMAN_MLX_URL", "http://127.0.0.1:8741")
PROD_REGEX = re.compile(os.environ.get("HUMAN_PRODUCTION_ADAPTERS_REGEX",
                                        r"prod|production"))


def http_json(method: str, url: str, body: dict | None = None, timeout: int = 30):
    """Return (status_code, parsed_body_or_None). HTTP errors return
    their status with the parsed body when available; transport errors
    raise URLError."""
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode()
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read().decode())
    except urllib.error.HTTPError as e:
        try:
            payload = json.loads(e.read().decode())
        except Exception:
            payload = None
        return e.code, payload


def get_current_adapter(mlx_url: str) -> str | None:
    """GET /v1/adapters/current. Returns adapter_path or None."""
    try:
        status, body = http_json("GET", f"{mlx_url.rstrip('/')}/v1/adapters/current")
    except (urllib.error.URLError, OSError) as e:
        print(f"  ERROR: MLX server unreachable at {mlx_url}: {e}", file=sys.stderr)
        return None
    if status != 200 or not body:
        return None
    return body.get("adapter_path")


def swap_adapter(mlx_url: str, adapter_path: str) -> tuple[bool, dict]:
    """POST /v1/adapters/swap. Returns (success, response_body)."""
    try:
        status, body = http_json("POST", f"{mlx_url.rstrip('/')}/v1/adapters/swap",
                                  body={"adapter_path": adapter_path})
    except (urllib.error.URLError, OSError) as e:
        return False, {"error": str(e)}
    if status != 200:
        return False, body or {"error": f"HTTP {status}"}
    return True, body or {}


def append_lineage(action: str, frm: str | None, to: str | None, ok: bool,
                   detail: dict | None = None) -> None:
    """Record promote/rollback in the lineage manifest. Best-effort."""
    try:
        LINEAGE_PATH.parent.mkdir(parents=True, exist_ok=True)
        rec = {
            "timestamp": int(time.time() * 1000),
            "kind": "promote" if action == "promote" else "rollback",
            "action": action,
            "from_adapter": frm,
            "to_adapter": to,
            "ok": ok,
            "detail": detail or {},
        }
        with open(LINEAGE_PATH, "a") as f:
            f.write(json.dumps(rec, default=str) + "\n")
    except OSError as e:
        print(f"  WARN: could not append lineage: {e}")


def last_promotion_from_lineage() -> dict | None:
    """Walk the lineage manifest backward to find the most-recent
    successful 'promote' record (for rollback target resolution)."""
    if not LINEAGE_PATH.exists():
        return None
    last = None
    for line in LINEAGE_PATH.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            rec = json.loads(line)
        except json.JSONDecodeError:
            continue
        if rec.get("action") == "promote" and rec.get("ok"):
            last = rec
    return last


def is_production_adapter(path: str) -> bool:
    """Heuristic guard: any adapter path matching the production
    regex requires --yes. Operator can override with --no-prod-check."""
    return bool(PROD_REGEX.search(path))


def cmd_current(args):
    mlx_url = args.mlx_url
    current = get_current_adapter(mlx_url)
    if current is None:
        print(f"  ({mlx_url}) no adapter currently loaded OR server unreachable")
        return 0
    print(f"  ({mlx_url}) current adapter: {current}")
    return 0


def _read_adapter_scale(adapter_path):
    """Return the LoRA scale from <adapter>/adapter_config.json. A config that
    exists but omits `scale` is treated as mlx_lm_lora's UNSAFE 10.0 default.
    Returns None only when no config can be read (cannot assess)."""
    try:
        cfg_path = Path(adapter_path) / "adapter_config.json"
        if not cfg_path.exists():
            return None
        import json as _json
        cfg = _json.loads(cfg_path.read_text())
        lp = cfg.get("lora_parameters", cfg)
        s = lp.get("scale")
        return 10.0 if s is None else float(s)
    except (OSError, ValueError, TypeError):
        return None


def cmd_promote(args):
    if not args.adapter:
        print("ERROR: --adapter required", file=sys.stderr)
        return 2
    if not Path(args.adapter).exists() and not args.allow_missing:
        print(f"ERROR: adapter path {args.adapter} not found "
              f"(use --allow-missing to bypass)", file=sys.stderr)
        return 2
    if is_production_adapter(args.adapter) and not args.yes and not args.no_prod_check:
        print(f"ERROR: adapter path looks production-flavored ({args.adapter}).\n"
              f"Pass --yes to confirm, or --no-prod-check to disable the heuristic.",
              file=sys.stderr)
        return 3

    # lora-scale-default-or-die.md: NEVER promote an adapter whose LoRA scale
    # exceeds the 8.0 ceiling. scale>8 over-amplifies the persona delta and
    # collapses the base model's instruction-following (the 2026-05-25 scale=20
    # incident that produced ~2 weeks of empty replies). A missing scale is
    # treated as mlx_lm_lora's unsafe 10.0 default. This is the last gate before
    # a bad adapter reaches the live server — retrain at scale=2.0 instead.
    scale = _read_adapter_scale(args.adapter)
    if scale is not None and scale > 8.0:
        print(f"ERROR: refusing to promote {args.adapter}: LoRA scale {scale} "
              f"exceeds the 8.0 safety ceiling (lora-scale-default-or-die.md). "
              f"scale>8 collapses base instruction-following. Retrain at scale=2.0.",
              file=sys.stderr)
        return 4

    mlx_url = args.mlx_url
    current = get_current_adapter(mlx_url)
    print(f"  Current adapter: {current or '(none)'}")
    print(f"  Target adapter:  {args.adapter}")

    if args.dry_run:
        print(f"  (--dry-run) would POST swap_adapter('{args.adapter}') to {mlx_url}")
        return 0

    ok, body = swap_adapter(mlx_url, args.adapter)
    append_lineage("promote", current, args.adapter, ok, body)
    if ok:
        print(f"  Promoted: {current or '(none)'} → {args.adapter}")
        return 0
    print(f"  PROMOTE FAILED: {body}", file=sys.stderr)
    return 2


def cmd_rollback(args):
    mlx_url = args.mlx_url
    last = last_promotion_from_lineage()
    if not last:
        print(f"ERROR: no successful promotion in lineage to roll back from",
              file=sys.stderr)
        return 2
    target = last.get("from_adapter")
    if not target:
        print(f"ERROR: prior promotion had no recorded from_adapter "
              f"(cold-start promote — nothing to roll back to)", file=sys.stderr)
        return 2
    current = get_current_adapter(mlx_url)
    print(f"  Current adapter: {current or '(none)'}")
    print(f"  Rollback target: {target}")

    if args.dry_run:
        print(f"  (--dry-run) would POST swap_adapter('{target}') to {mlx_url}")
        return 0
    if not args.yes:
        print(f"ERROR: rollback requires --yes (this changes a live MLX server)",
              file=sys.stderr)
        return 3

    ok, body = swap_adapter(mlx_url, target)
    append_lineage("rollback", current, target, ok, body)
    if ok:
        print(f"  Rolled back: {current or '(none)'} → {target}")
        return 0
    print(f"  ROLLBACK FAILED: {body}", file=sys.stderr)
    return 2


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--mlx-url", default=DEFAULT_MLX_URL)
    ap.add_argument("--dry-run", action="store_true",
                    help="Print the swap call but don't execute")
    sub = ap.add_subparsers(dest="cmd", required=True)

    sub.add_parser("current", help="Show the currently-loaded adapter")

    p_promote = sub.add_parser("promote", help="Swap to a new adapter and record")
    p_promote.add_argument("--adapter", required=True,
                            help="Absolute path to the LoRA adapter directory")
    p_promote.add_argument("--yes", action="store_true",
                            help="Confirm production-path swap (skip safety prompt)")
    p_promote.add_argument("--no-prod-check", action="store_true",
                            help="Disable the production-path heuristic")
    p_promote.add_argument("--allow-missing", action="store_true",
                            help="Allow promoting an adapter path that doesn't exist locally "
                                 "(MLX server may resolve it on its own machine)")

    p_rollback = sub.add_parser("rollback",
                                  help="Revert to the previous adapter from lineage")
    p_rollback.add_argument("--yes", action="store_true",
                              help="Confirm — required for non-dry-run rollback")

    args = ap.parse_args()
    if args.cmd == "current":
        return cmd_current(args)
    if args.cmd == "promote":
        return cmd_promote(args)
    if args.cmd == "rollback":
        return cmd_rollback(args)
    return 1


if __name__ == "__main__":
    sys.exit(main())
