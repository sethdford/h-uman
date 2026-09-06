#!/usr/bin/env python3
"""Authorship gap — PersonalBench protocol (arXiv 2608.19746) on h-uman's own corpus.

LUAR-MUD (Rivera-Soto et al. 2021) 5v5 profile embeddings:
  ceiling : Seth 5-text profile  vs  5 OTHER held-out Seth texts
  twin    : Seth 5-text profile  vs  5 adapter replies (the trials' ai_response)
  floor   : Seth 5-text profile  vs  5-text profiles of N other real senders in chat.db
All three use the same frozen Seth profile per split; splits are re-drawn B times for a
bootstrap CI. "Twin" reported per register (casual = real reply <= 12 words, else
substantive) because the July finding was that curation shifts WHICH register is strong.

Reads chat.db read-only (other senders' texts never leave the machine; only cosines are
written). Refuses (exit 2, nothing written) when the model cannot load, when fewer than
--min-trials trials exist, or when fewer than 20 other senders have 5+ texts.
"""
import argparse, json, os, random, re, sqlite3, sys, time

def load_luar():
    import torch
    from transformers import AutoModel, AutoTokenizer
    mid = "rrivera1849/LUAR-MUD"
    tok = AutoTokenizer.from_pretrained(mid, trust_remote_code=True)
    model = AutoModel.from_pretrained(mid, trust_remote_code=True).eval()
    def profile(texts, max_len=64):
        """One LUAR embedding for a multi-document profile: [1, episodes, tokens]."""
        b = tok(texts, max_length=max_len, padding="max_length", truncation=True, return_tensors="pt")
        with torch.no_grad():
            v = model(input_ids=b["input_ids"].unsqueeze(0), attention_mask=b["attention_mask"].unsqueeze(0))
        return torch.nn.functional.normalize(v, dim=-1)[0]
    return profile

def other_senders(chatdb, min_texts=10, max_senders=60, seed=0):
    con = sqlite3.connect(f"file:{chatdb}?mode=ro", uri=True); con.text_factory = bytes
    sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__))))
    from imessage_text import decode_attributed_body
    by = {}
    for hid, txt, blob in con.execute(
            "SELECT handle_id, text, attributedBody FROM message WHERE is_from_me=0 "
            "AND COALESCE(associated_message_type,0)=0 AND handle_id IS NOT NULL ORDER BY date DESC LIMIT 60000"):
        s = None
        if txt:
            s = txt.decode("utf-8", "replace")
        elif blob:
            try: s = decode_attributed_body(blob)
            except Exception: s = None
        if not s or len(s.split()) < 3 or len(s) > 400 or "http" in s: continue
        by.setdefault(hid, []).append(s.strip())
    rng = random.Random(seed)
    senders = [v for v in by.values() if len(v) >= min_texts]
    rng.shuffle(senders)
    return senders[:max_senders]

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--trials", default=os.path.expanduser("~/blind_ab_run/classifier_trials.json"))
    ap.add_argument("--chatdb", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--out", default=os.path.expanduser("~/.human/logs/authorship-gap-%s.json" % time.strftime("%Y-%m-%d")))
    ap.add_argument("--splits", type=int, default=200)
    ap.add_argument("--min-trials", type=int, default=20)
    ap.add_argument("--seed", type=int, default=7)
    a = ap.parse_args()
    raw = json.load(open(a.trials))["trials"]
    # A trial only counts when BOTH texts exist: a missing/empty ai_response is a
    # failed generation, and "[timeout]" is the sentinel the generators emit.
    # --min-trials used to be checked on the raw row count, so 25 rows with zero
    # usable replies passed the input gate and the twin was computed on nothing.
    def usable(t):
        return (isinstance(t, dict)
                and isinstance(t.get("real_seth"), str) and t["real_seth"].strip()
                and isinstance(t.get("ai_response"), str) and t["ai_response"].strip()
                and not t["ai_response"].startswith("[timeout]"))
    trials = [t for t in raw if usable(t)]
    if not trials:
        sys.exit(f"REFUSING: 0 valid trials after filtering ({len(raw)} rows, none with both "
                 f"real_seth and ai_response); nothing written")
    if len(trials) < a.min_trials:
        sys.exit(f"REFUSING: {len(trials)} valid trials (of {len(raw)} rows) < {a.min_trials}; "
                 f"nothing written")
    try:
        profile = load_luar()
    except Exception as e:
        sys.exit(f"REFUSING: LUAR unavailable ({type(e).__name__}: {e}); nothing written")
    others = other_senders(a.chatdb, seed=a.seed)
    if len(others) < 20:
        sys.exit(f"REFUSING: only {len(others)} other senders with enough texts; floor would be noise")
    real = [t["real_seth"] for t in trials]; ai = [t["ai_response"] for t in trials]
    reg = ["casual" if len(r.split()) <= 12 else "substantive" for r in real]
    rng = random.Random(a.seed)
    # Pre-embed other-author profiles once (5 texts each, fixed).
    floor_profiles = [profile(rng.sample(o, 5)) for o in others]
    res = {"ceiling": [], "twin": [], "twin_casual": [], "twin_substantive": [], "floor": []}
    idx = list(range(len(trials)))
    for _ in range(a.splits):
        rng.shuffle(idx)
        prof_i, held_i, twin_i = idx[:5], idx[5:10], idx[10:15]
        P = profile([real[i] for i in prof_i])
        res["ceiling"].append(float(P @ profile([real[i] for i in held_i])))
        res["twin"].append(float(P @ profile([ai[i] for i in twin_i])))
        for r in ("casual", "substantive"):
            pool = [i for i in idx[5:] if reg[i] == r]
            if len(pool) >= 5:
                res[f"twin_{r}"].append(float(P @ profile([ai[i] for i in pool[:5]])))
        F = rng.choice(floor_profiles)
        res["floor"].append(float(P @ F))
    def stat(xs):
        if not xs: return None
        xs = sorted(xs); n = len(xs)
        return {"mean": round(sum(xs) / n, 3), "ci95": [round(xs[int(0.025 * n)], 3), round(xs[int(0.975 * n) - 1], 3)], "n": n}
    out = {"date": time.strftime("%Y-%m-%d"), "protocol": "PersonalBench 5v5 LUAR-MUD, 64-token episodes", "trials": len(trials), "trials_raw": len(raw),
           "registers": {"casual": reg.count("casual"), "substantive": reg.count("substantive")},
           "other_senders_for_floor": len(others), "splits": a.splits,
           "ceiling_seth_vs_seth": stat(res["ceiling"]), "twin_seth_vs_adapter": stat(res["twin"]),
           "twin_casual": stat(res["twin_casual"]), "twin_substantive": stat(res["twin_substantive"]),
           "floor_seth_vs_other_humans": stat(res["floor"])}
    c, t, f = out["ceiling_seth_vs_seth"]["mean"], out["twin_seth_vs_adapter"]["mean"], out["floor_seth_vs_other_humans"]["mean"]
    out["gap_closed_fraction"] = round((t - f) / (c - f), 3) if c != f else None
    out["reading"] = ("adapter is between other humans and Seth" if f < t < c else
                      "adapter is BELOW the cross-author floor (farther from Seth than strangers are)" if t <= f else
                      "adapter at/above ceiling")
    json.dump(out, open(a.out, "w"), indent=2)
    print(json.dumps({k: v for k, v in out.items() if k != "protocol"}, indent=1))
    print(f"wrote {a.out}")

if __name__ == "__main__":
    sys.exit(main())
