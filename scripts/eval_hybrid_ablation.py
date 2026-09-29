#!/usr/bin/env python3
"""Contract C2 reconstructive-hybrid ABLATION study on LongMemEval-S.

Companion to eval_memory_benchmarks.py, reusing its build_db/sh/parse_keys/rrf
helpers so the DB-build (the expensive, embedding-heavy step) happens ONCE per
question and every ablation variant is measured against that SAME index —
instead of rebuilding+reindexing per variant, which would multiply embedder
load 7x for no reason.

Each variant is `human memory search --hybrid <q>` with HU_RECON_ABLATE set to
disable/vary one stage of the reconstructive pipeline (src/memory/retrieval/hybrid.c):
  asis               : unmodified Contract C2 (opts.reconstructive path as shipped)
  no_scene           : (b) scene-select disabled -- every scene in the RRF pool selected
  no_neighbors       : (c) neighbour expansion (stage 2) disabled
  no_rerank          : (d) cross-encoder rerank (stage 3) disabled
  no_temporal        : (e) time-bounded filter (stage 4) disabled
  force_sufficient   : (f) sufficiency check (stage 5) never falls back
  scene_coverage_first: (g) coverage-first scene reordering (only meaningful
                         combined with the default cover-the-limit selection)

Plain hybrid (harness-side RRF of kw+sem, no C reconstructive path) and raw
kw/sem are measured once per question as the (a) baseline comparison point.

Refuses (exit 2) when the embedder is unreachable or too few questions scored
(same contract as eval_memory_benchmarks.py) -- never emits a partial table.
"""
import collections, importlib.util, json, os, random, re, sys, time, urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("emb", os.path.join(HERE, "eval_memory_benchmarks.py"))
emb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(emb)

DATA = emb.DATA
VARIANTS = ["asis", "no_scene", "no_neighbors", "no_rerank", "no_temporal",
            "force_sufficient", "scene_coverage_first"]

# DISCOVERED DURING THIS STUDY (2026-09-03), not an ablation flag: whenever
# hu_hybrid_retrieve's reconstructive path is insufficient, it falls back to
# the plain RRF+cross-encoder merge -- and that fallback's
# search_results_to_entries() (src/memory/retrieval/hybrid.c) sets
# entry.key = entry.content, because hu_search_result_t carries no key field
# to round-trip through. `human memory search --hybrid` then prints the full
# CONTENT string in the key column for every fallback row. parse_keys()'s
# session-id extraction (key.split(":")[0][1:]) silently parses garbage from
# that content for those rows, which would corrupt R@5 scoring for any
# variant/question whose fallback rate is nonzero -- including the ORIGINAL
# memory-benchmarks-c2.json numbers, not just these ablations. Filtering to
# well-formed "s<sid>:t<turn>" keys (the only shape this harness's synthetic
# rows use) and tracking the fallback rate separately is the fix applied
# below; it is a measurement-script correction only, no production code
# changed to accommodate it.
import re as _re
VALID_KEY_RE = _re.compile(r"^s[^:]+:t\d+$")


def run_variant(binp, dbp, question, variant, embed_url):
    env = {"HU_SEMANTIC_EMBED_URL": embed_url}
    if variant != "asis":
        env["HU_RECON_ABLATE"] = variant
    out = emb.sh(binp, dbp, ["search", "--hybrid", question], env)
    return emb.parse_keys(out)


