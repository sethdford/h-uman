#!/usr/bin/env python3
"""
Phase E5 (2026-05-18) — per-contact adapter routing.

Storage + CLI for: "when contact X messages the agent, use adapter Y."

Closes part of the per-relationship gap from the audit. Real
per-contact personalization is a SOTA pattern (Character.AI per-
character LoRAs, ChatGPT per-user fine-tunes). This script ships
the storage and operator surface; the C-side hot-swap-per-turn
wire is a follow-up slice.

Persistence:
    ~/.human/training-data/m3_contact_routes.json
    {
        "routes": {
            "<contact_id_hash>": {
                "contact_label": "alice",   # human-readable, optional
                "adapter_path": "/path/to/alice-lora.bin",
                "promoted_at_ms": 1779142521147,
                "outcome_count_at_promote": 87
            },
            ...
        },
        "default_adapter": "/path/to/default-lora.bin"  # optional
    }

The C side, on each turn:
  1. Compute contact_id_hash via hu_m3_outcome_hash_bytes(contact_id)
  2. Look up in the JSON (read at boot, cached)
  3. If found → swap to that adapter before inference
  4. If not found → use default_adapter, or no adapter

That C-side wire isn't in this slice — what we ship today:
  - Storage shape (JSON file)
  - CLI: list / promote / demote / lookup operations
  - Python lookup function (importable; future C bridge will mirror)
  - Atomic save (same pattern as personal_model.c)

Usage:
    python3 scripts/m3_contact_routing.py list
    python3 scripts/m3_contact_routing.py promote \\
        --contact-hash 9267442741025671579 \\
        --adapter-path ~/.human/training-data/adapters/mom-lora.bin \\
        --label "Mom"
    python3 scripts/m3_contact_routing.py lookup --contact-hash 9267442741025671579
    python3 scripts/m3_contact_routing.py demote --contact-hash 9267442741025671579
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any

ROUTES_PATH = Path.home() / ".human" / "training-data" / "m3_contact_routes.json"


def load_routes(path: Path = ROUTES_PATH) -> dict:
    """Read routes file. Returns empty struct if missing or malformed
    — never raises. The C side should mirror this resilience: a
    corrupt routes file should NOT block inference; just degrade to
    no per-contact routing."""
    if not path.exists():
        return {"routes": {}, "default_adapter": None}
    try:
        data = json.loads(path.read_text())
    except (json.JSONDecodeError, OSError):
        return {"routes": {}, "default_adapter": None}
    if not isinstance(data, dict):
        return {"routes": {}, "default_adapter": None}
    data.setdefault("routes", {})
    data.setdefault("default_adapter", None)
    return data


def save_routes_atomic(routes: dict, path: Path = ROUTES_PATH) -> None:
    """tmp + fsync + rename, same crash-safety pattern as
    personal_model.c::hu_personal_model_save."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".json.tmp")
    with open(tmp, "w") as f:
        json.dump(routes, f, indent=2)
        f.flush()
        import os
        os.fsync(f.fileno())
    tmp.replace(path)


def lookup_contact_adapter(contact_hash: int, routes: dict | None = None) -> str | None:
    """Return the adapter path for this contact, or the default, or
    None. This is the PUBLIC LOOKUP API — the C bridge will mirror
    this exact semantic.

    Lookup precedence:
      1. Specific contact_id_hash route
      2. default_adapter (set at top level)
      3. None (caller uses base model)
    """
    if routes is None:
        routes = load_routes()
    contact_route = routes.get("routes", {}).get(str(contact_hash))
    if contact_route and contact_route.get("adapter_path"):
        return contact_route["adapter_path"]
    return routes.get("default_adapter")


def promote(contact_hash: int, adapter_path: str, label: str | None,
            outcome_count: int) -> dict:
    """Add or update a route. Returns the updated route record."""
    routes = load_routes()
    record = {
        "contact_label": label or "",
        "adapter_path": adapter_path,
        "promoted_at_ms": int(time.time() * 1000),
        "outcome_count_at_promote": outcome_count,
    }
    routes["routes"][str(contact_hash)] = record
    save_routes_atomic(routes)
    return record


