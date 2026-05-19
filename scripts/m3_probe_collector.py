#!/usr/bin/env python3
"""
Phase H3b (2026-05-19) — probe queue → preference pairs collector.

H3 (m3_active_probe.py) WRITES probe questions to a queue file. This
script READS them. It's the missing half of the loop — without it, the
queue is a science-project log and the active-learning signal never
reaches DPO training.

The collector handles three modes, picked by --mode:

  simulate-tick — single-pass test mode (default).
    Pick the oldest pending queue entry, mark it sent, optionally
    simulate Seth's reply via --simulate-response=<letter|text>,
    convert to Alpaca-DPO pairs, append to pairs-out, mark done.
    No iMessage send, no chat.db poll. Useful in tests and CI.

  dispatch — production sender.
    For each pending entry: send the question via `human channel
    send imessage <handle> <question>`. Mark entry as "sent" with
    the sent timestamp. Does NOT block waiting for the reply.
    (Currently a STUB — wires required to call the human binary.)

  poll — production reply collector.
    For each "sent" entry: scan ~/Library/Messages/chat.db for
    INBOUND messages from <handle> AFTER the sent timestamp.
    First such message is treated as Seth's reply. Convert to
    pairs, append, mark "done".
    (Currently a STUB — wires required to read chat.db with FDA.)

For local end-to-end testing, simulate-tick is enough: H3 writes a
queue entry, the collector picks it up, converts the simulated
response to pairs. That's the WHOLE flow.

Usage:
    # Test path — pick next pending entry, convert simulated reply
    python3 scripts/m3_probe_collector.py \\
        --queue ~/.human/training-data/m3-active-probe-queue.jsonl \\
        --pairs-out ~/.human/training-data/m3-active-probe-pairs.jsonl \\
        --mode simulate-tick \\
        --simulate-response=B

Exit codes:
    0 — pair(s) written OR no pending entries (idempotent no-op)
    2 — queue file missing / malformed
    3 — production mode requested but underlying wire missing
"""
from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path

# Re-use the parser from H3 so the response→pairs contract stays
# pinned in one place. If H3 changes, the collector follows.
import importlib.util
_SCRIPT_DIR = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location(
    "m3_active_probe", _SCRIPT_DIR / "m3_active_probe.py")
_probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(_probe)
response_to_pairs = _probe.response_to_pairs
PROBE_HEADER = _probe.PROBE_HEADER

DEFAULT_QUEUE = Path.home() / ".human" / "training-data" / "m3-active-probe-queue.jsonl"
DEFAULT_PAIRS_OUT = Path.home() / ".human" / "training-data" / "m3-active-probe-pairs.jsonl"


# ─────────────────────────────────────────────────────────────────────
# Queue file I/O — rewrite-on-update so status changes persist
# ─────────────────────────────────────────────────────────────────────

