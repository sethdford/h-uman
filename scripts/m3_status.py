#!/usr/bin/env python3
"""
M3 Loop Status — operator surface for the personalization loop.

Inspects on-disk state and prints a single-screen snapshot of:
  - Outcomes ring health (driver state file)
  - Outcomes JSONL accumulator
  - id-map (which model/adapter ids have been observed)
  - Adapter directory (lineage of produced LoRAs)
  - Last training run summary (if a manifest exists)

No subprocess calls, no network — pure file-based. Safe to run while
the daemon is live; reads files atomically.

Usage:
    python3 scripts/m3_status.py              # default — human-readable
    python3 scripts/m3_status.py --json       # JSON output for piping
    make m3-status

Exit codes:
    0 — status printed (regardless of loop health)
    2 — fatal I/O error reading state files
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path
from typing import Any

HUMAN_HOME = Path.home() / ".human"
STATE_PATH = HUMAN_HOME / "m3_driver_state.json"
OUTCOMES_JSONL = HUMAN_HOME / "training-data" / "m3-outcomes.jsonl"
ID_MAP_PATH = HUMAN_HOME / "training-data" / "m3_id_map.json"
ADAPTER_DIR = HUMAN_HOME / "training-data" / "adapters"
LINEAGE_PATH = HUMAN_HOME / "training-data" / "adapter_lineage.jsonl"


def _read_json(p: Path) -> dict | None:
    if not p.exists():
        return None
    try:
        return json.loads(p.read_text())
    except (json.JSONDecodeError, OSError):
        return None


def _count_jsonl_lines(p: Path) -> int:
    if not p.exists():
        return 0
    count = 0
    with open(p) as f:
        for line in f:
            if line.strip():
                count += 1
    return count


def collect_driver_state() -> dict:
    state = _read_json(STATE_PATH)
    if state is None:
        return {"present": False}
    return {
        "present": True,
        "path": str(STATE_PATH),
        "last_ts_ms": state.get("last_ts_ms", 0),
        "last_iso": (time.strftime("%Y-%m-%d %H:%M:%S",
                                    time.localtime(state["last_ts_ms"] / 1000))
                     if state.get("last_ts_ms") else "never"),
        "seen_hashes": len(state.get("seen_prompt_hashes", [])),
    }


def collect_jsonl_state() -> dict:
    if not OUTCOMES_JSONL.exists():
        return {"present": False}
    samples = _count_jsonl_lines(OUTCOMES_JSONL)
    size = OUTCOMES_JSONL.stat().st_size
    # Quick stats on the most-recent 100 lines (full scan would be O(N)
    # on a growing file — keep this O(1)).
    recent_kinds: dict[int, int] = {}
    recent_guards: dict[int, int] = {}
    most_recent_ts = 0
    with open(OUTCOMES_JSONL) as f:
        # Tail the last 100 lines by seeking from end. Simpler: read
        # whole file but cap (this file rarely exceeds 10 MB in practice).
        lines = f.readlines()[-100:]
    for line in lines:
        try:
            o = json.loads(line)
        except json.JSONDecodeError:
            continue
        recent_kinds[o.get("k", 0)] = recent_kinds.get(o.get("k", 0), 0) + 1
        recent_guards[o.get("g", 0)] = recent_guards.get(o.get("g", 0), 0) + 1
        most_recent_ts = max(most_recent_ts, o.get("t", 0))
    return {
        "present": True,
        "path": str(OUTCOMES_JSONL),
        "total_samples": samples,
        "size_bytes": size,
        "recent_kinds": recent_kinds,
        "recent_guards": recent_guards,
        "most_recent_ts": most_recent_ts,
    }


def collect_id_map() -> dict:
    m = _read_json(ID_MAP_PATH)
    if m is None:
        return {"present": False}
    return {
        "present": True,
        "path": str(ID_MAP_PATH),
        "models": m.get("models", {}),
        "adapters": m.get("adapters", {}),
        "model_count": len(m.get("models", {})),
        "adapter_count": len(m.get("adapters", {})),
    }


def collect_adapter_dir() -> dict:
    if not ADAPTER_DIR.exists():
        return {"present": False}
    files = []
    for p in ADAPTER_DIR.iterdir():
        if p.is_file() and (p.name.startswith("m3-driver-") or "candidate" in p.name
                             or "baseline" in p.name):
            files.append({
                "name": p.name,
                "size_bytes": p.stat().st_size,
                "mtime": int(p.stat().st_mtime),
                "format": ("lora-bin" if _peek_format(p) == "LORA" else "safetensors"),
            })
    files.sort(key=lambda f: f["mtime"], reverse=True)
    total_size = sum(f["size_bytes"] for f in files)
    return {
        "present": True,
        "dir": str(ADAPTER_DIR),
        "adapter_count": len(files),
        "total_size_bytes": total_size,
        "most_recent": files[:5],  # top 5 most-recent
    }


def _peek_format(p: Path) -> str:
    try:
        with open(p, "rb") as f:
            return f.read(4).decode("ascii", "replace")
    except OSError:
        return ""


def collect_lineage() -> dict:
    """Adapter lineage manifest — JSONL with one record per produced
    adapter (D2 lands this). Present here as a 'soft' lookup so the
    status command works whether or not D2 has populated it yet."""
    if not LINEAGE_PATH.exists():
        return {"present": False, "entries": 0}
    entries = _count_jsonl_lines(LINEAGE_PATH)
    last = None
    if entries > 0:
        with open(LINEAGE_PATH) as f:
            for line in f:
                if line.strip():
                    last = line
    last_obj = json.loads(last) if last else None
    return {
        "present": True,
        "path": str(LINEAGE_PATH),
        "entries": entries,
        "most_recent": last_obj,
    }


def format_human(snapshot: dict) -> str:
    """Single-screen text summary. Designed to be readable, not
    parseable — for parseable output use --json."""
    lines = []
    lines.append("═" * 60)
    lines.append("  M3 PERSONALIZATION LOOP — STATUS")
    lines.append("═" * 60)

    # Driver state
    d = snapshot["driver"]
    lines.append("\n  Driver state:")
    if d["present"]:
        lines.append(f"    Watermark:  {d['last_iso']} (ts={d['last_ts_ms']})")
        lines.append(f"    Seen hashes: {d['seen_hashes']}")
    else:
        lines.append(f"    NOT INITIALIZED — driver has never run")

    # Outcomes JSONL
    j = snapshot["jsonl"]
    lines.append("\n  Outcomes JSONL:")
    if j["present"]:
        lines.append(f"    Samples:    {j['total_samples']} ({j['size_bytes']} bytes)")
        kind_names = {1: "stream", 2: "batch", 3: "proactive"}
        guard_names = {1: "PASS", 2: "REWRITE", 3: "REJECT", 0: "unknown"}
        if j["recent_kinds"]:
            kk = ", ".join(f"{kind_names.get(k, '?')}={v}"
                          for k, v in sorted(j["recent_kinds"].items()))
            lines.append(f"    Recent kinds: {kk}")
        if j["recent_guards"]:
            gg = ", ".join(f"{guard_names.get(g, '?')}={v}"
                          for g, v in sorted(j["recent_guards"].items()))
            lines.append(f"    Recent guards: {gg}")
    else:
        lines.append(f"    EMPTY — no outcomes have flowed through driver")

    # id-map
    m = snapshot["id_map"]
    lines.append("\n  id-map (outcome clustering):")
    if m["present"]:
        lines.append(f"    Models:   {m['model_count']}")
        for name, mid in sorted(m["models"].items(), key=lambda kv: kv[1])[:5]:
            lines.append(f"      {mid:3d}  {name}")
        lines.append(f"    Adapters: {m['adapter_count']}")
        for name, aid in sorted(m["adapters"].items(), key=lambda kv: kv[1])[:5]:
            lines.append(f"      {aid:3d}  {name}")
    else:
        lines.append(f"    NOT INITIALIZED — daemon hasn't attached an M3 adapter yet")

    # Adapter directory
    a = snapshot["adapters"]
    lines.append("\n  Adapter directory:")
    if a["present"]:
        lines.append(f"    Total: {a['adapter_count']} files, "
                     f"{a['total_size_bytes']:,} bytes")
        lines.append(f"    Most recent (top 5):")
        for f in a["most_recent"]:
            ts = time.strftime("%m-%d %H:%M", time.localtime(f["mtime"]))
            lines.append(f"      {ts}  {f['format']:13s}  "
                         f"{f['size_bytes']:>8,} B  {f['name']}")
    else:
        lines.append(f"    EMPTY — no adapters produced yet")

    # Lineage (D2)
    l = snapshot["lineage"]
    lines.append("\n  Lineage manifest:")
    if l["present"]:
        lines.append(f"    Entries: {l['entries']}")
        if l["most_recent"]:
            mr = l["most_recent"]
            lines.append(f"    Most recent: {mr.get('timestamp', '?')} "
                         f"verdict={mr.get('verdict', '?')} "
                         f"size={mr.get('size_bytes', '?')}")
    else:
        lines.append(f"    NOT POPULATED — lineage tracking lands in D2")

    lines.append("\n" + "═" * 60)
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--json", action="store_true",
                    help="Output JSON instead of human-readable summary")
    args = ap.parse_args()

    snapshot = {
        "driver": collect_driver_state(),
        "jsonl": collect_jsonl_state(),
        "id_map": collect_id_map(),
        "adapters": collect_adapter_dir(),
        "lineage": collect_lineage(),
        "collected_at": int(time.time() * 1000),
    }

    if args.json:
        print(json.dumps(snapshot, indent=2, default=str))
    else:
        print(format_human(snapshot))

    return 0


if __name__ == "__main__":
    sys.exit(main())
