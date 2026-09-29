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
    with store.private_open(out_csv, newline="") as f:
        w = csv.writer(f)
        w.writerow(["contact_id", "context", "reply", "backend"])
        w.writerows(rows)
    return len(rows)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    default_out = os.path.join(store.default_dir(), "gold_export.csv")
    ap.add_argument("out", nargs="?", default=default_out,
                    help=f"output CSV (default {default_out})")
    ap.add_argument("--store", default=store.DEFAULT_PATH)
    a = ap.parse_args(argv)
    n = export_rated(store.open_store(a.store), store.prepare_output(a.out))
    print(f"{n} rated-good replies exported to {a.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