def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="./build/human")
    ap.add_argument("--limit", type=int, default=30)
    ap.add_argument("--min-q", type=int, default=25)
    ap.add_argument("--seed", type=int, default=3)
    ap.add_argument("--embed-url", default=os.environ.get("HU_SEMANTIC_EMBED_URL", "http://127.0.0.1:8741"))
    ap.add_argument("--out", default="docs/plans/2026-08-02-semantic-retrieval/memory-benchmarks-c2-ablation.json")
    a = ap.parse_args()

    try:
        urllib.request.urlopen(a.embed_url + "/health", timeout=10)
    except Exception as e:
        sys.exit(f"REFUSING: embedder down at {a.embed_url} ({e}); nothing written")
    # emb.build_db()'s own reindex call reads HU_SEMANTIC_EMBED_URL from
    # os.environ directly (not a parameter) -- set it here so the expensive
    # per-question reindex step uses the SAME endpoint as everything else in
    # this script, instead of silently falling back to its 8749 default.
    os.environ["HU_SEMANTIC_EMBED_URL"] = a.embed_url

    path = os.path.join(DATA, "longmemeval_s.json")
    if not os.path.exists(path):
        sys.exit(f"REFUSING: {path} missing")
    data = json.load(open(path))
    rng = random.Random(a.seed)
    rng.shuffle(data)
    by_type = collections.defaultdict(list)
    for q in data:
        by_type[q["question_type"]].append(q)
    per = max(1, a.limit // len(by_type))
    qs = [q for t in sorted(by_type) for q in by_type[t][:per]]

    tmp = "/tmp/hu_membench_ablation"
    os.makedirs(tmp, exist_ok=True)
    dbp = os.path.join(tmp, "lme.db")

    results = {v: [] for v in VARIANTS}
    baseline = []  # kw, sem, harness-rrf-hybrid per question (measured once)
    skipped = []

    for n, q in enumerate(qs, 1):
        rows = []
        for sid, sess in zip(q["haystack_session_ids"], q["haystack_sessions"]):
            for k, turn in enumerate(sess):
                rows.append((f"s{sid}:t{k}", str(sid), f"{turn['role']}: {turn['content']}"))
        print(f"  [{n}/{len(qs)}] building {q['question_id']} ({q['question_type']}, {len(rows)} rows)", flush=True)
        idx = emb.build_db(a.bin, dbp, rows)
        if idx < len(rows) * 0.9:
            time.sleep(10)
            idx = emb.build_db(a.bin, dbp, rows)
            if idx < len(rows) * 0.9:
                skipped.append({"question_id": q["question_id"], "type": q["question_type"],
                                 "indexed": idx, "rows": len(rows)})
                print(f"  [{n}/{len(qs)}] SKIPPED {q['question_id']}: indexed {idx}/{len(rows)}", flush=True)
                continue

        env = {"HU_SEMANTIC_EMBED_URL": a.embed_url}
        kw = emb.parse_keys(emb.sh(a.bin, dbp, ["search", q["question"]]))
        sem = emb.parse_keys(emb.sh(a.bin, dbp, ["search", "--semantic", q["question"]], env))
        ans = set(str(s) for s in q["answer_session_ids"])

        def sess_r5(keys):
            valid = [k for k in keys if VALID_KEY_RE.match(k)]
            seen = []
            for key in valid:
                sid = key.split(":")[0][1:]
                if sid not in seen:
                    seen.append(sid)
                if len(seen) == 5:
                    break
            all_fallback = len(keys) > 0 and len(valid) == 0
            return int(bool(ans & set(seen))), all_fallback

        kw_r5, kw_fb = sess_r5(kw)
        sem_r5, sem_fb = sess_r5(sem)
        hyb_r5, hyb_fb = sess_r5(emb.rrf(kw, sem))
        baseline.append({"type": q["question_type"], "kw": kw_r5, "sem": sem_r5, "hybrid": hyb_r5})

        row_line = f"  [{n}/{len(qs)}] {q['question_type'][:22]:22} kw={kw_r5} sem={sem_r5} hyb={hyb_r5}"
        for v in VARIANTS:
            keys = run_variant(a.bin, dbp, q["question"], v, a.embed_url)
            r5, all_fallback = sess_r5(keys)
            results[v].append({"type": q["question_type"], "r5": r5, "all_fallback": all_fallback})
            row_line += f" {v}={r5}{'!fb' if all_fallback else ''}"
        print(row_line, flush=True)

    n_scored = len(baseline)
    if n_scored < a.min_q:
        sys.exit(f"REFUSING: {n_scored} questions scored < {a.min_q}")
    if len(skipped) > max(2, len(qs) // 10):
        sys.exit(f"REFUSING: {len(skipped)} questions skipped for embedder/index failures: {skipped}")

    def summarize_baseline(rows):
        out = {"n": len(rows)}
        for m in ("kw", "sem", "hybrid"):
            out[m] = round(sum(r[m] for r in rows) / len(rows), 3)
        by = collections.defaultdict(list)
        for r in rows:
            by[r["type"]].append(r)
        out["by_type"] = {t: {m: round(sum(r[m] for r in v) / len(v), 3) for m in ("kw", "sem", "hybrid")} | {"n": len(v)}
                          for t, v in sorted(by.items())}
        return out

    def summarize_variant(rows):
        out = {"n": len(rows), "overall": round(sum(r["r5"] for r in rows) / len(rows), 3),
               "fallback_rate": round(sum(1 for r in rows if r["all_fallback"]) / len(rows), 3)}
        by = collections.defaultdict(list)
        for r in rows:
            by[r["type"]].append(r)
        out["by_type"] = {t: {"r5": round(sum(x["r5"] for x in v) / len(v), 3),
                              "fallback_rate": round(sum(1 for x in v if x["all_fallback"]) / len(v), 3),
                              "n": len(v)}
                          for t, v in sorted(by.items())}
        return out

    out_json = {
        "date": time.strftime("%Y-%m-%d"),
        "binary": a.bin,
        "embed_url": a.embed_url,
        "seed": a.seed,
        "limit": a.limit,
        "protocol": "session-level R@5 from top-10 turns (same as eval_memory_benchmarks.py longmemeval_s)",
        "note": "companion ablation study for Contract C2's hu_hybrid_retrieve reconstructive path "
                "(src/memory/retrieval/hybrid.c); each variant sets HU_RECON_ABLATE and re-runs "
                "`human memory search --hybrid` against the SAME indexed db as the baseline row "
                "-- no redesign, default behavior (opts.reconstructive stays plumbed the same, "
                "HU_RECON_ABLATE unset in production) is unaffected by this script.",
        "baseline": summarize_baseline(baseline),
        "variants": {v: summarize_variant(results[v]) for v in VARIANTS},
        "skipped_embedder_or_index": skipped,
    }
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    json.dump(out_json, open(a.out, "w"), indent=2)
    print(json.dumps({k: v for k, v in out_json.items() if k not in ("protocol", "note")}, indent=1))
    print("wrote", a.out)


if __name__ == "__main__":
    sys.exit(main())
