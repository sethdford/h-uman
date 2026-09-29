"""Rate Gemma's reference replies (spec §4.3): write a sheet of unrated rows,
then import Seth's y/n back into the store."""
import argparse
import csv
import json
import sys

from . import store

FIELDS = ["id", "context", "reply", "good"]


def write_rating_sheet(store_con, out_csv):
    rows = store_con.execute("SELECT id, context, reply FROM reference_replies"
                             " WHERE rated IS NULL ORDER BY id").fetchall()
    with store.private_open(out_csv, newline="") as f:
        w = csv.DictWriter(f, fieldnames=FIELDS)
        w.writeheader()
        for rid, ctx, reply in rows:
            w.writerow({"id": rid, "context": store.csv_safe(ctx), "reply": store.csv_safe(reply),
                       "good": ""})
    return len(rows)


def import_ratings(store_con, sheet_csv):
    """Imports as ONE transaction: on any unexpected error, roll back and
    re-raise, so no partial import can later be committed by someone else's
    commit on the same connection. Only rows the UPDATE actually touched
    (cursor.rowcount == 1) count as rated; an id matching no row is
    "unknown_ids", a non-integer id is "skipped" (same bucket as an
    unparseable good/bad answer)."""
    c = {"rated_good": 0, "rated_bad": 0, "skipped": 0, "unknown_ids": 0}
    try:
        with open(sheet_csv, newline="") as f:
            rows = list(csv.DictReader(f))
        for r in rows:
            v = store.parse_yes_no(r.get("good"))
            if v is None:
                c["skipped"] += 1
                continue
            try:
                rid = int(r["id"])
            except (TypeError, ValueError):
                c["skipped"] += 1
                continue
            cur = store_con.execute("UPDATE reference_replies SET rated = ? WHERE id = ?",
                                    (1 if v else 0, rid))
            if cur.rowcount == 1:
                c["rated_good" if v else "rated_bad"] += 1
            else:
                c["unknown_ids"] += 1
        store_con.commit()
    except Exception:
        store_con.rollback()
        raise
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
