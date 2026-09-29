#!/usr/bin/env python3
"""Phase 1 of docs/plans/2026-08-02-semantic-retrieval/spec.md.

Does semantic recall beat keyword on paraphrased queries, over the REAL corpus?

Keyword baseline is SQLite FTS5 via the existing `memories_fts` table -- the
retriever the system actually uses -- not a strawman. Semantic is
nomic-embed-text-v2 through MLX, in-process, no HTTP.

WHAT THIS MEASURES: paraphrase robustness -- can a query reach a stored memory
that means the same thing in different words. That is precisely what embeddings
buy and keyword cannot do. It does NOT measure end-to-end reply quality and must
not be reported as if it did.

REFUSES rather than falls back. If the embedder is unavailable, or is the fake
hash-projection `embedder_local`, this exits non-zero and writes nothing --
a number from a different path is worse than no number
(.claude/rules/no-number-without-a-measurement.md).
"""
import argparse
import json
import os
import re
import sqlite3
import sys

DB = os.path.expanduser("~/.human/memory.db")
MODEL = "mlx-community/nomicai-modernbert-embed-base-8bit"

# Paraphrase probes: (query, substring that identifies the target memory).
# Each query deliberately shares little or no vocabulary with its target, which
# is the whole point -- keyword cannot bridge that gap, embeddings should.
PROBES = [
    ("what does he think of that wood panelling style?", "batten boards"),
    ("who is going to arrange the fitting?", "find an installer"),
    ("is he looking at new places to live?", "apartment tours"),
    ("is he still trying to sell the house?", "keeping it listed for sale"),
    ("sending a bit of money for household stuff", "cleaning supplies"),
    ("getting food ready for the party", "food prepping"),
    ("does he joke about the assistant?", "digital intern"),
    ("someone offered to fly out and look after him", "come see you"),
    ("unpaid bills with contractors", "payment decisions"),
    ("weekend plans with his partner", "plan for saturday"),
    ("balancing work and home life", "professional responsibilities and personal life"),
    ("he was surprised by something a person said", "cant believe he said that"),
    ("a document was delivered", "sent the document"),
    ("short polite back-and-forth", "cordial exchanges"),
    ("arranging a ride and building access", "Uber"),
    ("apologising for a small mistake", "Ok sorryy"),
    ("agreeing to speak again later", "text me later"),
    ("juggling admin tasks and deadlines at work", "administrative matters and time management"),
    ("the assistant does both deep technical work and personal chat", "hybrid capacity"),
    ("good things said about him professionally", "Positive professional interactions"),
]


def load_corpus(db_path):
    con = sqlite3.connect(db_path)
    # memories.content holds rows with invalid UTF-8 (mangled smart quotes from
    # attributedBody decoding), which makes the default text_factory raise mid-scan.
    # Replace the bad bytes rather than dropping the row: a decode error is not a
    # reason to silently shrink the corpus the measurement runs over.
    con.text_factory = lambda b: b.decode("utf-8", errors="replace")
    rows = con.execute(
        "SELECT id, content FROM memories WHERE content IS NOT NULL AND LENGTH(content) > 10"
    ).fetchall()
    return con, [(str(i), c) for i, c in rows]


def keyword_rank(con, query, target_sub, k=5):
    """Rank via the FTS5 index the system actually uses."""
    # FTS5 MATCH needs bare terms; strip punctuation and drop stopwords-ish shorts.
    terms = [t for t in re.findall(r"[A-Za-z']+", query) if len(t) > 2]
    if not terms:
        return None
    q = " OR ".join(terms)
    try:
        rows = con.execute(
            "SELECT m.content FROM memories_fts f JOIN memories m ON m.rowid = f.rowid "
            "WHERE memories_fts MATCH ? ORDER BY bm25(memories_fts) LIMIT ?", (q, k)
        ).fetchall()
    except sqlite3.Error:
        return None
    for rank, (content,) in enumerate(rows, 1):
        if target_sub.lower() in (content or "").lower():
            return rank
    return None


def semantic_rank(embed_fn, corpus_vecs, corpus, query, target_sub, k=5):
    import mlx.core as mx
    qv = embed_fn([query])[0]
    sims = corpus_vecs @ qv
    order = mx.argsort(-sims)[:k].tolist()
    for rank, idx in enumerate(order, 1):
        if target_sub.lower() in corpus[idx][1].lower():
            return rank
    return None


