"""Build the one-time 30-row check sheet (spec §4.1). The verdict is hidden;
the key stays private next to the sheet. Any picked insight whose memory.db row
is gone, or whose evidence doesn't resolve, is skipped and backfilled from the
remaining pool of that verdict; refuses (returns 2, writes nothing) if the
resolvable pool can't fill n_unsupported + n_supported."""
import argparse
import csv
import json
import os
import random
import sqlite3
import sys

from . import audit, store


def _resolvable_picks(store_con, mem, chat, verdict, n, rng):
    """Up to n (insight_id, note, cited_texts) triples for `verdict`, skipping any
    insight whose memory.db row is gone or whose evidence doesn't resolve. None if
    the resolvable pool can't fill n."""
    ids = [r[0] for r in store_con.execute(
        "SELECT DISTINCT insight_id FROM audits WHERE verdict = ? ORDER BY insight_id",
        (verdict,))]
    rng.shuffle(ids)
    picked = []
    for iid in ids:
        if len(picked) >= n:
            break
        rec = mem.execute("SELECT insight, evidence_ids FROM contact_insights WHERE id = ?",
                          (iid,)).fetchone()
        if not rec:
            continue
        texts = audit.resolve_evidence(rec[1], mem, chat)
        if not texts:
            continue
        picked.append((iid, rec[0], texts))
    return picked if len(picked) >= n else None


def write_check_sheet(store_con, mem, chat, out_csv, key_json, n_unsupported=20, n_supported=10,
                      seed=0):
    rng = random.Random(seed)
    uns = _resolvable_picks(store_con, mem, chat, "unsupported", n_unsupported, rng)
    sup = _resolvable_picks(store_con, mem, chat, "supported", n_supported, rng)
    if uns is None or sup is None:
        print(f"refusing: need {n_unsupported} resolvable unsupported and {n_supported} "
              "resolvable supported audits", file=sys.stderr)
        return 2
    picks = [(iid, note, texts, "unsupported") for iid, note, texts in uns] + \
            [(iid, note, texts, "supported") for iid, note, texts in sup]
    rng.shuffle(picks)
    rows, key = [], {}
    for n, (iid, note, texts, verdict) in enumerate(picks):
        rows.append({"row": str(n), "note": note, "cited_messages": " | ".join(texts),
                     "supported": ""})
        key[str(n)] = {"insight_id": iid, "gemma": verdict}
    os.makedirs(os.path.dirname(out_csv) or ".", exist_ok=True)
    os.makedirs(os.path.dirname(key_json) or ".", exist_ok=True)
    with store.private_open(out_csv, newline="") as f:
        w = csv.DictWriter(f, fieldnames=["row", "note", "cited_messages", "supported"])
        w.writeheader()
        w.writerows(rows)
    with store.private_open(key_json) as f:
        json.dump(key, f, indent=1)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--store", default=store.DEFAULT_PATH)
    ap.add_argument("--mem-db", default=os.path.expanduser("~/.human/memory.db"))
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--out", default=os.path.expanduser("~/.human/second_opinion/audit_check.csv"))
    a = ap.parse_args(argv)
    mem = sqlite3.connect(f"file:{a.mem_db}?mode=ro", uri=True)
    chat = sqlite3.connect(f"file:{a.chat_db}?mode=ro", uri=True)
    return write_check_sheet(store.open_store(a.store), mem, chat, a.out,
                             a.out.replace(".csv", ".key.json"))


if __name__ == "__main__":
    sys.exit(main())
