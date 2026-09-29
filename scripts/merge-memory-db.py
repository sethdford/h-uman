#!/usr/bin/env python3
"""Merge a quarantined memory.db back with the rows written since it was set aside.

    merge-memory-db.py --base <old.db> --delta <new.db> --out <merged.db> \
        [--must-grow t1,t2] [--dry-run]

--base  is the COMPLETE store up to the quarantine (e.g. memory.db.corrupt-<ts>)
--delta is the store the daemon created afterwards (rows since)
--out   is written fresh (never in place). Both inputs are opened read-only.

Rules, per table in delta:
  * absent in base  -> CREATE from delta's sqlite_master.sql, copy every row
  * present in base -> INSERT OR IGNORE base.t(<shared cols>) SELECT <shared cols>
                       FROM delta.t. For AUTOINCREMENT tables the id column is
                       DROPPED from the column list: ids restarted at 1 in the
                       delta and would collide with unrelated base rows. Dedup
                       then rides on the table's own UNIQUE constraints — which
                       is exactly what OR IGNORE is for.
Indexes/triggers/views from base are kept; delta-only ones are created.
Refuses (exit 2) when --out fails PRAGMA quick_check or any --must-grow table
did not grow. Prints a before/added/after table so the numbers are the evidence.
"""
import argparse, os, shutil, sqlite3, sys

def tables(con, schema="main"):
    return {r[0]: r[1] for r in con.execute(
        f"SELECT name, sql FROM {schema}.sqlite_master WHERE type='table' "
        "AND name NOT LIKE 'sqlite_%'")}

def cols(con, schema, t):
    return [r[1] for r in con.execute(f"PRAGMA {schema}.table_info(\"{t}\")")]

def autoinc_col(con, schema, t, sql):
    if not sql or "AUTOINCREMENT" not in sql.upper():
        return None
    for r in con.execute(f"PRAGMA {schema}.table_info(\"{t}\")"):
        if r[5]:  # pk
            return r[1]
    return None

def merge(base, delta, out, must_grow=(), dry_run=False):
    if os.path.exists(out):
        raise SystemExit(f"refusing to overwrite {out}")
    shutil.copyfile(base, out)
    for sib in ("-wal", "-shm"):
        if os.path.exists(base + sib):
            shutil.copyfile(base + sib, out + sib)
    con = sqlite3.connect(out)
    con.execute("PRAGMA journal_mode=WAL")
    con.execute(f"ATTACH DATABASE 'file:{delta}?mode=ro' AS d")
    base_t, delta_t = tables(con, "main"), tables(con, "d")
    report = []
    con.execute("BEGIN")
    for t, dsql in sorted(delta_t.items()):
        if t.endswith("_fts") or "_fts_" in t or t.startswith("memories_fts"):
            continue  # virtual/shadow tables: rebuilt below
        d_cnt = con.execute(f"SELECT COUNT(*) FROM d.\"{t}\"").fetchone()[0]
        if t not in base_t:
            con.execute(dsql)
            con.execute(f"INSERT INTO main.\"{t}\" SELECT * FROM d.\"{t}\"")
            report.append((t, 0, d_cnt, d_cnt, "created"))
            continue
        before = con.execute(f"SELECT COUNT(*) FROM main.\"{t}\"").fetchone()[0]
        shared = [c for c in cols(con, "d", t) if c in set(cols(con, "main", t))]
        ai = autoinc_col(con, "main", t, base_t[t])
        if ai in shared:
            shared.remove(ai)
        if not shared:
            report.append((t, before, 0, before, "no shared cols")); continue
        cl = ", ".join(f'"{c}"' for c in shared)
        con.execute(f"INSERT OR IGNORE INTO main.\"{t}\" ({cl}) SELECT {cl} FROM d.\"{t}\"")
        after = con.execute(f"SELECT COUNT(*) FROM main.\"{t}\"").fetchone()[0]
        report.append((t, before, after - before, after, "autoinc" if ai else "keyed"))
    # Delta-only NON-unique indexes (idempotent). UNIQUE indexes are left to the
    # engine's own schema migration at next open: it dedupes first, then creates
    # them, and a failure there is logged by the daemon instead of hidden here.
    for (sql,) in con.execute("SELECT sql FROM d.sqlite_master WHERE type='index' AND sql IS NOT NULL "
                              "AND upper(sql) NOT LIKE 'CREATE UNIQUE%'").fetchall():
        try:
            con.execute(sql.replace("CREATE INDEX", "CREATE INDEX IF NOT EXISTS", 1))
        except sqlite3.Error as e:
            print(f"  [warn] index not created: {e}: {sql[:80]}")
    if dry_run:
        con.execute("ROLLBACK")
    else:
        con.execute("COMMIT")
        # FTS shadow tables cannot be merged row-wise; rebuild from content.
        for (name,) in con.execute("SELECT name FROM main.sqlite_master WHERE type='table' AND sql LIKE 'CREATE VIRTUAL TABLE%fts5%'").fetchall():
            try: con.execute(f"INSERT INTO \"{name}\"(\"{name}\") VALUES('rebuild')")
            except sqlite3.Error as e: print(f"  [warn] fts rebuild {name}: {e}")
        con.commit()
    con.execute("DETACH DATABASE d")
    qc = con.execute("PRAGMA quick_check").fetchone()[0]
    con.close()
    print(f"{'table':32} {'before':>8} {'added':>7} {'after':>8}  how")
    for t, b, a, af, how in report:
        print(f"{t:32} {b:8d} {a:7d} {af:8d}  {how}")
    print(f"quick_check: {qc}")
    bad = [t for t in must_grow if not any(r[0] == t and r[2] > 0 for r in report)]
    if qc != "ok":
        raise SystemExit(f"FATAL: merged DB quick_check = {qc}")
    if bad:
        raise SystemExit(f"FATAL: tables in --must-grow did not grow: {bad}")
    return report

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base", required=True); ap.add_argument("--delta", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--must-grow", default="")
    ap.add_argument("--dry-run", action="store_true")
    a = ap.parse_args()
    merge(a.base, a.delta, a.out, tuple(x for x in a.must_grow.split(",") if x), a.dry_run)

if __name__ == "__main__":
    sys.exit(main())
