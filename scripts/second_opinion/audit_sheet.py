"""Build the one-time 30-row check sheet (spec §4.1). The verdict is hidden;
the key stays private next to the sheet."""
import argparse
import csv
import json
import os
import random
import sqlite3
import sys

from . import audit, store


def _pick(store_con, verdict, n, rng):
    ids = [r[0] for r in store_con.execute(
        "SELECT DISTINCT insight_id FROM audits WHERE verdict = ?", (verdict,))]
    rng.shuffle(ids)
    return ids[:n] if len(ids) >= n else None


def write_check_sheet(store_con, mem, chat, out_csv, key_json, n_unsupported=20, n_supported=10,
                      seed=0):
    rng = random.Random(seed)
    uns = _pick(store_con, "unsupported", n_unsupported, rng)
    sup = _pick(store_con, "supported", n_supported, rng)
    if uns is None or sup is None:
        print(f"refusing: need {n_unsupported} unsupported and {n_supported} supported audits",
              file=sys.stderr)
        return 2
    picks = [(i, "unsupported") for i in uns] + [(i, "supported") for i in sup]
    rng.shuffle(picks)
    rows, key = [], {}
    for n, (iid, verdict) in enumerate(picks):
        rec = mem.execute("SELECT insight, evidence_ids FROM contact_insights WHERE id = ?",
                          (iid,)).fetchone()
        texts = audit.resolve_evidence(rec[1], mem, chat) if rec else []
        rows.append({"row": str(n), "note": rec[0] if rec else "",
                     "cited_messages": " | ".join(texts), "supported": ""})
        key[str(n)] = {"insight_id": iid, "gemma": verdict}
    with open(out_csv, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["row", "note", "cited_messages", "supported"])
        w.writeheader()
        w.writerows(rows)
    with open(key_json, "w") as f:
        json.dump(key, f, indent=1)
    os.chmod(out_csv, 0o600)
    os.chmod(key_json, 0o600)
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--store", default=store.DEFAULT_PATH)
    ap.add_argument("--mem-db", default=os.path.expanduser("~/.human/memory.db"))
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--out", default=os.path.expanduser("~/.human/second_opinion/audit_check.csv"))
    a = ap.parse_args(argv)
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    mem = sqlite3.connect(f"file:{a.mem_db}?mode=ro", uri=True)
    chat = sqlite3.connect(f"file:{a.chat_db}?mode=ro", uri=True)
    return write_check_sheet(store.open_store(a.store), mem, chat, a.out,
                             a.out.replace(".csv", ".key.json"))


if __name__ == "__main__":
    sys.exit(main())
