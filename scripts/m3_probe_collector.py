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

  dispatch — production sender (macOS).
    For each pending entry: send the question via Messages.app
    (osascript wire) to --operator-handle. Mark entry as "sent"
    with sent_ts_ms + operator_handle. Does NOT block waiting for
    the reply.
    Safety: --confirm-real-send is REQUIRED to actually invoke
    osascript. Without it, dry-run prints what would be sent.

  poll — production reply collector (macOS).
    For each "sent" entry: scan --chat-db (default ~/Library/
    Messages/chat.db) for messages on the operator's iMessage thread
    AFTER sent_ts_ms, skipping probe echoes (PROBE_HEADER prefix).
    First non-probe message becomes the reply; convert to pairs,
    append, mark "done".
    Requires Full Disk Access for the running shell on macOS.

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

    # Production dispatch (dry-run by default — see what would send)
    python3 scripts/m3_probe_collector.py --mode dispatch \\
        --operator-handle +15555550123

    # Production dispatch (LIVE — actually sends via Messages.app)
    python3 scripts/m3_probe_collector.py --mode dispatch \\
        --operator-handle +15555550123 --confirm-real-send

    # Production poll (reads chat.db for replies)
    python3 scripts/m3_probe_collector.py --mode poll \\
        --operator-handle +15555550123

Exit codes:
    0 — pair(s) written OR no pending entries (idempotent no-op)
    1 — partial failure (some sends failed)
    2 — queue file missing / malformed
    3 — production mode requested but required arg/file missing
"""
from __future__ import annotations

import argparse
import json
import os
import sqlite3
import subprocess
import sys
import time
from pathlib import Path

# Mac/Apple epoch offset used by chat.db (matches m3_extract_corpus.py)
APPLE_EPOCH_OFFSET_SEC = 978307200

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
# Dispatch wire — Messages.app via osascript (macOS)
# ─────────────────────────────────────────────────────────────────────

def send_via_messages_app(handle: str, body: str,
                           dry_run: bool = True) -> tuple[bool, str]:
    """Send `body` via Messages.app to iMessage handle `handle`.

    dry_run=True (default) prints what WOULD be sent and returns
    (True, "dry-run") without invoking osascript. Safety net for any
    accidental run; flipping to live requires --confirm-real-send.

    Returns (ok, detail). detail is "dry-run", "sent", or the error
    string when ok=False.
    """
    if not handle:
        return False, "no operator handle"
    if dry_run:
        print(f"  [DRY-RUN] osascript send → {handle} ({len(body)} chars)")
        return True, "dry-run"
    # Escape for AppleScript string literals. AppleScript strings use
    # backslash escapes for "  \  newline  tab — same convention as C.
    safe_handle = handle.replace("\\", "\\\\").replace('"', '\\"')
    safe_body = (body.replace("\\", "\\\\")
                      .replace('"', '\\"')
                      .replace("\n", "\\n")
                      .replace("\t", "\\t"))
    script = (
        'tell application "Messages"\n'
        '    set targetService to 1st service whose service type = iMessage\n'
        f'    set targetBuddy to buddy "{safe_handle}" of targetService\n'
        f'    send "{safe_body}" to targetBuddy\n'
        'end tell\n')
    try:
        result = subprocess.run(
            ["osascript", "-e", script],
            capture_output=True, text=True, timeout=20)
    except (subprocess.TimeoutExpired, FileNotFoundError) as e:
        return False, f"osascript invocation failed: {e}"
    if result.returncode != 0:
        return False, f"osascript exit {result.returncode}: {result.stderr.strip()}"
    return True, "sent"


def dispatch_mode(queue_path: Path, operator_handle: str,
                   confirm_real_send: bool) -> int:
    """Send every pending probe to `operator_handle` via Messages.app.
    Default dry_run=True is overridden only by confirm_real_send=True."""
    entries = load_queue(queue_path)
    pending = [(i, e) for i, e in enumerate(entries)
                if e.get("status") == "pending"]
    if not pending:
        print(f"  No pending entries in {queue_path}")
        return 0
    if not operator_handle:
        print(f"  --mode=dispatch requires --operator-handle (or M3_OPERATOR_HANDLE env)")
        print(f"  Pending entries (idx → handle hash → preview):")
        for i, e in pending:
            print(f"    [{i}] {e.get('handle','?'):>10} "
                  f"msg={e.get('user_message','')[:40]!r}")
        return 3
    sent = 0
    failed = 0
    dry_run = not confirm_real_send
    if dry_run:
        print(f"  DRY-RUN mode (pass --confirm-real-send to actually send)")
    for i, e in pending:
        ok, detail = send_via_messages_app(
            operator_handle, e.get("question", ""), dry_run=dry_run)
        if ok:
            e["status"] = "sent" if not dry_run else "pending"
            if not dry_run:
                e["sent_ts_ms"] = int(time.time() * 1000)
                e["operator_handle"] = operator_handle
            sent += 1
            print(f"    [{i}] OK ({detail})")
        else:
            failed += 1
            print(f"    [{i}] FAILED: {detail}", file=sys.stderr)
    if not dry_run:
        save_queue(queue_path, entries)
    tag = "(DRY-RUN — queue unchanged)" if dry_run else "(LIVE — queue updated)"
    print(f"  Dispatched {sent}/{len(pending)} {tag}; {failed} failed")
    return 0 if failed == 0 else 1


# ─────────────────────────────────────────────────────────────────────
# Poll wire — read chat.db for replies (macOS, needs FDA)
# ─────────────────────────────────────────────────────────────────────

def _decode_attributed_body(raw: bytes) -> str:
    """Local copy of the typedstream decoder. Kept in sync with
    m3_extract_corpus.py::decode_attributed_body — see that docstring
    for the format. Pinned by tests in both modules' verifiers."""
    if not raw or len(raw) < 30:
        return ""
    ns_idx = raw.find(b'NSString')
    if ns_idx < 0:
        return ""
    plus = raw.find(b'\x2b', ns_idx, ns_idx + 50)
    if plus < 0 or plus + 1 >= len(raw):
        return ""
    marker = raw[plus + 1]
    if marker == 0x81:
        if plus + 4 > len(raw):
            return ""
        body_len = int.from_bytes(raw[plus+2:plus+4], 'little')
        body_start = plus + 4
    elif marker == 0x84:
        if plus + 6 > len(raw):
            return ""
        body_len = int.from_bytes(raw[plus+2:plus+6], 'little')
        body_start = plus + 6
    else:
        body_len = marker
        body_start = plus + 2
    body_end = body_start + body_len
    if body_len <= 0 or body_len > 1_000_000 or body_end > len(raw):
        return ""
    return raw[body_start:body_end].decode('utf-8', 'replace')


