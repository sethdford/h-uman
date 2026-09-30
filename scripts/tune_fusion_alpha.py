#!/usr/bin/env python3
"""Sweep HU_HYBRID_FUSION_ALPHA for the daemon's plain hybrid call and pick alpha
leave-one-conversation-out (arXiv 2606.04194's protocol).

Same sampled questions, same fresh memory.db per haystack and same shipped binary
as scripts/eval_memory_benchmarks.py (it reuses that script's sampler and index
builder). For every question it runs `human memory search --hybrid --plain` once per
alpha in {0.0, 0.1, ..., 1.0} under HU_HYBRID_FUSION=score, plus three reference
arms: keyword only, semantic only, and the production RRF merge
(HU_HYBRID_FUSION=rrf).

Metrics per arm, over the ranked retrieval UNITS:
  LongMemEval-S : units = distinct sessions, in order, among the returned turns
                  (the benchmark's session-level protocol; top-10 turns -> <=10 sessions)
  LoCoMo-10     : units = distinct turn keys (dia_ids) among the top-10 returned turns
  hit@1   first unit is relevant
  r@5     any relevant unit in the first 5 (the "R@k" any-hit protocol of
          eval_memory_benchmarks.py, so the rrf arm is comparable to earlier JSONs)
  r@10    any relevant unit in the first 10
  ndcg@5  binary-relevance NDCG over the first 5 units

Leave-one-conversation-out: a "conversation" is a LoCoMo conversation (10 groups) or
a LongMemEval question's own haystack (one group per question). For each held-out
group the alpha is chosen on the OTHER groups by mean (hit@1, ndcg@5, r@5, r@10),
ties toward 0.5 and then the smaller alpha, and scored on the held-out group only.
`loco_selected_alpha` is the alpha most folds chose. Also reported, labelled as
optimistic: the in-sample best alpha on all groups.

Writes counts and metric means only -- no question or memory text.
Refuses (non-zero exit, nothing written) when a dataset file is missing, the embedder is
down, fewer than --min-q questions were scored, or the semantic leg came back empty
on more than 10% of questions (a dying embedder must not look like alpha=0 winning).

The controller runs this against the live embedding server; tests cover the pure
parts (scripts/test_tune_fusion_alpha.py).
"""
import argparse, collections, json, math, os, sys, time, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eval_memory_benchmarks as emb  # noqa: E402

ALPHAS = tuple(round(i / 10, 1) for i in range(11))
METRIC_NAMES = ("hit@1", "r@5", "r@10", "ndcg@5")
OBJECTIVE = ("hit@1", "ndcg@5", "r@5", "r@10")
REFERENCE_ARMS = ("kw", "sem", "rrf")
MAX_EMPTY_SEM_FRAC = 0.10


def alpha_arm(a):
    return f"alpha={a:.1f}"


def dedupe(seq):
    seen, out = set(), []
    for x in seq:
        if x not in seen:
            seen.add(x); out.append(x)
    return out


def lme_units(keys):
    """Turn keys "s<sid>:t<k>" -> distinct session ids in rank order."""
    return dedupe(k.split(":")[0][1:] for k in keys)


def locomo_units(keys):
    return dedupe(keys[:10])


def metrics(ranked, gold):
    """hit@1 / r@5 / r@10 / ndcg@5 of a ranked unit list against a non-empty gold set."""
    gold = set(gold)
    if not gold:
        raise ValueError("empty gold set: nothing to measure")
    rel = [1 if u in gold else 0 for u in ranked]
    dcg = sum(r / math.log2(i + 2) for i, r in enumerate(rel[:5]))
    idcg = sum(1 / math.log2(i + 2) for i in range(min(len(gold), 5)))
    return {"hit@1": float(bool(rel[:1] and rel[0])), "r@5": float(any(rel[:5])),
            "r@10": float(any(rel[:10])), "ndcg@5": dcg / idcg}


