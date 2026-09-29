"""Export reference replies Seth rated good (rated = 1). Unrated or rejected
rows are never exported. No training export exists (spec §9)."""
import argparse
import csv
import os
import sys

from . import store


def export_rated(store_con, out_csv):
    rows = store_con.execute("SELECT contact_id, context, reply, backend FROM reference_replies"
                             " WHERE rated = 1 ORDER BY id").fetchall()
    with open(out_csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["contact_id", "context", "reply", "backend"])
        w.writerows(rows)
    os.chmod(out_csv, 0o600)
    return len(rows)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("out")
    ap.add_argument("--store", default=store.DEFAULT_PATH)
    a = ap.parse_args(argv)
    print(f"{export_rated(store.open_store(a.store), a.out)} rated-good replies exported")
    return 0


if __name__ == "__main__":
    sys.exit(main())