def poll_chat_db_for_replies(chat_db: Path, operator_handle: str,
                              since_ms: int,
                              probe_header: str = PROBE_HEADER
                              ) -> list[tuple[int, str, bool]]:
    """Return messages on the operator's iMessage thread sent AFTER
    since_ms, EXCLUDING the probe echoes (those start with probe_header).
    Each tuple: (ts_ms, text, is_from_me).

    Reads BOTH `text` and `attributedBody` columns: modern macOS stores
    formatted / multi-line / emoji bodies in attributedBody with
    text=NULL. Without that fallback, long freetext replies would be
    invisible to the poll.

    Soft-fails: returns [] on any sqlite error (FDA missing, corrupt db,
    schema drift). The caller sees the empty list and skips that entry.
    """
    if not chat_db.exists() or not operator_handle:
        return []
    if since_ms > 0:
        since_apple_ns = int((since_ms / 1000.0 - APPLE_EPOCH_OFFSET_SEC)
                               * 1_000_000_000)
    else:
        since_apple_ns = 0
    out: list[tuple[int, str, bool]] = []
    try:
        conn = sqlite3.connect(str(chat_db))
        # Don't override text_factory here; we want str for `text` and
        # bytes for `attributedBody`. Use detect_types where needed.
        cur = conn.cursor()
        cur.execute(
            "SELECT m.text, m.date, m.is_from_me, m.attributedBody "
            "FROM message m LEFT JOIN handle h ON m.handle_id = h.ROWID "
            "WHERE h.id = ? AND m.date > ? "
            "  AND ((m.text IS NOT NULL AND length(m.text) > 0) "
            "       OR (m.attributedBody IS NOT NULL "
            "           AND length(m.attributedBody) > 0)) "
            "ORDER BY m.date ASC",
            (operator_handle, since_apple_ns))
        for text, apple_ns, ifm, ab in cur.fetchall():
            text = (text or "").strip()
            # Fall back to attributedBody when text is missing/empty
            if (not text or len(text) < 1) and ab:
                text = _decode_attributed_body(ab).strip()
            if not text:
                continue
            if text.startswith(probe_header):
                continue
            ts_ms = int((apple_ns / 1e9 + APPLE_EPOCH_OFFSET_SEC) * 1000)
            out.append((ts_ms, text, bool(ifm)))
        conn.close()
    except sqlite3.Error as e:
        print(f"  WARN: chat.db poll failed ({e}). FDA missing?",
              file=sys.stderr)
        return []
    return out