def mean_metrics(records, arm):
    n = len(records)
    if n == 0:
        raise ValueError("no records: a mean over nothing is not a measurement")
    return {m: round(sum(r["arms"][arm][m] for r in records) / n, 4) for m in METRIC_NAMES}


def select_alpha(records, candidates=ALPHAS):
    """Alpha maximising the mean OBJECTIVE tuple; ties toward 0.5, then smaller."""
    def key(a):
        mm = mean_metrics(records, alpha_arm(a))
        return tuple(mm[m] for m in OBJECTIVE) + (-abs(a - 0.5), -a)
    return max(candidates, key=key)


def loco(records):
    """Leave-one-group-out alpha selection; each record needs 'group' and 'arms'."""
    groups = sorted({r["group"] for r in records})
    if len(groups) < 2:
        raise ValueError(f"leave-one-conversation-out needs >= 2 groups, got {len(groups)}")
    chosen, held_out = collections.Counter(), []
    for g in groups:
        train = [r for r in records if r["group"] != g]
        a = select_alpha(train)
        chosen[a] += 1
        for r in records:
            if r["group"] == g:
                held_out.append({"group": g, "arms": {"held_out": r["arms"][alpha_arm(a)]}})
    top = max(chosen.values())
    tied = [a for a, c in chosen.items() if c == top]
    return {
        "folds": len(groups),
        "n": len(held_out),
        "held_out": mean_metrics(held_out, "held_out"),
        "selected_alpha_counts": {f"{a:.1f}": chosen[a] for a in sorted(chosen)},
        "loco_selected_alpha": select_alpha(records, tied) if len(tied) > 1 else tied[0],
        "in_sample_best_alpha_optimistic": select_alpha(records),
        "rrf_same_questions": mean_metrics(records, "rrf"),
    }


def summarize(records):
    arms = list(REFERENCE_ARMS) + [alpha_arm(a) for a in ALPHAS]
    return {"n": len(records), "groups": len({r["group"] for r in records}),
            "arms": {arm: mean_metrics(records, arm) for arm in arms},
            "loco": loco(records)}


def search_arms(binp, dbp, question):
    """Ranked keys per arm for one question."""
    env = emb.embed_env()
    keys = {"kw": emb.parse_keys(emb.sh(binp, dbp, ["search", question])),
            "sem": emb.parse_keys(emb.sh(binp, dbp, ["search", "--semantic", question], env))}
    plain = ["search", "--hybrid", "--plain", question]
    keys["rrf"] = emb.parse_keys(emb.sh(binp, dbp, plain, {**env, **emb.plain_env("rrf")}))
    for a in ALPHAS:
        keys[alpha_arm(a)] = emb.parse_keys(emb.sh(binp, dbp, plain, {**env, **emb.plain_env("score", a)}))
    return keys


def record(group, keys, units, gold):
    return {"group": group, "sem_empty": not keys["sem"],
            "arms": {arm: metrics(units(k), gold) for arm, k in keys.items()}}