def _cli_ranks(bin_path, args, target_sub, k):
    """Run `human memory search ...` and return the 1-based rank of the first
    line whose content contains target_sub, else None. Output lines look like
    `  [3] key (0.812): content...` (semantic) or `  [3] key: content` (keyword)."""
    import subprocess
    try:
        # bytes, not text: memory content carries invalid UTF-8 (mangled smart
        # quotes); a decode error must not abort the measurement.
        out = subprocess.run([bin_path, "memory", "search", *args], capture_output=True,
                             timeout=120).stdout.decode("utf-8", errors="replace")
    except subprocess.TimeoutExpired:
        sys.exit("FATAL: CLI search timed out — refusing to score a hung path")
    rank = 0
    for line in out.splitlines():
        m = re.match(r"\s*\[(\d+)\]\s+(.*)$", line)
        if not m:
            continue
        rank += 1
        if rank > k:
            break
        if target_sub.lower() in m.group(2).lower():
            return rank
    return None


def main_via_cli(a):
    """Phase-2 proof: the same probes, but every rank comes from the shipped
    binary's own retrieval — keyword through the engine's FTS5 recall, semantic
    through the HTTP embedder + sqlite-vec store. Refuses if the semantic path
    errors on the first probe (an unreachable embedder must not score as 0)."""
    import subprocess
    probe = subprocess.run([a.via_cli, "memory", "search", "--semantic", PROBES[0][0]],
                           capture_output=True, timeout=120)
    perr = probe.stderr.decode("utf-8", errors="replace")
    if probe.returncode != 0 or "cannot attach" in perr or "search --semantic:" in perr:
        sys.exit(f"FATAL: semantic CLI path unavailable: {perr.strip()[:200]} — refusing to score")
    rows = []
    for q, target in PROBES:
        kw = _cli_ranks(a.via_cli, [q], target, a.k)
        sem = _cli_ranks(a.via_cli, ["--semantic", q], target, a.k)
        rows.append({"query": q, "target": target, "keyword_rank": kw, "semantic_rank": sem})
        print(f"  kw={kw or '-':<3} sem={sem or '-':<3}  {q[:56]}")
    n = len(rows)
    rec = lambda key, at: sum(1 for r in rows if r[key] and r[key] <= at) / n
    mrr = lambda key: sum(1.0 / r[key] for r in rows if r[key]) / n
    res = {"n_probes": n, "path": "production C binary via CLI", "binary": a.via_cli,
           "db": os.environ.get("HU_MEMORY_SQLITE_PATH", a.db),
           "embed_url": os.environ.get("HU_SEMANTIC_EMBED_URL", "http://127.0.0.1:8741"),
           "measures": "paraphrase robustness on the LIVE retrieval path; NOT end-to-end reply quality",
           "keyword": {"recall@1": rec("keyword_rank", 1), "recall@5": rec("keyword_rank", a.k), "mrr": mrr("keyword_rank")},
           "semantic": {"recall@1": rec("semantic_rank", 1), "recall@5": rec("semantic_rank", a.k), "mrr": mrr("semantic_rank")},
           "rows": rows}
    delta = (res["semantic"]["recall@5"] - res["keyword"]["recall@5"]) * 100
    res["recall@5_delta_points"] = round(delta, 1)
    res["pass"] = delta >= 20.0
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    json.dump(res, open(a.out, "w"), indent=2)
    print(f"\n[phase2-cli] n={n}  keyword recall@5 {res['keyword']['recall@5']:.2f}  semantic recall@5 "
          f"{res['semantic']['recall@5']:.2f}  delta {delta:+.1f} (pass >= +20) -> {'PASS' if res['pass'] else 'FAIL'}")
    print(f"  wrote {a.out}")
    return 0 if res["pass"] else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=DB)
    ap.add_argument("--model", default=MODEL)
    ap.add_argument("--out", default="docs/plans/2026-08-02-semantic-retrieval/phase1-results.json")
    ap.add_argument("--k", type=int, default=5)
    ap.add_argument("--via-cli", metavar="HUMAN_BIN", default=None,
                    help="Phase 2: rank through the PRODUCTION C path instead of in-process — "
                         "`<bin> memory search <q>` (engine FTS5) vs `<bin> memory search --semantic <q>` "
                         "(HTTP embedder + sqlite-vec). Honours HU_MEMORY_SQLITE_PATH / HU_SEMANTIC_EMBED_URL.")
    a = ap.parse_args()
    if a.via_cli:
        return main_via_cli(a)

    # --- the embedder must be REAL. Never score the hash-projection stub. ---
    try:
        import mlx.core as mx
        from mlx_embeddings import load as emb_load
    except ImportError as e:
        sys.exit(f"FATAL: mlx_embeddings unavailable ({e}). Refusing to score with a "
                 f"fallback embedder -- embedder_local.c is a hash projection with no "
                 f"semantics and would produce a plausible, meaningless number.")
    try:
        model, tok = emb_load(a.model)
    except Exception as e:
        sys.exit(f"FATAL: could not load {a.model} ({type(e).__name__}: {e}). Refusing to score.")

    def embed(texts):
        out = model.encode(texts, tokenizer=tok) if hasattr(model, "encode") else None
        if out is None:
            ins = tok.batch_encode_plus(texts, return_tensors="mlx", padding=True,
                                        truncation=True, max_length=512)
            out = model(ins["input_ids"], attention_mask=ins.get("attention_mask")).text_embeds
        v = out if not hasattr(out, "text_embeds") else out.text_embeds
        v = v / mx.linalg.norm(v, axis=-1, keepdims=True)
        return v

    con, corpus = load_corpus(a.db)
    print(f"[phase1] corpus: {len(corpus)} memories; embedder: {a.model}")

    vecs = []
    B = 64
    for i in range(0, len(corpus), B):
        vecs.append(embed([c for _, c in corpus[i:i + B]]))
        print(f"\r[phase1] embedded {min(i+B, len(corpus))}/{len(corpus)}", end="", flush=True)
    corpus_vecs = mx.concatenate(vecs, axis=0)
    print()

    rows = []
    for q, target in PROBES:
        kw = keyword_rank(con, q, target, a.k)
        sem = semantic_rank(embed, corpus_vecs, corpus, q, target, a.k)
        rows.append({"query": q, "target": target, "keyword_rank": kw, "semantic_rank": sem})
        print(f"  kw={kw or '-':<3} sem={sem or '-':<3}  {q[:56]}")

    n = len(rows)
    if n == 0:
        sys.exit("FATAL: zero probes -- an empty query set satisfies any pass criterion.")

    def rec(key, at):
        return sum(1 for r in rows if r[key] and r[key] <= at) / n

    def mrr(key):
        return sum(1.0 / r[key] for r in rows if r[key]) / n

    res = {
        "n_probes": n, "corpus_size": len(corpus), "embedder": a.model,
        "keyword_backend": "sqlite fts5 memories_fts + bm25",
        "measures": "paraphrase robustness only; NOT end-to-end reply quality",
        "keyword": {"recall@1": rec("keyword_rank", 1), "recall@5": rec("keyword_rank", a.k),
                    "mrr": mrr("keyword_rank")},
        "semantic": {"recall@1": rec("semantic_rank", 1), "recall@5": rec("semantic_rank", a.k),
                     "mrr": mrr("semantic_rank")},
        "rows": rows,
    }
    delta = (res["semantic"]["recall@5"] - res["keyword"]["recall@5"]) * 100
    res["recall@5_delta_points"] = round(delta, 1)
    res["pass"] = delta >= 20.0   # criterion fixed in the spec BEFORE running

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    json.dump(res, open(a.out, "w"), indent=2)

    print(f"\n[phase1] n={n} over {len(corpus)} memories")
    print(f"  keyword : recall@1 {res['keyword']['recall@1']:.2f}  "
          f"recall@5 {res['keyword']['recall@5']:.2f}  MRR {res['keyword']['mrr']:.3f}")
    print(f"  semantic: recall@1 {res['semantic']['recall@1']:.2f}  "
          f"recall@5 {res['semantic']['recall@5']:.2f}  MRR {res['semantic']['mrr']:.3f}")
    print(f"  delta recall@5: {delta:+.1f} points (pass needs >= +20)  -> "
          f"{'PASS' if res['pass'] else 'FAIL'}")
    print(f"  wrote {a.out}")
    return 0 if res["pass"] else 1


if __name__ == "__main__":
    sys.exit(main())
