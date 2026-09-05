#!/usr/bin/env python3
"""Task 12 — drift monitor. Twelve fixed probe prompts through the PRODUCTION
head (dump_prompt_head, HU_PERSONA_HEAD=live) to :8741, replies embedded via
/v1/embeddings; the first run writes the baseline, later runs report per-prompt
cosine to the baseline reply and a mean. Refuses (no file) on any empty or
`[timeout]` reply — an unmeasured night must not look like a stable one.

    drift_monitor.py [--embed-url URL] [--chat-url URL] [--out-dir ~/.human/drift]
"""
import argparse, json, os, sys, time, urllib.request, urllib.error
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

PROBES = [
    ("casual", "hey what are you up to tonight"),
    ("casual", "lol did you see that game"),
    ("casual", "you around this weekend?"),
    ("substantive", "what do you think about moving to florida long term"),
    ("substantive", "how's the new place treating you"),
    ("substantive", "any advice on negotiating with a contractor"),
    ("event", "did the house sell yet"),
    ("event", "when's your last day at work"),
    ("event", "how did the move go"),
    ("opinion", "is it worth paying for a personal trainer"),
    ("opinion", "thoughts on the new iphone"),
    ("opinion", "would you ever go back to philly"),
]

def post(url, body, headers, timeout):
    req = urllib.request.Request(url, data=json.dumps(body).encode(),
                                 headers={"Content-Type": "application/json", **headers})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.load(r)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--chat-url", default="http://127.0.0.1:8741/v1/chat/completions")
    ap.add_argument("--embed-url", default=os.environ.get("HU_SEMANTIC_EMBED_URL", "http://127.0.0.1:8741") + "/v1/embeddings")
    ap.add_argument("--out-dir", default=os.path.expanduser("~/.human/drift"))
    ap.add_argument("--threshold", type=float, default=0.85)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--temperature", type=float, default=0.0,
                    help="0 = greedy: a same-day repeat at T=0.7 scored mean_cos 0.725, i.e. sampling noise "
                         "alone would trip the 0.85 alarm; drift must be measured against a deterministic reply")
    a = ap.parse_args()
    try:
        if os.environ.get("HU_DRIFT_FAKE_HEAD") == "1":
            head = "TEST HEAD"  # unit tests only; production always dumps the real head
        else:
            from eval_blinded_ab import production_system_prompt
            head = production_system_prompt()
    except SystemExit as e:
        sys.exit(f"FATAL: production head unavailable ({e}); refusing to measure drift against a made-up prompt")
    replies = []
    for kind, q in PROBES:
        try:
            d = post(a.chat_url, {"model": "GLM-4.5-Air-4bit", "max_tokens": 120, "temperature": a.temperature,
                                  "messages": [{"role": "system", "content": head}, {"role": "user", "content": q}]},
                     {"X-HU-Priority": "batch"}, a.timeout)
            text = (d["choices"][0]["message"]["content"] or "").strip()
        except Exception as e:  # noqa: BLE001
            sys.exit(f"FATAL: generation failed for probe '{q}': {type(e).__name__}: {e} — no drift file written")
        if not text or text.startswith("[timeout]"):
            sys.exit(f"FATAL: empty/timeout reply for probe '{q}' — no drift file written")
        replies.append({"kind": kind, "prompt": q, "reply": text})
    try:
        emb = post(a.embed_url, {"input": [r["reply"] for r in replies]}, {"X-HU-Priority": "batch"}, a.timeout)
        vecs = [x["embedding"] for x in emb["data"]]
    except Exception as e:  # noqa: BLE001
        sys.exit(f"FATAL: embedding failed: {type(e).__name__}: {e} — no drift file written")
    if len(vecs) != len(replies):
        sys.exit("FATAL: embedding count mismatch — no drift file written")
    os.makedirs(a.out_dir, exist_ok=True)
    base_path = os.path.join(a.out_dir, "baseline.json")
    stamp = time.strftime("%Y-%m-%d")
    doc = {"date": stamp, "head_bytes": len(head), "temperature": a.temperature, "probes": [{**r, "vec": v} for r, v in zip(replies, vecs)]}
    if not os.path.exists(base_path):
        json.dump(doc, open(base_path, "w"))
        print(f"[drift] baseline written: {base_path} ({len(replies)} probes)")
        return 0
    base = json.load(open(base_path))
    dot = lambda x, y: sum(p * q for p, q in zip(x, y))
    rows = []
    for b, cur in zip(base["probes"], doc["probes"]):
        rows.append({"kind": b["kind"], "prompt": b["prompt"], "cos": round(dot(b["vec"], cur["vec"]), 4),
                     "baseline_reply": b["reply"][:120], "reply": cur["reply"][:120]})
    mean = sum(r["cos"] for r in rows) / len(rows)
    out = {"date": stamp, "baseline_date": base["date"], "mean_cos": round(mean, 4), "threshold": a.threshold,
           "verdict": "DRIFT" if mean < a.threshold else "STABLE", "rows": rows}
    path = os.path.join(a.out_dir, f"drift-{stamp}.json")
    json.dump(out, open(path, "w"), indent=2)
    print(f"[drift] {out['verdict']} mean_cos={mean:.3f} (threshold {a.threshold}) -> {path}")
    if out["verdict"] == "DRIFT":
        print(f"DRIFT mean_cos={mean:.3f} < {a.threshold}", file=sys.stderr)
        return 1
    return 0

if __name__ == "__main__":
    sys.exit(main())