def poll_mode(queue_path: Path, pairs_out: Path,
               chat_db: Path | None, operator_handle: str) -> int:
    """For each 'sent' entry, find the first reply in chat.db on the
    operator's thread, convert to pairs, mark done."""
    entries = load_queue(queue_path)
    sent_entries = [(i, e) for i, e in enumerate(entries)
                     if e.get("status") == "sent"]
    if not sent_entries:
        print(f"  No 'sent' entries waiting for replies")
        return 0
    if not operator_handle or chat_db is None:
        print(f"  --mode=poll requires --operator-handle and --chat-db "
              f"(or M3_OPERATOR_HANDLE + default ~/Library/Messages/chat.db)")
        return 3
    if not chat_db.exists():
        print(f"  --mode=poll: {chat_db} does not exist (FDA missing on macOS?)")
        return 3
    total_pairs = 0
    matched = 0
    for i, e in sent_entries:
        replies = poll_chat_db_for_replies(
            chat_db, operator_handle, e.get("sent_ts_ms", 0))
        if not replies:
            continue
        # Take the FIRST reply (oldest after sent_ts_ms)
        ts_ms, text, _ = replies[0]
        pairs = response_to_pairs(e.get("user_message", ""),
                                    e.get("candidates", []), text)
        if not pairs:
            e["status"] = "no_pairs"
            e["operator_reply"] = text
            matched += 1
            continue
        append_pairs(pairs_out, pairs)
        e["status"] = "done"
        e["done_ts_ms"] = int(time.time() * 1000)
        e["operator_reply"] = text
        e["operator_reply_ts_ms"] = ts_ms
        e["pairs_written"] = len(pairs)
        total_pairs += len(pairs)
        matched += 1
    save_queue(queue_path, entries)
    print(f"  Polled {len(sent_entries)} sent entries → {matched} matched, "
          f"{total_pairs} pairs written")
    return 0


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
    ap.add_argument("--operator-handle",
                    default=os.environ.get("M3_OPERATOR_HANDLE", ""),
                    help="iMessage handle to send probes to (your own number/email). "
                         "Required for --mode=dispatch and --mode=poll. "
                         "Env: M3_OPERATOR_HANDLE")
    ap.add_argument("--confirm-real-send", action="store_true",
                    help="In --mode=dispatch, actually invoke osascript. "
                         "Without this flag, dispatch is a dry-run.")
    ap.add_argument("--chat-db", type=Path,
                    default=Path.home() / "Library" / "Messages" / "chat.db",
                    help="Path to chat.db for --mode=poll (default: macOS standard)")
    args = ap.parse_args()

    print(f"\n{'='*60}")
    print(f"  M3 PROBE COLLECTOR (H3b)")
    print(f"{'='*60}")
    print(f"  Queue:     {args.queue}")
    print(f"  Pairs out: {args.pairs_out}")
    print(f"  Mode:      {args.mode}")
    if args.mode in ("dispatch", "poll"):
        print(f"  Operator:  {args.operator_handle or '(not set)'}")

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
        return dispatch_mode(args.queue, args.operator_handle,
                              args.confirm_real_send)
    elif args.mode == "poll":
        return poll_mode(args.queue, args.pairs_out,
                          args.chat_db, args.operator_handle)
    return 0


if __name__ == "__main__":
    sys.exit(main())