def demote(contact_hash: int) -> bool:
    """Remove a route. Returns True if a route existed and was removed."""
    routes = load_routes()
    if str(contact_hash) in routes.get("routes", {}):
        del routes["routes"][str(contact_hash)]
        save_routes_atomic(routes)
        return True
    return False


def cmd_list(args):
    routes = load_routes()
    n = len(routes.get("routes", {}))
    print(f"\n  Routes file: {ROUTES_PATH}")
    print(f"  Routes:      {n}")
    print(f"  Default:     {routes.get('default_adapter') or '(none)'}")
    if n == 0:
        print(f"\n  No per-contact routes configured.")
        return 0
    print(f"\n  {'Contact hash':>22}  Label             Adapter")
    print(f"  {'-' * 22}  ----------------  --------------------------")
    for ch, rec in sorted(routes["routes"].items()):
        label = (rec.get("contact_label") or "")[:16]
        ap = rec.get("adapter_path") or ""
        print(f"  {ch:>22}  {label:<16}  {ap}")
    return 0


def cmd_promote(args):
    if not args.contact_hash:
        print("ERROR: --contact-hash required", file=sys.stderr)
        return 2
    if not args.adapter_path:
        print("ERROR: --adapter-path required", file=sys.stderr)
        return 2
    if not Path(args.adapter_path).exists():
        print(f"WARN: adapter {args.adapter_path} does not exist (promoting anyway)")
    rec = promote(args.contact_hash, args.adapter_path, args.label,
                  args.outcome_count or 0)
    print(f"  Promoted: contact_hash={args.contact_hash} → {args.adapter_path}")
    print(f"  Record: {json.dumps(rec, indent=2)}")
    return 0


def cmd_demote(args):
    if not args.contact_hash:
        print("ERROR: --contact-hash required", file=sys.stderr)
        return 2
    if demote(args.contact_hash):
        print(f"  Demoted: contact_hash={args.contact_hash}")
        return 0
    print(f"  No route found for contact_hash={args.contact_hash}")
    return 0


def cmd_lookup(args):
    if not args.contact_hash:
        print("ERROR: --contact-hash required", file=sys.stderr)
        return 2
    adapter = lookup_contact_adapter(args.contact_hash)
    if adapter:
        print(f"  contact_hash={args.contact_hash} → {adapter}")
    else:
        print(f"  contact_hash={args.contact_hash} → (no route; use base model)")
    return 0


def cmd_set_default(args):
    if not args.adapter_path:
        print("ERROR: --adapter-path required", file=sys.stderr)
        return 2
    routes = load_routes()
    routes["default_adapter"] = args.adapter_path
    save_routes_atomic(routes)
    print(f"  Default adapter set: {args.adapter_path}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    sub.add_parser("list", help="List all configured routes")

    p_promote = sub.add_parser("promote", help="Add or update a per-contact route")
    p_promote.add_argument("--contact-hash", type=int, required=True)
    p_promote.add_argument("--adapter-path", required=True)
    p_promote.add_argument("--label", help="Optional human-readable name")
    p_promote.add_argument("--outcome-count", type=int, default=0,
                            help="Outcomes accumulated at time of promotion")

    p_demote = sub.add_parser("demote", help="Remove a per-contact route")
    p_demote.add_argument("--contact-hash", type=int, required=True)

    p_lookup = sub.add_parser("lookup", help="Resolve a contact to an adapter")
    p_lookup.add_argument("--contact-hash", type=int, required=True)

    p_default = sub.add_parser("set-default",
                                help="Set the default adapter (used when contact has no route)")
    p_default.add_argument("--adapter-path", required=True)

    args = ap.parse_args()

    if args.cmd == "list":
        return cmd_list(args)
    if args.cmd == "promote":
        return cmd_promote(args)
    if args.cmd == "demote":
        return cmd_demote(args)
    if args.cmd == "lookup":
        return cmd_lookup(args)
    if args.cmd == "set-default":
        return cmd_set_default(args)
    return 1


if __name__ == "__main__":
    sys.exit(main())
