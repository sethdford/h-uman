#!/usr/bin/env python3
"""Export real inbound iMessage turns for the real-turn replay harness.

A turn is a run of a contact's messages in a 1:1 thread followed by Seth's
own response: the text bubbles he sent before their next message, or a
tapback he put on one of their messages. Each turn carries the thread before
it (both sides, labelled) so `human replay` can rebuild the state the daemon
had. Output is one JSON object per line:

  {"id", "contact_id", "ts", "inbound_bubbles": [...],
   "history": [{"from_me", "text", "ts"}, ...],
   "seth_action": "text"|"tapback", "seth_reply_bubbles": [...]}

Seth's own responses only: Seth's account also carries the daemon's replies,
so a turn is dropped when memory.db (read-only) shows the daemon produced the
response — outbound_sends or a session-store assistant row for that contact
within 10 minutes whose text contains or is contained in the bubble. A tapback
turn is dropped when the daemon produced anything for that contact within 10
minutes (chat.db cannot tell the director's tapbacks from Seth's). The number
dropped is printed; no text is.

Privacy, by construction:
  - chat.db is opened read-only (sqlite URI mode=ro); nothing is written to it;
  - output goes ONLY to a private run dir outside the repo
    (default ~/blind_ab_run/<name>/, dir 0700, file 0600);
  - nothing but counts is ever printed.

Usage:
  python3 replay_export_turns.py --name replay-2026-10-02 --limit 40
  python3 replay_export_turns.py --name x --db /path/to/chat.db --since-days 14

Runbook: docs/guides/replay-harness.md.
"""
import argparse
import datetime
import json
import os
import sqlite3
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from export_seth_triples import msg_text  # noqa: E402  (the shared decoder)

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
# A Seth-side message is the DAEMON's (not Seth's) when memory.db has a
# matching record for that contact within this window: outbound_sends (one
# row per delivered send, since 2026-09-25) or the session store's assistant
# rows (every reply the daemon produced, since 2026-03). A turn whose
# response the daemon sent is not a sample of Seth.
PROVENANCE_WINDOW_S = 10 * 60
UPTIME_STAMP_MAX_MS = 10 ** 12  # outbound_sends rows stamped with uptime: time unknown
APPLE_EPOCH = 978307200
TAPBACK_TYPES = range(2000, 2006)  # love, like, dislike, laugh, emphasize, question


def default_db():
    return os.environ.get("HU_CHATDB") or os.path.expanduser("~/Library/Messages/chat.db")


