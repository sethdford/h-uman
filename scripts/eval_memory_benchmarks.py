#!/usr/bin/env python3
"""Measurement #2 — LongMemEval-S and LoCoMo-10 through h-uman's OWN retrieval.

No LLM in the loop (the MemPalace-style number): for each question the haystack is loaded
into a fresh memory.db, the shipped binary indexes it (FTS5 + sqlite-vec via the
embeddings endpoint), and `human memory search` / `--semantic` / `--hybrid` rank rows.

Two distinct "hybrid" columns are measured, on purpose:
  hybrid      : reciprocal-rank fusion of the kw + sem lists computed HERE in Python
                (the harness-side baseline — no scene-select/rerank/temporal-filter).
  hybrid_cli  : `human memory search --hybrid <q>` — Contract C2's reconstructive
                path (scene-select -> neighbour expansion -> rerank -> time-bounded
                filter -> sufficiency check), measured through the actual C binary.
  hybrid_plain: `human memory search --hybrid --plain <q>` — the daemon memory loader's
                call. `--fusion score --alpha A` runs it under HU_HYBRID_FUSION=score
                (score-level fusion, dense weight A); the default pins
                HU_HYBRID_FUSION=rrf so an inherited env var cannot move the column.
                scripts/tune_fusion_alpha.py sweeps A with leave-one-conversation-out
                selection over the same sampled questions (lme_questions/locomo_convs).

  LongMemEval-S : session-level R@5 — answer_session_ids vs the first 5 distinct sessions
                  among the top-10 retrieved turns (the benchmark's own recall protocol).
  LoCoMo-10     : turn-level R@10 — evidence dia_ids vs top-10 retrieved turns.

Refuses (exit 2, nothing written) when a dataset file is missing, the embedder is down,
or fewer than --min-q questions were scored.
"""
import argparse, collections, json, os, random, re, shutil, sqlite3, subprocess, sys, time, urllib.request, uuid

DATA = os.path.expanduser("~/.human/eval-data")

def sh(binp, dbp, args, env_extra=None):
    env = {**os.environ, "HU_MEMORY_SQLITE_PATH": dbp, **(env_extra or {})}
    return subprocess.run([binp, "memory", *args], capture_output=True, env=env, timeout=600).stdout.decode("utf-8", "replace")

def parse_keys(out):
    # Key ends at ": " or " (" — NOT at the first ":" — LoCoMo keys are "D1:3",
    # and the old non-greedy match truncated them to "D1" (R@10 = 0.0 on every arm).
    return [m.group(1) for m in re.finditer(r"^\s*\[\d+\]\s+(\S+?)(?:: | \()", out, re.M)]

def rrf(*lists, k=60):
    sc = collections.defaultdict(float)
    for L in lists:
        for r, key in enumerate(L, 1):
            sc[key] += 1.0 / (k + r)
    return [key for key, _ in sorted(sc.items(), key=lambda kv: -kv[1])]

def plain_env(fusion="rrf", alpha=None):
    """Env for the hybrid_plain column: the fusion gate, always explicit."""
    env = {"HU_HYBRID_FUSION": fusion}
    if fusion == "score":
        if alpha is None or not (0.0 <= alpha <= 1.0):
            raise ValueError(f"--alpha must be in [0,1] for --fusion score, got {alpha!r}")
        env["HU_HYBRID_FUSION_ALPHA"] = f"{alpha:.2f}"
    return env

def embed_env():
    return {"HU_SEMANTIC_EMBED_URL": os.environ.get("HU_SEMANTIC_EMBED_URL", "http://127.0.0.1:8749")}

def build_db(binp, dbp, rows):
    """rows: list of (key, session_id, content)."""
    for s in ("", "-wal", "-shm"):
        try: os.remove(dbp + s)
        except FileNotFoundError: pass
    sh(binp, dbp, ["count"])  # engine creates the schema
    con = sqlite3.connect(dbp)
    now = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    con.executemany("INSERT OR IGNORE INTO memories(id,key,content,category,session_id,source,created_at,updated_at,trust_tier,provenance) "
                    "VALUES(?,?,?,'core',?,?,?,?,1,?)",
                    [(str(uuid.uuid4()), k, c, sid, "bench", now, now, "bench") for k, sid, c in rows])
    con.execute("INSERT INTO memories_fts(memories_fts) VALUES('rebuild')")
    con.commit(); con.close()
    # HU_SEMANTIC_EMBED_URL_INDEX lets an asymmetric embedder (EmbeddingGemma's
    # document prefix) index with one server while queries use HU_SEMANTIC_EMBED_URL.
    out = sh(binp, dbp, ["reindex"], {"HU_SEMANTIC_EMBED_URL": os.environ.get("HU_SEMANTIC_EMBED_URL_INDEX")
                                     or os.environ.get("HU_SEMANTIC_EMBED_URL", "http://127.0.0.1:8749")})
    m = re.search(r'"index_size": (\d+)', out)
    return int(m.group(1)) if m else 0

