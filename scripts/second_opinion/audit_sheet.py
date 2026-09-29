"""Build the one-time 30-row check sheet (spec §4.1). The verdict is hidden;
the key stays private next to the sheet. Only one backend's verdicts on one
source population are drawn (default: local Gemma on curator_wide notes, the
population the HU_INSIGHT_WIDE gate is about), and no insight appears twice.
Any picked insight whose memory.db row is gone, or whose evidence doesn't
resolve, is skipped and backfilled from the remaining pool of that verdict;
refuses (returns 2, writes nothing) if the resolvable pool can't fill
n_unsupported + n_supported."""
import argparse
import csv
import json
import os
import random
import sqlite3
import sys

from . import audit, backend as be, store


def _resolvable_picks(store_con, mem, chat, verdict, n, rng, backend=None, source="all",
                      exclude=()):
    """Up to n (insight_id, note, cited_texts) triples for `verdict` from the
    given backend/source, skipping any insight in `exclude` or whose memory.db
    row is gone or whose evidence doesn't resolve. None if the resolvable pool
    can't fill n."""
    where, args = audit.audit_filter(backend, source)
    ids = [r[0] for r in store_con.execute(
        "SELECT DISTINCT insight_id FROM audits" + where + " AND verdict = ?"
        " ORDER BY insight_id", args + [verdict])]
    rng.shuffle(ids)
    picked = []
    for iid in ids:
        if len(picked) >= n:
            break
        if iid in exclude:
            continue
        rec = mem.execute("SELECT insight, evidence_ids FROM contact_insights WHERE id = ?",
                          (iid,)).fetchone()
        if not rec:
            continue
        texts = audit.resolve_evidence(rec[1], mem, chat)
        if not texts:
            continue
        picked.append((iid, rec[0], texts))
    return picked if len(picked) >= n else None


def key_path_for(out_csv):
    return os.path.splitext(out_csv)[0] + ".key.json"


def write_check_sheet(store_con, mem, chat, out_csv, key_json, n_unsupported=20, n_supported=10,
                      seed=0, backend=None, source="all"):
    if os.path.abspath(key_json) == os.path.abspath(out_csv):
        print("refusing: the key path equals the sheet path", file=sys.stderr)
        return 2
    rng = random.Random(seed)
    uns = _resolvable_picks(store_con, mem, chat, "unsupported", n_unsupported, rng,
                            backend, source)
    taken = {iid for iid, _, _ in uns or ()}
    sup = _resolvable_picks(store_con, mem, chat, "supported", n_supported, rng, backend, source,
                            exclude=taken)
    if uns is None or sup is None:
        print(f"refusing: need {n_unsupported} resolvable unsupported and {n_supported} "
              "resolvable supported audits", file=sys.stderr)
        return 2
    picks = [(iid, note, texts, "unsupported") for iid, note, texts in uns] + \
            [(iid, note, texts, "supported") for iid, note, texts in sup]
    rng.shuffle(picks)
    where, args = audit.audit_filter(backend, source)
    ids = [p[0] for p in picks]
    versions = sorted(r[0] for r in store_con.execute(
        "SELECT DISTINCT prompt_version FROM audits" + where
        + f" AND insight_id IN ({','.join('?' * len(ids))})", args + ids))
    rows, key = [], {"_meta": {"backend": backend, "source": source,
                               "prompt_versions": versions}}
    for n, (iid, note, texts, verdict) in enumerate(picks):
        rows.append({"row": str(n), "note": store.csv_safe(note),
                     "cited_messages": store.csv_safe(" | ".join(texts)), "supported": ""})
        key[str(n)] = {"insight_id": iid, "gemma": verdict}
    store.prepare_output(out_csv)
    store.prepare_output(key_json)
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
    ap.add_argument("--out", default=os.path.join(store.default_dir(), "audit_check.csv"))
    ap.add_argument("--backend", default=be.GemmaBackend().name,
                    help="whose verdicts to calibrate (default: the local Gemma backend)")
    ap.add_argument("--source", choices=["wide", "persona", "all"], default="wide",
                    help="note population (default wide: the HU_INSIGHT_WIDE gate)")
    a = ap.parse_args(argv)
    mem = sqlite3.connect(f"file:{a.mem_db}?mode=ro", uri=True)
    chat = sqlite3.connect(f"file:{a.chat_db}?mode=ro", uri=True)
    return write_check_sheet(store.open_store(a.store), mem, chat, a.out, key_path_for(a.out),
                             backend=a.backend, source=a.source)


if __name__ == "__main__":
    sys.exit(main())