def apple_ts(raw):
    """chat.db `date`: nanoseconds since 2001 on modern macOS, seconds before."""
    if raw is None:
        return 0
    raw = int(raw)
    return (raw // 1_000_000_000 if raw > 10**12 else raw) + APPLE_EPOCH


def fmt_local(epoch):
    return datetime.datetime.fromtimestamp(epoch).strftime("%Y-%m-%d %H:%M:%S")


def default_memory_db():
    state = os.environ.get("HU_STATE_DIR") or os.path.expanduser("~/.human")
    return os.path.join(state, "memory.db")


def _norm(text):
    return " ".join((text or "").lower().split())


def load_daemon_sends(mem_path):
    """{contact: [(epoch_s or None, normalized_text)]} of every message the
    daemon produced, read-only. Raises FileNotFoundError when there is no db."""
    if not os.path.isfile(mem_path):
        raise FileNotFoundError(mem_path)
    con = sqlite3.connect(f"file:{mem_path}?mode=ro", uri=True)
    con.text_factory = lambda b: b.decode("utf-8", "replace")
    out = {}
    try:
        try:
            for contact, ms, text in con.execute(
                    "SELECT contact, sent_at_ms, text FROM outbound_sends WHERE text IS NOT NULL"):
                t = None if ms < UPTIME_STAMP_MAX_MS else ms / 1000.0
                out.setdefault(contact, []).append((t, _norm(text)))
        except sqlite3.OperationalError:
            pass  # memory.db that predates outbound_sends
        try:
            for contact, created, text in con.execute(
                    "SELECT session_id, CAST(strftime('%s', created_at) AS INTEGER), content "
                    "FROM messages WHERE role = 'assistant'"):
                out.setdefault(contact, []).append((created, _norm(text)))
        except sqlite3.OperationalError:
            pass
    finally:
        con.close()
    return out


def daemon_sent(sends, contact, ts, text):
    """True when `text` sent at `ts` to `contact` matches a daemon record:
    either text contains the other (the daemon splits a reply into bubbles),
    and the record is within PROVENANCE_WINDOW_S (or has no usable time)."""
    t = _norm(text)
    if not t:
        return False
    for when, rec in sends.get(contact, ()):
        if when is not None and abs(when - ts) > PROVENANCE_WINDOW_S:
            continue
        if rec and (t in rec or rec in t):
            return True
    return False


def daemon_active(sends, contact, ts):
    """True when the daemon produced anything for `contact` near `ts` — a
    tapback then may be the director's, and chat.db cannot tell."""
    return any(when is not None and abs(when - ts) <= PROVENANCE_WINDOW_S
               for when, _ in sends.get(contact, ()))


def run_dir_for(name, root):
    """The private run dir. Refuses anything inside the repo."""
    if not name or "/" in name or name.startswith("."):
        raise ValueError("--name must be a plain directory name")
    path = os.path.realpath(os.path.join(os.path.expanduser(root), name))
    if path == REPO_ROOT or path.startswith(REPO_ROOT + os.sep):
        raise ValueError("run dir must be outside the repository")
    return path


def make_private_dir(path):
    os.makedirs(path, mode=0o700, exist_ok=True)
    os.chmod(path, 0o700)


def write_private_jsonl(path, rows):
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    os.fchmod(fd, 0o600)
    with os.fdopen(fd, "w") as f:
        for r in rows:
            f.write(json.dumps(r, ensure_ascii=False) + "\n")


def load_threads(con):
    """{chat_id: (handle, [rows])} for 1:1 chats, rows oldest first.

    A row is (ts, from_me, text_or_None, guid, tapback_target_guid_or_None)."""
    one_to_one = {
        cid for (cid,) in con.execute(
            "SELECT chat_id FROM chat_handle_join GROUP BY chat_id HAVING COUNT(*) = 1")
    }
    rows = con.execute(
        """
        SELECT cmj.chat_id, m.is_from_me, m.text, m.attributedBody, m.date, m.guid,
               m.associated_message_type, m.associated_message_guid, h.id
        FROM message m
        JOIN chat_message_join cmj ON cmj.message_id = m.ROWID
        LEFT JOIN handle h ON h.ROWID = m.handle_id
        WHERE m.item_type = 0
        ORDER BY cmj.chat_id, m.date ASC
        """).fetchall()
    threads = {}
    undecoded = 0
    for chat_id, from_me, text, body, date, guid, amt, aguid, handle in rows:
        if chat_id not in one_to_one:
            continue
        handle_slot = threads.setdefault(chat_id, [None, []])
        if handle and not from_me and handle_slot[0] is None:
            handle_slot[0] = handle
        if amt and amt in TAPBACK_TYPES:
            target = (aguid or "").split("/")[-1] or None
            handle_slot[1].append((apple_ts(date), bool(from_me), None, guid, target))
            continue
        if amt:
            continue  # tapback removals, edits, other associations
        content = msg_text(text, body)
        if not content:
            if body is not None:
                undecoded += 1
            continue
        handle_slot[1].append((apple_ts(date), bool(from_me), content, guid, None))
    return {cid: (h, r) for cid, (h, r) in threads.items() if h}, undecoded


def turns_in_thread(handle, rows, history_n, max_gap_s):
    """Yield turns: their run of texts, then Seth's next text run or tapback.
    Every is_from_me row is a candidate; select_turns drops the daemon's."""
    i = 0
    n = len(rows)
    while i < n:
        if rows[i][1] or rows[i][2] is None:  # need their text to start a turn
            i += 1
            continue
        start = i
        while i < n and not rows[i][1] and rows[i][2] is not None:
            i += 1
        inbound = rows[start:i]
        inbound_guids = {r[3] for r in inbound}
        j = i
        while j < n and not rows[j][1] and rows[j][2] is None:
            j += 1  # their own tapbacks between the run and Seth's response
        if j >= n or not rows[j][1]:
            i = j
            continue
        if rows[j][0] - inbound[-1][0] > max_gap_s:
            i = j
            continue
        if rows[j][2] is None:  # Seth's first response is a tapback
            if rows[j][4] not in inbound_guids:
                i = j + 1
                continue
            action, reply, reply_ts = "tapback", [], [rows[j][0]]
            k = j + 1
        else:
            k = j
            reply, reply_ts = [], []
            while k < n and rows[k][1] and rows[k][2] is not None:
                reply.append(rows[k][2])
                reply_ts.append(rows[k][0])
                k += 1
            action = "text"
        hist_rows = [r for r in rows[max(0, start - 4 * history_n):start] if r[2] is not None]
        history = [{"from_me": r[1], "text": r[2], "ts": fmt_local(r[0])}
                   for r in hist_rows[-history_n:]]
        yield {
            "contact_id": handle,
            "ts": inbound[-1][0],
            "inbound_bubbles": [r[2] for r in inbound],
            "history": history,
            "seth_action": action,
            "seth_reply_bubbles": reply,
            "_reply_ts": reply_ts,
        }
        i = k


def is_daemon_turn(turn, sends):
    """The response in this turn came from the daemon, not Seth."""
    c = turn["contact_id"]
    if turn["seth_action"] == "tapback":
        return daemon_active(sends, c, turn["_reply_ts"][0])
    return any(daemon_sent(sends, c, ts, text)
               for ts, text in zip(turn["_reply_ts"], turn["seth_reply_bubbles"]))


def select_turns(threads, limit, per_contact, since_ts, history_n, max_gap_s, sends=None,
                 excluded=None):
    """Newest turns first, round-robin across contacts, capped per contact.

    Newest first because the memory snapshot is taken now: the closer a turn is
    to today, the less the replay's memory knows about its own future."""
    buckets = []
    for handle, rows in threads.values():
        ts = []
        for t in turns_in_thread(handle, rows, history_n, max_gap_s):
            if t["ts"] < since_ts:
                continue
            if sends is not None and is_daemon_turn(t, sends):
                if excluded is not None:
                    excluded.append(t["ts"])
                continue
            ts.append(t)
        ts.sort(key=lambda t: t["ts"], reverse=True)
        if ts:
            buckets.append(ts[:per_contact] if per_contact > 0 else ts)
    buckets.sort(key=lambda b: b[0]["ts"], reverse=True)
    out = []
    depth = 0
    while len(out) < limit and any(depth < len(b) for b in buckets):
        for b in buckets:
            if depth < len(b) and len(out) < limit:
                out.append(b[depth])
        depth += 1
    out.sort(key=lambda t: t["ts"])
    for k, t in enumerate(out, 1):
        t["id"] = f"t{k:04d}"
        t.pop("_reply_ts", None)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--name", required=True, help="run name (dir under --run-root)")
    ap.add_argument("--run-root", default="~/blind_ab_run")
    ap.add_argument("--db", default=default_db())
    ap.add_argument("--limit", type=int, default=40)
    ap.add_argument("--per-contact", type=int, default=3)
    ap.add_argument("--since-days", type=int, default=30)
    ap.add_argument("--history", type=int, default=20, help="thread messages kept per turn")
    ap.add_argument("--max-gap-min", type=int, default=240,
                    help="Seth's response must start within this many minutes")
    ap.add_argument("--memory-db", default=default_memory_db(),
                    help="memory.db holding the daemon's send provenance (read-only)")
    a = ap.parse_args(argv)
    try:
        run_dir = run_dir_for(a.name, a.run_root)
    except ValueError as e:
        print(f"refusing: {e}", file=sys.stderr)
        return 2
    if not os.path.exists(a.db):
        print("refusing: chat.db not found", file=sys.stderr)
        return 2
    try:
        sends = load_daemon_sends(a.memory_db)
    except FileNotFoundError:
        print("refusing: no memory.db for send provenance (--memory-db); without it the "
              "daemon's own replies would be counted as Seth's", file=sys.stderr)
        return 2
    con = sqlite3.connect(f"file:{a.db}?mode=ro", uri=True)
    try:
        threads, undecoded = load_threads(con)
    finally:
        con.close()
    since = 0 if a.since_days <= 0 else int(
        datetime.datetime.now().timestamp()) - a.since_days * 86400
    excluded = []
    turns = select_turns(threads, a.limit, a.per_contact, since, a.history, a.max_gap_min * 60,
                         sends=sends, excluded=excluded)
    if not turns:
        print("refusing: no turns matched (widen --since-days?)", file=sys.stderr)
        return 1
    make_private_dir(run_dir)
    write_private_jsonl(os.path.join(run_dir, "turns.jsonl"), turns)
    tapbacks = sum(1 for t in turns if t["seth_action"] == "tapback")
    print(f"wrote {len(turns)} turns ({tapbacks} where Seth tapbacked) from "
          f"{len({t['contact_id'] for t in turns})} contacts to {run_dir}/turns.jsonl")
    print(f"  1:1 threads scanned: {len(threads)}; undecodable bodies skipped: {undecoded}")
    print(f"  turns excluded because the daemon sent the response: {len(excluded)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
