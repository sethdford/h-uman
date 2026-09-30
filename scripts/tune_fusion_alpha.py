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
A question on which ANY embedding-dependent call failed (non-zero exit, a
`search --hybrid:`/`--semantic:` error, or "semantic index unavailable", where
--hybrid silently degrades to keyword-only) is dropped from EVERY arm and counted
as `questions_excluded_embed_error`; above 10% the sweep exits 2 without selecting
an alpha. `loco.paired_vs_rrf` gives per-question win/tie/loss of the held-out
score arm against rrf on the same questions.

The controller runs this against the live embedding server; tests cover the pure
parts (scripts/test_tune_fusion_alpha.py).
"""
import argparse, collections, json, math, os, subprocess, sys, time, urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eval_memory_benchmarks as emb  # noqa: E402

ALPHAS = tuple(round(i / 10, 1) for i in range(11))
METRIC_NAMES = ("hit@1", "r@5", "r@10", "ndcg@5")
OBJECTIVE = ("hit@1", "ndcg@5", "r@5", "r@10")
REFERENCE_ARMS = ("kw", "sem", "rrf")
MAX_EMPTY_SEM_FRAC = 0.10
MAX_EXCLUDED_FRAC = 0.10
# stderr of a `human memory search` call whose dense leg did not run: the
# retrieve failed (embedder error) or no semantic index could be attached, in
# which case --hybrid silently falls back to keyword-only output.
EMBED_FAILURE_MARKERS = ("search --hybrid:", "search --semantic:", "semantic index unavailable")


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


def paired_counts(pairs):
    """Per metric, how many questions the held-out score arm beat / tied / lost
    to the production rrf arm ON THE SAME QUESTION -- the paired view the
    means alone hide (a few points of Hit@1 at n~100 is inside the noise)."""
    out = {}
    for m in METRIC_NAMES:
        c = {"win": 0, "tie": 0, "loss": 0}
        for p in pairs:
            d = p["arms"]["held_out"][m] - p["arms"]["rrf"][m]
            c["win" if d > 1e-12 else "loss" if d < -1e-12 else "tie"] += 1
        out[m] = c
    return out


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
                held_out.append({"group": g, "arms": {"held_out": r["arms"][alpha_arm(a)],
                                                      "rrf": r["arms"]["rrf"]}})
    top = max(chosen.values())
    tied = [a for a, c in chosen.items() if c == top]
    return {
        "folds": len(groups),
        "n": len(held_out),
        "held_out": mean_metrics(held_out, "held_out"),
        "paired_vs_rrf": paired_counts(held_out),
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


def run_search(binp, dbp, args, env_extra=None):
    """(ranked keys, dense_leg_ran) for one `human memory search` call."""
    env = {**os.environ, "HU_MEMORY_SQLITE_PATH": dbp, **(env_extra or {})}
    p = subprocess.run([binp, "memory", *args], capture_output=True, env=env, timeout=600)
    err = p.stderr.decode("utf-8", "replace")
    ok = p.returncode == 0 and not any(m in err for m in EMBED_FAILURE_MARKERS)
    return emb.parse_keys(p.stdout.decode("utf-8", "replace")), ok


def search_arms(binp, dbp, question):
    """(ranked keys per arm, embed_failed) for one question. embed_failed is
    True when ANY embedding-dependent call (semantic or hybrid) failed: that
    question then scores in NO arm, never as zeros in one of them."""
    env = emb.embed_env()
    keys = {"kw": run_search(binp, dbp, ["search", question])[0]}
    keys["sem"], ok = run_search(binp, dbp, ["search", "--semantic", question], env)
    failed = not ok
    plain = ["search", "--hybrid", "--plain", question]
    keys["rrf"], ok = run_search(binp, dbp, plain, {**env, **emb.plain_env("rrf")})
    failed |= not ok
    for a in ALPHAS:
        keys[alpha_arm(a)], ok = run_search(binp, dbp, plain, {**env, **emb.plain_env("score", a)})
        failed |= not ok
    return keys, failed


def record(group, keys, units, gold):
    return {"group": group, "sem_empty": not keys["sem"],
            "arms": {arm: metrics(units(k), gold) for arm, k in keys.items()}}


def check_excluded(excluded, attempted, label):
    """Refuse (exit 2, nothing written) when too many questions lost an arm to
    embedder errors: the survivors are no longer the sampled benchmark."""
    if attempted and excluded > MAX_EXCLUDED_FRAC * attempted:
        print(f"REFUSING: {excluded}/{attempted} {label} questions excluded for embedder errors "
              f"(> {MAX_EXCLUDED_FRAC:.0%}); no alpha selected, nothing written", file=sys.stderr)
        sys.exit(2)


def run_longmemeval(binp, limit, seed, tmp):
    out, skipped, excluded = [], 0, 0
    qs = emb.lme_questions(limit, seed)
    for n, (q, rows) in enumerate(qs, 1):
        dbp = os.path.join(tmp, "lme.db")
        if emb.build_db_retry(binp, dbp, rows) < len(rows) * 0.9:
            skipped += 1
            print(f"  lme [{n}/{len(qs)}] skipped (index incomplete)", flush=True)
            continue
        keys, failed = search_arms(binp, dbp, q["question"])
        if failed:
            excluded += 1
            print(f"  lme [{n}/{len(qs)}] excluded (embedder error)", flush=True)
            continue
        out.append(record("lme:" + q["question_id"], keys, lme_units, {str(s) for s in q["answer_session_ids"]}))
        print(f"  lme [{n}/{len(qs)}] done", flush=True)
    if skipped > max(2, len(qs) // 10):
        sys.exit(f"REFUSING: {skipped} LongMemEval questions skipped for incomplete indexes")
    check_excluded(excluded, len(out) + excluded, "LongMemEval")
    return out, skipped, excluded


def run_locomo(binp, limit, seed, tmp):
    out, excluded = [], 0
    for ci, rows, qa in emb.locomo_convs(limit, seed):
        dbp = os.path.join(tmp, f"locomo{ci}.db")
        idx = emb.build_db(binp, dbp, rows)
        if idx < len(rows) * 0.9:
            sys.exit(f"REFUSING: conv{ci} indexed {idx}/{len(rows)}")
        for q in qa:
            keys, failed = search_arms(binp, dbp, q["question"])
            if failed:
                excluded += 1
                continue
            out.append(record(f"locomo:{ci}", keys, locomo_units, set(q["evidence"])))
        print(f"  locomo conv {ci}: {len(qa)} questions", flush=True)
    check_excluded(excluded, len(out) + excluded, "LoCoMo")
    return out, excluded


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
        r, skipped, excluded = run_longmemeval(a.bin, a.limit, a.seed, tmp)
        if len(r) < a.min_q: sys.exit(f"REFUSING: {len(r)} LongMemEval questions < {a.min_q}")
        check_semantic_alive(r, "LongMemEval")
        out["longmemeval_s"] = summarize(r) | {"skipped_incomplete_index": skipped,
                                               "questions_excluded_embed_error": excluded}
        records += r
    if a.bench in ("locomo", "both"):
        r, excluded = run_locomo(a.bin, a.limit, a.seed, tmp)
        if len(r) < a.min_q: sys.exit(f"REFUSING: {len(r)} LoCoMo questions < {a.min_q}")
        check_semantic_alive(r, "LoCoMo")
        out["locomo10"] = summarize(r) | {"questions_excluded_embed_error": excluded}
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