def run_longmemeval(binp, limit, seed, tmp):
    out, skipped = [], 0
    qs = emb.lme_questions(limit, seed)
    for n, (q, rows) in enumerate(qs, 1):
        dbp = os.path.join(tmp, "lme.db")
        if emb.build_db_retry(binp, dbp, rows) < len(rows) * 0.9:
            skipped += 1
            print(f"  lme [{n}/{len(qs)}] skipped (index incomplete)", flush=True)
            continue
        keys = search_arms(binp, dbp, q["question"])
        out.append(record("lme:" + q["question_id"], keys, lme_units, {str(s) for s in q["answer_session_ids"]}))
        print(f"  lme [{n}/{len(qs)}] done", flush=True)
    if skipped > max(2, len(qs) // 10):
        sys.exit(f"REFUSING: {skipped} LongMemEval questions skipped for incomplete indexes")
    return out, skipped


def run_locomo(binp, limit, seed, tmp):
    out = []
    for ci, rows, qa in emb.locomo_convs(limit, seed):
        dbp = os.path.join(tmp, f"locomo{ci}.db")
        idx = emb.build_db(binp, dbp, rows)
        if idx < len(rows) * 0.9:
            sys.exit(f"REFUSING: conv{ci} indexed {idx}/{len(rows)}")
        for q in qa:
            keys = search_arms(binp, dbp, q["question"])
            out.append(record(f"locomo:{ci}", keys, locomo_units, set(q["evidence"])))
        print(f"  locomo conv {ci}: {len(qa)} questions", flush=True)
    return out


def check_semantic_alive(records, label):
    empty = sum(r["sem_empty"] for r in records)
    if records and empty > MAX_EMPTY_SEM_FRAC * len(records):
        sys.exit(f"REFUSING: semantic leg empty on {empty}/{len(records)} {label} questions; nothing written")


def default_out(tag):
    stem = "memory-benchmarks-fusion-alpha-sweep" + (f"-{tag}" if tag else "")
    return f"docs/plans/2026-08-02-semantic-retrieval/{stem}-{time.strftime('%Y-%m-%d')}.json"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--bin", default="./build/human")
    ap.add_argument("--bench", choices=["longmemeval", "locomo", "both"], default="both")
    ap.add_argument("--limit", type=int, default=60)
    ap.add_argument("--min-q", type=int, default=30)
    ap.add_argument("--seed", type=int, default=3)
    ap.add_argument("--tag", default="", help="embedder tag for the output name, e.g. gemma")
    ap.add_argument("--out", default=None)
    a = ap.parse_args(argv)
    url = os.environ.get("HU_SEMANTIC_EMBED_URL", "http://127.0.0.1:8749")
    try: urllib.request.urlopen(url + "/health", timeout=5)
    except Exception as e: sys.exit(f"REFUSING: embedder down ({e}); nothing written")
    tmp = "/tmp/hu_membench_fusion"; os.makedirs(tmp, exist_ok=True)
    out = {"date": time.strftime("%Y-%m-%d"), "binary": a.bin, "seed": a.seed, "limit": a.limit,
           "alphas": list(ALPHAS), "protocol": {
               "arms": "kw = `memory search`; sem = `--semantic`; rrf = `--hybrid --plain` with "
                       "HU_HYBRID_FUSION=rrf (production); alpha=A = the same call with "
                       "HU_HYBRID_FUSION=score HU_HYBRID_FUSION_ALPHA=A (A = dense weight)",
               "units": "LongMemEval-S: distinct sessions among returned turns; LoCoMo-10: distinct "
                        "dia_ids among the top-10 turns",
               "metrics": "hit@1; r@5/r@10 = any relevant unit in top k (eval_memory_benchmarks "
                          "protocol); ndcg@5 binary relevance",
               "loco": "alpha chosen on all other conversations by mean (hit@1, ndcg@5, r@5, r@10), "
                       "ties -> nearest 0.5 then smaller; scored on the held-out conversation. "
                       "LongMemEval: one conversation per question haystack"}}
    records = []
    if a.bench in ("longmemeval", "both"):
        r, skipped = run_longmemeval(a.bin, a.limit, a.seed, tmp)
        if len(r) < a.min_q: sys.exit(f"REFUSING: {len(r)} LongMemEval questions < {a.min_q}")
        check_semantic_alive(r, "LongMemEval")
        out["longmemeval_s"] = summarize(r) | {"skipped_incomplete_index": skipped}
        records += r
    if a.bench in ("locomo", "both"):
        r = run_locomo(a.bin, a.limit, a.seed, tmp)
        if len(r) < a.min_q: sys.exit(f"REFUSING: {len(r)} LoCoMo questions < {a.min_q}")
        check_semantic_alive(r, "LoCoMo")
        out["locomo10"] = summarize(r)
        records += r
    if a.bench == "both":
        out["pooled"] = {"n": len(records), "loco": loco(records)}
    path = a.out or default_out(a.tag)
    with open(path, "w") as f:
        json.dump(out, f, indent=2)
    print(json.dumps({k: v for k, v in out.items() if k != "protocol"}, indent=1))
    print("wrote", path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
