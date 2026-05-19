#!/usr/bin/env python3
"""
M3 status dashboard (2026-05-19) — one-screen view of the autonomous
loop. Distinct from m3_status.py (older, file-existence summary):
this script reads ACTUAL state from the artifacts produced by every
stage and reports what's working, what's stale, and what's pending.

Five sections:
  H-tier (data acquisition) — corpus, counterfactuals, probe queue
  Training                  — adapter inventory + most-recent training metrics
  Eval                      — most-recent metadata + behavioral verdicts
  Promotion                 — adapter lineage + active production adapter
  Loop                      — last m3_loop_cycle.sh run; cron schedule

Read-only; no side effects.

Usage:
    python3 scripts/m3_status_dashboard.py
    python3 scripts/m3_status_dashboard.py --json    # machine-readable
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import re
import subprocess
import sys
from pathlib import Path

HUMAN_HOME = Path(os.environ.get("HUMAN_HOME") or (Path.home() / ".human"))
TRAINING = HUMAN_HOME / "training-data"
LOGS = HUMAN_HOME / "logs"


# ─────────────────────────────────────────────────────────────────────
# Small helpers
# ─────────────────────────────────────────────────────────────────────

def file_age(path: Path) -> str:
    if not path.exists():
        return "n/a"
    age = datetime.datetime.now() - datetime.datetime.fromtimestamp(
        path.stat().st_mtime)
    if age.days:
        return f"{age.days}d ago"
    h = age.seconds // 3600
    if h:
        return f"{h}h ago"
    m = age.seconds // 60
    if m:
        return f"{m}m ago"
    return f"{age.seconds}s ago"


def line_count(path: Path) -> int:
    if not path.exists():
        return 0
    return sum(1 for _ in open(path))


def size_human(n: int) -> str:
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:.1f}{unit}"
        n /= 1024
    return f"{n:.1f}TB"


def section(title: str) -> None:
    print(f"\n{title}")
    print("─" * len(title))


# ─────────────────────────────────────────────────────────────────────
# Section: H-tier (data acquisition)
# ─────────────────────────────────────────────────────────────────────

def h_tier_status() -> dict:
    corpus = TRAINING / "m3-corpus.jsonl"
    cf = TRAINING / "m3-counterfactuals.jsonl"
    holdout = TRAINING / "m3-holdout-prompts.jsonl"
    queue = TRAINING / "m3-active-probe-queue.jsonl"
    pairs = TRAINING / "m3-active-probe-pairs.jsonl"
    # Probe queue states
    q_states = {"pending": 0, "sent": 0, "done": 0, "other": 0}
    if queue.exists():
        for line in open(queue):
            try:
                r = json.loads(line)
                s = r.get("status", "other")
                if s not in q_states:
                    q_states["other"] += 1
                else:
                    q_states[s] += 1
            except (json.JSONDecodeError, KeyError):
                continue
    return {
        "corpus_records":         line_count(corpus),
        "corpus_age":             file_age(corpus),
        "counterfactual_pairs":   line_count(cf),
        "counterfactual_age":     file_age(cf),
        "holdout_prompts":        line_count(holdout),
        "holdout_age":            file_age(holdout),
        "probe_queue_pending":    q_states["pending"],
        "probe_queue_sent":       q_states["sent"],
        "probe_queue_done":       q_states["done"],
        "probe_pairs":            line_count(pairs),
    }


# ─────────────────────────────────────────────────────────────────────
# Section: training inventory
# ─────────────────────────────────────────────────────────────────────

def training_status() -> dict:
    adapters_dir = TRAINING / "adapters"
    if not adapters_dir.exists():
        return {"adapters": []}
    adapters = []
    for d in sorted(adapters_dir.iterdir(),
                     key=lambda p: p.stat().st_mtime if p.exists() else 0,
                     reverse=True)[:8]:
        if d.is_dir():
            sf = d / "adapters.safetensors"
            sz = sf.stat().st_size if sf.exists() else 0
            adapters.append({
                "name":     d.name,
                "type":     "safetensors-dir",
                "size":     size_human(sz),
                "age":      file_age(sf if sf.exists() else d),
            })
        elif d.suffix in (".safetensors", ".bin"):
            adapters.append({
                "name":     d.name,
                "type":     d.suffix[1:],
                "size":     size_human(d.stat().st_size),
                "age":      file_age(d),
            })
    return {"adapters": adapters}


# ─────────────────────────────────────────────────────────────────────
# Section: most-recent eval verdicts
# ─────────────────────────────────────────────────────────────────────

def eval_status() -> dict:
    if not LOGS.exists():
        return {}
    # Most-recent metadata + behavioral verdict files
    meta_files = sorted(LOGS.glob("m3-verdict-*.json"),
                         key=lambda p: p.stat().st_mtime,
                         reverse=True)
    beh_files = sorted(LOGS.glob("m3-behavioral-*.json"),
                        key=lambda p: p.stat().st_mtime,
                        reverse=True)
    out = {}
    if meta_files:
        try:
            v = json.load(open(meta_files[0]))
            out["metadata"] = {
                "verdict": v.get("verdict"),
                "reason":  v.get("reason", "")[:80],
                "age":     file_age(meta_files[0]),
            }
        except (json.JSONDecodeError, OSError):
            pass
    if beh_files:
        try:
            v = json.load(open(beh_files[0]))
            out["behavioral"] = {
                "verdict": v.get("verdict"),
                "reason":  v.get("reason", "")[:80],
                "wins":    f"{v.get('wins','?')}/5",
                "diversity_collapsed": v.get("diversity", {}).get("is_collapsed"),
                "age":     file_age(beh_files[0]),
            }
        except (json.JSONDecodeError, OSError):
            pass
    return out


# ─────────────────────────────────────────────────────────────────────
# Section: promotion + lineage
# ─────────────────────────────────────────────────────────────────────

def promotion_status() -> dict:
    lineage = TRAINING / "adapter_lineage.jsonl"
    out = {"events_total": line_count(lineage),
           "last_promote": None}
    if lineage.exists():
        last_promote = None
        for line in open(lineage):
            try:
                r = json.loads(line)
                if r.get("action") == "promote" and r.get("ok"):
                    last_promote = r
            except (json.JSONDecodeError, KeyError):
                continue
        if last_promote:
            out["last_promote"] = {
                "to_adapter": last_promote.get("to_adapter", ""),
                "ts_ms":      last_promote.get("ts_ms"),
            }
    return out


# ─────────────────────────────────────────────────────────────────────
# Section: launchd / cron schedule
# ─────────────────────────────────────────────────────────────────────

def schedule_status() -> dict:
    out = {"plist_installed": False, "last_log_age": "n/a"}
    label = "ai.human.m3-loop"
    try:
        result = subprocess.run(["launchctl", "list", label],
                                 capture_output=True, text=True, timeout=5)
        if result.returncode == 0:
            out["plist_installed"] = True
            # Extract last exit status if present
            for line in result.stdout.split("\n"):
                if line.startswith("\t\"LastExitStatus\""):
                    m = re.search(r"=\s*(\d+)", line)
                    if m:
                        out["last_exit_status"] = int(m.group(1))
    except (subprocess.TimeoutExpired, FileNotFoundError):
        pass
    # Most-recent cycle log
    if LOGS.exists():
        cycle_logs = sorted(LOGS.glob("m3-loop-*.log"),
                            key=lambda p: p.stat().st_mtime,
                            reverse=True)
        if cycle_logs:
            out["last_log_age"] = file_age(cycle_logs[0])
            out["last_log_path"] = str(cycle_logs[0])
            # Check completion marker in the log
            tail = list(open(cycle_logs[0]))[-5:]
            out["last_log_completed"] = any(
                "m3-loop-cycle complete" in l for l in tail)
    return out


# ─────────────────────────────────────────────────────────────────────
# Renderer
# ─────────────────────────────────────────────────────────────────────

def render_human(h: dict, t: dict, e: dict, p: dict, s: dict) -> None:
    section("H-tier (data acquisition)")
    print(f"  Corpus:                {h['corpus_records']:>5}  ({h['corpus_age']})")
    print(f"  Counterfactual pairs:  {h['counterfactual_pairs']:>5}  ({h['counterfactual_age']})")
    print(f"  Held-out prompts:      {h['holdout_prompts']:>5}  ({h['holdout_age']})")
    print(f"  Probe queue:           pending={h['probe_queue_pending']}  "
          f"sent={h['probe_queue_sent']}  done={h['probe_queue_done']}")
    print(f"  Probe gold-label pairs:{h['probe_pairs']:>5}")

    section("Training inventory (most recent 8)")
    for a in t["adapters"][:8]:
        print(f"  {a['name']:<45} {a['size']:>10}  {a['age']}")
    if not t["adapters"]:
        print(f"  (none)")

    section("Most-recent eval verdicts")
    if "metadata" in e:
        m = e["metadata"]
        print(f"  Metadata:    {m['verdict'].upper():<10}  {m['reason']}  ({m['age']})")
    if "behavioral" in e:
        b = e["behavioral"]
        collapsed = " — MODE COLLAPSE" if b.get("diversity_collapsed") else ""
        print(f"  Behavioral:  {b['verdict'].upper():<10}  wins={b['wins']}{collapsed}  ({b['age']})")
    if "metadata" not in e and "behavioral" not in e:
        print(f"  (no eval verdicts yet)")

    section("Promotion lineage")
    print(f"  Lineage events: {p['events_total']}")
    if p.get("last_promote"):
        lp = p["last_promote"]
        print(f"  Last promote: {Path(lp['to_adapter']).name}")
    else:
        print(f"  No prior promotion")

    section("Schedule (launchd ai.human.m3-loop)")
    print(f"  Plist installed:  {s['plist_installed']}")
    if s.get("last_exit_status") is not None:
        ok = "✓" if s["last_exit_status"] == 0 else "✗"
        print(f"  Last exit status: {s['last_exit_status']} {ok}")
    print(f"  Last cycle log:   {s['last_log_age']}")
    if s.get("last_log_completed"):
        print(f"    completion marker present: ✓")
    print()


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--json", action="store_true",
                    help="Emit machine-readable JSON instead of human view")
    args = ap.parse_args()

    h = h_tier_status()
    t = training_status()
    e = eval_status()
    p = promotion_status()
    s = schedule_status()

    if args.json:
        print(json.dumps({"h_tier": h, "training": t, "eval": e,
                          "promotion": p, "schedule": s}, indent=2))
    else:
        print(f"\n  M3 STATUS DASHBOARD — {datetime.datetime.now().isoformat(' ', 'seconds')}")
        render_human(h, t, e, p, s)
    return 0


if __name__ == "__main__":
    sys.exit(main())
