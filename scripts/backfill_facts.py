#!/usr/bin/env python3
"""Task 4 of docs/superpowers/plans/2026-09-01-sota-e2e.md — extract facts from
the user's OWN historical iMessages into a JSONL the C importer
(`human memory import-facts`) feeds into ~/.human/graph.db.

Reuses the measured, filtered sampler and extractor from
eval_fact_extract_yield.py (reactions excluded, is_from_me=1, daemon-shaped
dropped, closed predicate vocabulary). Nothing here is new logic; the point is
that the 0.62 facts/msg yield was measured on exactly this path.

Read-only against chat.db. Writes only --out. Refuses (exit 2, --out removed)
if fewer than 60% of messages produce parseable output, so a dead server can
never look like an empty history.
"""
import argparse, json, os, sqlite3, sys, time, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eval_fact_extract_yield as y  # noqa: E402

def rows_with_ts(db):
    """Same filters as y.sample_messages, but keep rowid + timestamp so the
    importer can ingest in chronological order (supersession is time-ordered)."""
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "blind_ab"))
    from imessage_text import decode_attributed_body
    con = sqlite3.connect(f"file:{db}?mode=ro", uri=True)
    con.text_factory = bytes
    q = ("SELECT ROWID, text, attributedBody, date FROM message "
         "WHERE is_from_me = 1 AND COALESCE(associated_message_type,0) = 0 ORDER BY date ASC")
    out = []
    for rowid, txt, blob, date in con.execute(q):
        s = None
        if txt:
            s = txt.decode("utf-8", errors="replace")
        if not s and blob:
            try: s = decode_attributed_body(blob)
            except Exception: s = None
        if not s or len(s.strip()) < y.MIN_LEN: continue
        t = s.strip()
        if y.DAEMON_SHAPE.search(t): continue
        # chat.db date: ns since 2001-01-01 (or s on very old rows)
        ts = int(date // 1_000_000_000 if date > 10_000_000_000 else date) + 978307200
        out.append((rowid, ts, t))
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=y.CHATDB)
    ap.add_argument("--out", required=True)
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--timeout", type=int, default=120)
    a = ap.parse_args()
    msgs = rows_with_ts(a.db)
    if a.limit: msgs = msgs[:a.limit]
    print(f"[backfill] {len(msgs)} eligible own messages -> {a.out}", flush=True)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    n_ok = n_bad = n_facts = 0
    t0 = time.time()
    with open(a.out, "w") as f:
        for i, (rowid, ts, text) in enumerate(msgs, 1):
            try:
                facts, _raw = y.extract(text, a.timeout)
            except Exception as e:
                facts = None; print(f"  [{i}] ERROR {type(e).__name__}", flush=True)
            if facts is None:
                n_bad += 1
            else:
                n_ok += 1
                for fc in facts:
                    p = str(fc.get("predicate", ""))
                    if p not in y.CLOSED_PREDICATES: continue
                    obj = str(fc.get("object", "")).strip()
                    if not obj or len(obj) > 200: continue
                    f.write(json.dumps({"contact": "self", "subject": str(fc.get("subject", "user")) or "user",
                                        "predicate": p, "object": obj,
                                        "confidence": float(fc.get("confidence", 0.5) or 0.5),
                                        "ts": ts, "source": f"chat.db:{rowid}"}) + "\n")
                    n_facts += 1
            if i % 25 == 0:
                f.flush()
                print(f"  [{i}/{len(msgs)}] ok={n_ok} bad={n_bad} facts={n_facts} {time.time()-t0:.0f}s", flush=True)
    total = n_ok + n_bad
    if total == 0 or n_ok < 0.6 * total:
        os.remove(a.out)
        sys.exit(f"FATAL: parseable {n_ok}/{total} < 60% — refusing to emit a partial backfill (out removed)")
    print(f"[backfill] DONE ok={n_ok} bad={n_bad} facts={n_facts} ({n_facts/max(n_ok,1):.2f}/msg) in {time.time()-t0:.0f}s", flush=True)
    return 0

if __name__ == "__main__":
    sys.exit(main())
