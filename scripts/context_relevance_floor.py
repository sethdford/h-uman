#!/usr/bin/env python3
"""Derive HU_CONTEXT_RELEVANCE's semantic threshold from the live index.

The threshold is the cosine similarity an UNRELATED pair of stored memories
already reach with each other: the median over every pair of vectors in the
sqlite-vec index (`memories_vec`). A recalled hit must score at least that
high against the message before it is injected on a turn the word-count
cliff used to starve (docs/guides/context-relevance.md).

Reads only the float vectors (no text, keys or ids) from the vec0 shadow
tables, read-only. Prints quantiles; `--json` prints one JSON object.

    python3 scripts/context_relevance_floor.py [--db ~/.human/memory.db] [--json]

Exit 0 = measured; 2 = too few vectors to measure (never prints a threshold
it did not measure).
"""
from __future__ import annotations

import argparse
import json
import math
import os
import sqlite3
import struct
import sys

MIN_VECTORS = 10
QUANTILES = (0.5, 0.75, 0.9, 0.95, 0.99)


def default_db() -> str:
    state = os.environ.get("HU_STATE_DIR") or os.path.join(os.path.expanduser("~"), ".human")
    return os.path.join(state, "memory.db")


def load_vectors(db_path: str, table: str = "memories_vec") -> list[list[float]]:
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    try:
        dim_row = con.execute(
            "SELECT sql FROM sqlite_master WHERE name = ?", (table,)
        ).fetchone()
        if not dim_row or "float[" not in dim_row[0]:
            return []
        dim = int(dim_row[0].split("float[", 1)[1].split("]", 1)[0])
        chunks = {
            cid: blob
            for cid, blob in con.execute(f"SELECT rowid, vectors FROM {table}_vector_chunks00")
        }
        vecs = []
        for cid, off in con.execute(f"SELECT chunk_id, chunk_offset FROM {table}_rowids"):
            blob = chunks.get(cid)
            if blob is None or (off + 1) * dim * 4 > len(blob):
                continue
            v = struct.unpack_from(f"{dim}f", blob, off * dim * 4)
            n = math.sqrt(sum(x * x for x in v))
            if n > 0:
                vecs.append([x / n for x in v])
        return vecs
    finally:
        con.close()


def pair_quantiles(vecs: list[list[float]]) -> dict:
    sims = []
    for i in range(len(vecs)):
        a = vecs[i]
        for j in range(i + 1, len(vecs)):
            sims.append(sum(x * y for x, y in zip(a, vecs[j])))
    sims.sort()

    def q(p: float) -> float:  # nearest-rank
        return sims[min(len(sims) - 1, max(0, math.ceil(p * len(sims)) - 1))]

    return {"n_vectors": len(vecs), "n_pairs": len(sims),
            "quantiles": {f"p{int(p * 100)}": round(q(p), 4) for p in QUANTILES}}


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--db", default=default_db())
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)
    if not os.path.exists(args.db):
        print(f"no database at {args.db}", file=sys.stderr)
        return 2
    vecs = load_vectors(args.db)
    if len(vecs) < MIN_VECTORS:
        print(f"only {len(vecs)} vectors (< {MIN_VECTORS}); refusing to derive a threshold",
              file=sys.stderr)
        return 2
    out = pair_quantiles(vecs)
    out["suggested_min_score"] = out["quantiles"]["p50"]
    if args.json:
        print(json.dumps(out))
    else:
        print(f"vectors={out['n_vectors']} pairs={out['n_pairs']}")
        for k, v in out["quantiles"].items():
            print(f"  {k}: {v}")
        print(f"HU_CONTEXT_RELEVANCE_MIN_SCORE={out['suggested_min_score']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
