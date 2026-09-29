"""Rate Gemma's reference replies (spec §4.3): write a sheet of unrated rows,
then import Seth's y/n back into the store."""
import argparse
import csv
import json
import os
import sys

from . import store

FIELDS = ["id", "context", "reply", "good"]


def write_rating_sheet(store_con, out_csv):
    rows = store_con.execute("SELECT id, context, reply FROM reference_replies"
                             " WHERE rated IS NULL ORDER BY id").fetchall()
    with open(out_csv, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        for rid, ctx, reply in rows:
            w.writerow({"id": rid, "context": ctx, "reply": reply, "good": ""})
    os.chmod(out_csv, 0o600)
    return len(rows)


def import_ratings(store_con, sheet_csv):
    c = {"rated_good": 0, "rated_bad": 0, "skipped": 0}
    with open(sheet_csv, newline="") as f:
        for r in csv.DictReader(f):
            v = (r.get("good") or "").strip().lower()
            if v not in ("y", "n"):
                c["skipped"] += 1
                continue
            store_con.execute("UPDATE reference_replies SET rated = ? WHERE id = ?",
                              (1 if v == "y" else 0, int(r["id"])))
            c["rated_good" if v == "y" else "rated_bad"] += 1
    store_con.commit()
    return c


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--store", default=store.DEFAULT_PATH)
    ap.add_argument("--write", help="write unrated rows to this CSV")
    ap.add_argument("--import", dest="imp", help="import a completed CSV")
    a = ap.parse_args(argv)
    con = store.open_store(a.store)
    if a.write:
        print(f"{write_rating_sheet(con, a.write)} unrated reference replies written")
    if a.imp:
        print(json.dumps(import_ratings(con, a.imp)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