def load_queue(path: Path) -> list[dict]:
    """Read every JSONL entry. Malformed lines are skipped with a
    stderr warning (so one bad entry doesn't kill the whole run)."""
    if not path.exists():
        return []
    out = []
    for i, line in enumerate(path.read_text().splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        try:
            out.append(json.loads(line))
        except json.JSONDecodeError as e:
            print(f"  WARN: queue line {i} unparseable ({e}); skipping",
                  file=sys.stderr)
    return out


def save_queue(path: Path, entries: list[dict]) -> None:
    """Atomic rewrite. tmp + rename so an interrupted save can never
    leave the queue in a half-written state."""
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    with open(tmp, "w") as f:
        for e in entries:
            f.write(json.dumps(e, ensure_ascii=False) + "\n")
        f.flush()
    tmp.replace(path)


def find_next_pending(entries: list[dict]) -> int | None:
    """Return index of the oldest pending entry, or None."""
    pending = [(i, e) for i, e in enumerate(entries)
                if e.get("status") == "pending"]
    if not pending:
        return None
    # Sort by ts_ms so the oldest goes first
    pending.sort(key=lambda ie: ie[1].get("ts_ms", 0))
    return pending[0][0]


# ─────────────────────────────────────────────────────────────────────
# Pair emission — append-only, schema matches H2 and H3
# ─────────────────────────────────────────────────────────────────────

def append_pairs(path: Path, pairs: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "a") as f:
        for p in pairs:
            f.write(json.dumps(p, ensure_ascii=False) + "\n")


# ─────────────────────────────────────────────────────────────────────
# Simulate-tick — the test path that proves the loop end-to-end
# ─────────────────────────────────────────────────────────────────────

def simulate_tick(queue_path: Path, pairs_out: Path,
                   simulated_response: str | None) -> tuple[int, int]:
    """Single-pass: pick next pending → convert reply → mark done.
    Returns (n_pairs_written, n_pending_remaining)."""
    entries = load_queue(queue_path)
    idx = find_next_pending(entries)
    if idx is None:
        print(f"  No pending entries in {queue_path}")
        return 0, 0
    entry = entries[idx]
    user_msg = entry.get("user_message", "")
    cands = entry.get("candidates", [])
    if not cands:
        print(f"  WARN: queue entry {idx} has no candidates; skipping",
              file=sys.stderr)
        entry["status"] = "malformed"
        save_queue(queue_path, entries)
        return 0, len([e for e in entries if e.get("status") == "pending"])

    if not simulated_response:
        # No simulated response — just mark sent and exit
        entry["status"] = "sent"
        entry["sent_ts_ms"] = int(time.time() * 1000)
        save_queue(queue_path, entries)
        print(f"  Entry {idx} marked sent (no --simulate-response given)")
        return 0, len([e for e in entries if e.get("status") == "pending"])

    pairs = response_to_pairs(user_msg, cands, simulated_response)
    if not pairs:
        print(f"  WARN: simulated response {simulated_response!r} produced no pairs",
              file=sys.stderr)
        entry["status"] = "no_pairs"
        save_queue(queue_path, entries)
        return 0, len([e for e in entries if e.get("status") == "pending"])

    append_pairs(pairs_out, pairs)
    entry["status"] = "done"
    entry["done_ts_ms"] = int(time.time() * 1000)
    entry["pairs_written"] = len(pairs)
    save_queue(queue_path, entries)
    remaining = len([e for e in entries if e.get("status") == "pending"])
    print(f"  Wrote {len(pairs)} pair(s) → {pairs_out} (pending: {remaining})")
    return len(pairs), remaining


# ─────────────────────────────────────────────────────────────────────
# Dispatch / poll — production stubs documenting the wire surface
# ─────────────────────────────────────────────────────────────────────

def dispatch_mode(queue_path: Path) -> int:
    """Send every pending probe via the human binary's iMessage channel.
    STUB: the actual `human channel send` invocation is not yet wired —
    deliberately, because shipping accidental iMessages to real contacts
    while testing would be terrible. The contract is documented here so
    the wire can land as a separate, deliberate slice."""
    entries = load_queue(queue_path)
    pending = [(i, e) for i, e in enumerate(entries)
                if e.get("status") == "pending"]
    if not pending:
        print(f"  No pending entries in {queue_path}")
        return 0
    print(f"  --mode=dispatch: would send {len(pending)} probe(s) via iMessage")
    print(f"  STUB: this mode requires the human-binary iMessage wire.")
    print(f"        See deliver_probe('imessage') in m3_active_probe.py")
    print(f"        Pending entries (idx → handle):")
    for i, e in pending:
        print(f"          [{i}] handle={e.get('handle','?')} "
              f"msg={e.get('user_message','')[:40]!r}")
    return 3


def poll_mode(queue_path: Path, pairs_out: Path,
               chat_db: Path | None = None) -> int:
    """Scan chat.db for replies to sent probes. STUB: requires Full
    Disk Access on macOS and the same SQLite read pattern that H1
    uses. Contract documented; wire is a separate slice."""
    entries = load_queue(queue_path)
    sent = [(i, e) for i, e in enumerate(entries)
             if e.get("status") == "sent"]
    if not sent:
        print(f"  No 'sent' entries waiting for replies")
        return 0
    print(f"  --mode=poll: {len(sent)} entry/entries awaiting reply")
    print(f"  STUB: requires chat.db poll with FDA.")
    print(f"        See extract_imessage() in m3_extract_corpus.py for the read pattern.")
    return 3


# ─────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--queue", type=Path, default=DEFAULT_QUEUE)
    ap.add_argument("--pairs-out", type=Path, default=DEFAULT_PAIRS_OUT)
    ap.add_argument("--mode", choices=["simulate-tick", "dispatch", "poll"],
                    default="simulate-tick")
    ap.add_argument("--simulate-response",
                    help="In simulate-tick mode, the canned reply (letter or freetext)")
    ap.add_argument("--max-ticks", type=int, default=1,
                    help="In simulate-tick mode, process up to N entries")
    args = ap.parse_args()

    print(f"\n{'='*60}")
    print(f"  M3 PROBE COLLECTOR (H3b)")
    print(f"{'='*60}")
    print(f"  Queue:     {args.queue}")
    print(f"  Pairs out: {args.pairs_out}")
    print(f"  Mode:      {args.mode}")

    if args.mode == "simulate-tick":
        total_pairs = 0
        for _ in range(args.max_ticks):
            n_pairs, remaining = simulate_tick(args.queue, args.pairs_out,
                                                args.simulate_response)
            total_pairs += n_pairs
            if remaining == 0:
                break
        print(f"\n  TOTAL pairs written this run: {total_pairs}")
        return 0
    elif args.mode == "dispatch":
        return dispatch_mode(args.queue)
    elif args.mode == "poll":
        return poll_mode(args.queue, args.pairs_out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