def lme_questions(limit, seed, data_dir=None):
    """The sampled LongMemEval-S questions in run order, as (question, rows)."""
    path = os.path.join(data_dir or DATA, "longmemeval_s.json")
    if not os.path.exists(path): sys.exit(f"REFUSING: {path} missing")
    data = json.load(open(path))
    rng = random.Random(seed); rng.shuffle(data)
    by_type = collections.defaultdict(list)
    for q in data: by_type[q["question_type"]].append(q)
    per = max(1, limit // len(by_type)); qs = [q for t in sorted(by_type) for q in by_type[t][:per]]
    out = []
    for q in qs:
        rows = []
        for sid, sess in zip(q["haystack_session_ids"], q["haystack_sessions"]):
            for k, turn in enumerate(sess):
                rows.append((f"s{sid}:t{k}", str(sid), f"{turn['role']}: {turn['content']}"))
        out.append((q, rows))
    return out

def build_db_retry(binp, dbp, rows):
    """build_db, retried once after 10 s when under 90% of rows got indexed.
    The 8-bit embedder has died on specific rows (deterministic); the pause gives
    its supervisor time to restart it. Returns the final indexed count."""
    idx = build_db(binp, dbp, rows)
    if idx < len(rows) * 0.9:
        time.sleep(10)
        idx = build_db(binp, dbp, rows)
    return idx

def longmemeval(binp, limit, seed, tmp, hybrid_plain_env=None):
    qs = lme_questions(limit, seed)
    res = []; skipped = []
    for n, (q, rows) in enumerate(qs, 1):
        dbp = os.path.join(tmp, "lme.db")
        print(f"  [{n}/{len(qs)}] building {q['question_id']} ({len(rows)} rows)", flush=True)
        idx = build_db_retry(binp, dbp, rows)
        if idx < len(rows) * 0.9:
            # SKIP and COUNT the question rather than abort the run; too many skips
            # refuses at the end.
            skipped.append({"question_id": q["question_id"], "type": q["question_type"], "indexed": idx, "rows": len(rows)})
            print(f"  [{n}/{len(qs)}] SKIPPED {q['question_id']}: indexed {idx}/{len(rows)}", flush=True)
            continue
        env = embed_env()
        kw = parse_keys(sh(binp, dbp, ["search", q["question"]]))
        sem = parse_keys(sh(binp, dbp, ["search", "--semantic", q["question"]], env))
        hyb_cli = parse_keys(sh(binp, dbp, ["search", "--hybrid", q["question"]], env))
        hyb_plain = parse_keys(sh(binp, dbp, ["search", "--hybrid", "--plain", q["question"]], {**env, **(hybrid_plain_env or plain_env())}))
        ans = set(str(s) for s in q["answer_session_ids"])
        def sess_r5(keys):
            seen = []
            for key in keys:
                sid = key.split(":")[0][1:]
                if sid not in seen: seen.append(sid)
                if len(seen) == 5: break
            return int(bool(ans & set(seen)))
        r = {"type": q["question_type"], "kw": sess_r5(kw), "sem": sess_r5(sem), "hybrid": sess_r5(rrf(kw, sem)),
             "hybrid_cli": sess_r5(hyb_cli), "hybrid_plain": sess_r5(hyb_plain), "rows": len(rows)}
        res.append(r); print(f"  [{n}/{len(qs)}] {q['question_type'][:22]:22} kw={r['kw']} sem={r['sem']} hyb={r['hybrid']} hyb_cli={r['hybrid_cli']} hyb_plain={r['hybrid_plain']} rows={len(rows)}", flush=True)
    if len(skipped) > max(2, len(qs) // 10): sys.exit(f"REFUSING: {len(skipped)} questions skipped for embedder crashes: {skipped}")
    longmemeval.skipped = skipped
    return res

def locomo_convs(limit, seed, data_dir=None):
    """The LoCoMo-10 conversations in run order, as (index, rows, sampled_qa). One
    rng is shared across conversations, so the sample depends on this order."""
    path = os.path.join(data_dir or DATA, "locomo10.json")
    if not os.path.exists(path): sys.exit(f"REFUSING: {path} missing")
    data = json.load(open(path)); rng = random.Random(seed); out = []
    for ci, conv in enumerate(data):
        rows = []
        c = conv["conversation"]
        for skey in sorted(k for k in c if re.fullmatch(r"session_\d+", k)):
            for turn in c[skey]:
                rows.append((turn["dia_id"], skey, f"{turn['speaker']}: {turn.get('text','')}"))
        qa = [q for q in conv["qa"] if q.get("evidence")]
        rng.shuffle(qa); qa = qa[:max(1, limit // len(data))]
        out.append((ci, rows, qa))
    return out

def locomo(binp, limit, seed, tmp, hybrid_plain_env=None):
    res = []
    for ci, rows, qa in locomo_convs(limit, seed):
        dbp = os.path.join(tmp, f"locomo{ci}.db")
        idx = build_db(binp, dbp, rows)
        if idx < len(rows) * 0.9: sys.exit(f"REFUSING: conv{ci} indexed {idx}/{len(rows)}")
        for q in qa:
            env = embed_env()
            kw = parse_keys(sh(binp, dbp, ["search", q["question"]]))[:10]
            sem = parse_keys(sh(binp, dbp, ["search", "--semantic", q["question"]], env))[:10]
            hyb_cli = parse_keys(sh(binp, dbp, ["search", "--hybrid", q["question"]], env))[:10]
            hyb_plain = parse_keys(sh(binp, dbp, ["search", "--hybrid", "--plain", q["question"]], {**env, **(hybrid_plain_env or plain_env())}))[:10]
            ev = set(q["evidence"])
            r = {"category": q.get("category"), "kw": int(bool(ev & set(kw))), "sem": int(bool(ev & set(sem))),
                 "hybrid": int(bool(ev & set(rrf(kw, sem)[:10]))), "hybrid_cli": int(bool(ev & set(hyb_cli))),
                 "hybrid_plain": int(bool(ev & set(hyb_plain)))}
            res.append(r)
        print(f"  conv {ci}: {len(rows)} turns, {len(qa)} questions, running kw={sum(r['kw'] for r in res)}/{len(res)}", flush=True)
    return res

METRICS = ("kw", "sem", "hybrid", "hybrid_cli", "hybrid_plain")

def summarize(res, key_field):
    out = {"n": len(res)}
    for m in METRICS:
        out[m] = round(sum(r[m] for r in res) / len(res), 3)
    by = collections.defaultdict(list)
    for r in res: by[str(r[key_field])].append(r)
    out["by_" + key_field] = {k: {m: round(sum(r[m] for r in v) / len(v), 3) for m in METRICS} | {"n": len(v)} for k, v in sorted(by.items())}
    return out

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--bin", default="./build/human")
    ap.add_argument("--bench", choices=["longmemeval", "locomo", "both"], default="both")
    ap.add_argument("--limit", type=int, default=60)
    ap.add_argument("--min-q", type=int, default=30)
    ap.add_argument("--seed", type=int, default=3)
    ap.add_argument("--out", default="docs/plans/2026-08-02-semantic-retrieval/memory-benchmarks-%s.json" % time.strftime("%Y-%m-%d"))
    ap.add_argument("--fusion", choices=["rrf", "score"], default="rrf",
                    help="HU_HYBRID_FUSION for the hybrid_plain column (default rrf, production)")
    ap.add_argument("--alpha", type=float, default=None,
                    help="HU_HYBRID_FUSION_ALPHA (dense weight in [0,1]); required with --fusion score")
    a = ap.parse_args()
    try: hp_env = plain_env(a.fusion, a.alpha)
    except ValueError as e: sys.exit(f"REFUSING: {e}")
    try: urllib.request.urlopen(os.environ.get("HU_SEMANTIC_EMBED_URL", "http://127.0.0.1:8749") + "/health", timeout=5)
    except Exception as e: sys.exit(f"REFUSING: embedder down ({e}); nothing written")
    tmp = "/tmp/hu_membench"; os.makedirs(tmp, exist_ok=True)
    out = {"date": time.strftime("%Y-%m-%d"), "binary": a.bin, "protocol": {
        "longmemeval": "session-level R@5 from top-10 turns", "locomo": "turn-level R@10",
        "hybrid": "harness-side RRF(k=60) of kw+sem lists (Python, no C hybrid path)",
        "hybrid_cli": "human memory search --hybrid <q> -- Contract C2 reconstructive retrieval "
                      "(scene-select -> neighbour expansion -> rerank -> time-bounded filter -> "
                      "sufficiency check) measured through the C binary -- CLI-only mode, no daemon path sets reconstructive",
        "hybrid_plain": "human memory search --hybrid --plain <q> -- the daemon memory loader's call "
                        "(reconstructive=false: RRF merge -> cut to limit; the term-overlap rerank runs only "
                        "with use_reranking, which the loader leaves off); this is production",
        "hybrid_plain_fusion": hp_env}}
    if a.bench in ("longmemeval", "both"):
        r = longmemeval(a.bin, a.limit, a.seed, tmp, hp_env)
        if len(r) < a.min_q: sys.exit(f"REFUSING: {len(r)} LongMemEval questions < {a.min_q}")
        out["longmemeval_s"] = summarize(r, "type"); out["longmemeval_skipped_embedder_crash"] = getattr(longmemeval, "skipped", [])
    if a.bench in ("locomo", "both"):
        r = locomo(a.bin, a.limit, a.seed, tmp, hp_env)
        if len(r) < a.min_q: sys.exit(f"REFUSING: {len(r)} LoCoMo questions < {a.min_q}")
        out["locomo10"] = summarize(r, "category")
    json.dump(out, open(a.out, "w"), indent=2); print(json.dumps({k: v for k, v in out.items() if k not in ("protocol",)}, indent=1)); print("wrote", a.out)

if __name__ == "__main__":
    sys.exit(main())
