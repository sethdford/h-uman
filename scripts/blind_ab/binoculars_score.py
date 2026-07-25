#!/usr/bin/env python3
"""Binoculars-style AI-tell scoring with the base/adapted (LoRA) model pair.

Adaptation of Binoculars (arXiv 2401.12070) to a persona discriminator:
instead of two unrelated pretrained models, the observer/performer pair is
    base    = mlx-community/gemma-4-31b-it-8bit
    adapted = base + the seth LoRA adapter
For a text x (reply tokens, conditioned on the incoming message via the
chat template):

    logPPL_M(x)      = -mean_i log P_M(x_i | ctx, x_<i)
    XEnt(P->O)(x)    = -mean_i sum_v P_perf(v | ...) * log P_obs(v | ...)
    binoculars score = logPPL_observer / XEnt(performer -> observer)

Both direction assignments are computed in one run:
    dirA: observer=base,    performer=adapted   (paper default here)
    dirB: observer=adapted, performer=base

Memory: only ONE 31B model is resident at a time. Pass 1 runs the base
model and caches per-position full-vocab log-softmax (float16 .npy) to
--cache-dir; pass 2 frees the base, loads base+adapter, and combines.

No side effects without flags: results go to stdout unless --out is given;
the cache dir is only written under --cache-dir (default: a temp dir that
is removed on exit).

Usage:
    # score the paired blinded-AB corpus + AUC + windowed AUC
    python3 binoculars_score.py --pairs data/eval_blinded_ab.json \
        --out /tmp/binoculars_results.json --windows 3,5,10

    # score arbitrary texts (JSON list of {"text": ..., "context": ...,
    # "label": "real"|"ai" (optional)})
    python3 binoculars_score.py --texts my_texts.json

    # one-off
    python3 binoculars_score.py --text "yo running late be there in 10"

    # verify the math on synthetic distributions (no model load)
    python3 binoculars_score.py --selftest
"""
import argparse
import gc
import json
import math
import os
import random
import sys
import tempfile

DEFAULT_BASE = "mlx-community/gemma-4-31b-it-8bit"
DEFAULT_ADAPTER = os.path.expanduser(
    "~/.human/training-data/adapters/seth-lora-v5-8bit-20260718-105251")


# ---------------------------------------------------------------- data ----

def load_items(args):
    """Normalize input into [{'text', 'context', 'label'}, ...]."""
    items = []
    if args.pairs:
        with open(args.pairs) as f:
            doc = json.load(f)
        trials = doc["trials"] if isinstance(doc, dict) else doc
        for t in trials:
            ctx = t.get("incoming", "")
            items.append({"text": t["real_seth"], "context": ctx,
                          "label": "real", "trial": t.get("i")})
            items.append({"text": t["ai_response"], "context": ctx,
                          "label": "ai", "trial": t.get("i")})
    elif args.texts:
        with open(args.texts) as f:
            rows = json.load(f)
        for r in rows:
            if isinstance(r, str):
                r = {"text": r}
            items.append({"text": r["text"], "context": r.get("context", ""),
                          "label": r.get("label"), "trial": r.get("trial")})
    elif args.text:
        items.append({"text": args.text, "context": args.context or "",
                      "label": None, "trial": None})
    else:
        raise SystemExit("one of --pairs / --texts / --text is required")
    return [it for it in items if it["text"].strip()]


# ---------------------------------------------------------- tokenization ----

def response_token_span(tokenizer, context, text):
    """Token ids for (chat-templated context + reply) and the reply span.

    Returns (full_ids, resp_start, resp_len): positions
    full_ids[resp_start : resp_start+resp_len] are the reply content tokens
    (template suffix like <end_of_turn> excluded when identifiable).
    """
    if context:
        msgs_prompt = [{"role": "user", "content": context}]
        prompt_ids = tokenizer.apply_chat_template(
            msgs_prompt, add_generation_prompt=True)
        full_ids = tokenizer.apply_chat_template(
            msgs_prompt + [{"role": "assistant", "content": text}],
            add_generation_prompt=False)
        if full_ids[:len(prompt_ids)] != prompt_ids:
            # template not prefix-stable; fall back to raw scoring
            return response_token_span(tokenizer, "", text)
        resp_start = len(prompt_ids)
    else:
        bos = [tokenizer.bos_token_id] if tokenizer.bos_token_id is not None else []
        full_ids = bos + tokenizer.encode(text, add_special_tokens=False)
        resp_start = len(bos)

    # prefer the exact content-token subsequence; else take the tail minus
    # trailing special tokens
    content = tokenizer.encode(text, add_special_tokens=False)
    if full_ids[resp_start:resp_start + len(content)] == content:
        resp_len = len(content)
    else:
        tail = full_ids[resp_start:]
        specials = set(getattr(tokenizer, "all_special_ids", []) or [])
        while tail and tail[-1] in specials:
            tail = tail[:-1]
        resp_len = len(tail)
    if resp_len == 0:  # pathological: keep at least one position
        resp_len = max(1, len(full_ids) - resp_start)
    return full_ids, resp_start, resp_len


# ------------------------------------------------------------- scoring ----

def _forward_logprobs(model, ids, resp_start, resp_len):
    """Full-vocab log-softmax at the positions that PREDICT the reply tokens.

    Position p in the logits predicts token p+1, so the predicting slice is
    [resp_start-1, resp_start-1+resp_len).
    """
    import mlx.core as mx
    x = mx.array(ids)[None]
    logits = model(x)[0]  # (seq, vocab)
    sl = logits[resp_start - 1: resp_start - 1 + resp_len, :].astype(mx.float32)
    lp = sl - mx.logsumexp(sl, axis=-1, keepdims=True)
    lp16 = lp.astype(mx.float16)
    mx.eval(lp16)
    return lp16


def run_pass(model_path, adapter_path, items, cache_dir, tag, quiet=False):
    """Score all items under one model; cache per-item logprobs as .npy."""
    import mlx.core as mx
    from mlx_lm import load

    kwargs = {"adapter_path": adapter_path} if adapter_path else {}
    if not quiet:
        print(f"[{tag}] loading {model_path}"
              + (f" + adapter {adapter_path}" if adapter_path else ""),
              file=sys.stderr)
    model, tokenizer = load(model_path, **kwargs)

    meta = []
    for idx, it in enumerate(items):
        ids, rs, rl = response_token_span(tokenizer, it["context"], it["text"])
        lp = _forward_logprobs(model, ids, rs, rl)
        import numpy as np
        np.save(os.path.join(cache_dir, f"{tag}_{idx:05d}.npy"),
                np.array(lp, copy=False))
        tok_ids = ids[rs:rs + rl]
        meta.append({"idx": idx, "resp_tokens": tok_ids, "n_tokens": rl})
        if not quiet and (idx + 1) % 10 == 0:
            print(f"[{tag}] {idx + 1}/{len(items)}", file=sys.stderr)
        mx.clear_cache()

    del model
    gc.collect()
    mx.clear_cache()
    return meta


def combine(items, meta, cache_dir):
    """Compute per-item logPPL / cross-entropy sums for both directions."""
    import numpy as np
    results = []
    for m in meta:
        idx = m["idx"]
        base_lp = np.load(os.path.join(cache_dir, f"base_{idx:05d}.npy")).astype(np.float32)
        adpt_lp = np.load(os.path.join(cache_dir, f"adapted_{idx:05d}.npy")).astype(np.float32)
        toks = np.array(m["resp_tokens"])
        n = len(toks)
        rows = np.arange(n)

        nll_base = -base_lp[rows, toks]          # -log P_base(x_i)
        nll_adpt = -adpt_lp[rows, toks]
        p_base = np.exp(base_lp)
        p_adpt = np.exp(adpt_lp)
        xent_a2b = -(p_adpt * base_lp).sum(axis=1)   # performer=adapted, observer=base
        xent_b2a = -(p_base * adpt_lp).sum(axis=1)   # performer=base, observer=adapted

        it = dict(items[idx])
        it.pop("context", None)
        it.update({
            "n_tokens": int(n),
            "sum_nll_base": float(nll_base.sum()),
            "sum_nll_adapted": float(nll_adpt.sum()),
            "sum_xent_a2b": float(xent_a2b.sum()),
            "sum_xent_b2a": float(xent_b2a.sum()),
        })
        it["logppl_base"] = it["sum_nll_base"] / n
        it["logppl_adapted"] = it["sum_nll_adapted"] / n
        it["score_dirA"] = it["sum_nll_base"] / max(it["sum_xent_a2b"], 1e-9)
        it["score_dirB"] = it["sum_nll_adapted"] / max(it["sum_xent_b2a"], 1e-9)
        # delta log-likelihood per token (adapted minus base) — the simplest
        # persona-contrast baseline, reported alongside binoculars ratios
        it["score_dll"] = (it["sum_nll_base"] - it["sum_nll_adapted"]) / n
        results.append(it)
    return results


# ----------------------------------------------------------- statistics ----

def auc(pos_scores, neg_scores):
    """Rank-based ROC AUC (probability a positive outranks a negative)."""
    pairs = [(s, 1) for s in pos_scores] + [(s, 0) for s in neg_scores]
    pairs.sort(key=lambda t: t[0])
    ranks, i = {}, 0
    while i < len(pairs):
        j = i
        while j < len(pairs) and pairs[j][0] == pairs[i][0]:
            j += 1
        r = (i + j + 1) / 2.0  # average rank, 1-based
        for k in range(i, j):
            ranks[k] = r
        i = j
    rank_sum_pos = sum(ranks[k] for k, (_, lab) in enumerate(pairs) if lab == 1)
    n1, n0 = len(pos_scores), len(neg_scores)
    if n1 == 0 or n0 == 0:
        return float("nan")
    return (rank_sum_pos - n1 * (n1 + 1) / 2.0) / (n1 * n0)


def threshold_at_fpr(pos_scores, neg_scores, fpr, higher_is_positive):
    """Threshold on the negative (real) distribution at a target FPR,
    plus the TPR that threshold achieves. Returns (thr, tpr, actual_fpr)."""
    neg = sorted(neg_scores, reverse=higher_is_positive)
    k = max(0, min(len(neg) - 1, int(math.floor(fpr * len(neg)))))
    # threshold sits just past the k-th most extreme negative
    thr = neg[k]
    if higher_is_positive:
        tp = sum(1 for s in pos_scores if s > thr)
        fp = sum(1 for s in neg_scores if s > thr)
    else:
        tp = sum(1 for s in pos_scores if s < thr)
        fp = sum(1 for s in neg_scores if s < thr)
    return thr, tp / len(pos_scores), fp / len(neg_scores)


def windowed_auc(results, score_key, num_key, den_key, k, n_samples=400, seed=7):
    """AUC over token-pooled window scores: sample k same-class messages,
    pool numerator/denominator sums (equivalent to concatenation)."""
    rng = random.Random(seed)
    by_label = {"real": [], "ai": []}
    for r in results:
        if r.get("label") in by_label:
            by_label[r["label"]].append(r)
    if min(len(v) for v in by_label.values()) < k:
        return float("nan")
    pooled = {"real": [], "ai": []}
    for lab, rows in by_label.items():
        for _ in range(n_samples):
            grp = rng.sample(rows, k)
            num = sum(g[num_key] for g in grp)
            den = sum(g[den_key] for g in grp)
            if den_key == "n_tokens":
                pooled[lab].append(num / den)
            else:
                pooled[lab].append(num / max(den, 1e-9))
    return auc(pooled["ai"], pooled["real"])


SCORE_DEFS = {
    # name: (per-message key, pooled numerator, pooled denominator)
    "dirA (obs=base)":    ("score_dirA", "sum_nll_base", "sum_xent_a2b"),
    "dirB (obs=adapted)": ("score_dirB", "sum_nll_adapted", "sum_xent_b2a"),
    "dll (adapted-base)": ("score_dll", None, "n_tokens"),
    "logppl_base":        ("logppl_base", "sum_nll_base", "n_tokens"),
    "logppl_adapted":     ("logppl_adapted", "sum_nll_adapted", "n_tokens"),
}


def analyze(results, windows):
    labeled = [r for r in results if r.get("label") in ("real", "ai")]
    if (not labeled
            or not any(r["label"] == "ai" for r in labeled)
            or not any(r["label"] == "real" for r in labeled)):
        return None  # AUC/thresholds need both classes
    report = {"n_real": sum(1 for r in labeled if r["label"] == "real"),
              "n_ai": sum(1 for r in labeled if r["label"] == "ai"),
              "scores": {}}
    for name, (key, num_key, den_key) in SCORE_DEFS.items():
        ai = [r[key] for r in labeled if r["label"] == "ai"]
        real = [r[key] for r in labeled if r["label"] == "real"]
        a = auc(ai, real)  # AUC of "score is higher for AI"
        oriented_higher_ai = a >= 0.5
        a_or = a if oriented_higher_ai else 1.0 - a
        entry = {"auc_raw_higher_is_ai": round(a, 4),
                 "auc_oriented": round(a_or, 4),
                 "ai_is": "higher" if oriented_higher_ai else "lower"}
        for fpr in (0.05, 0.10):
            thr, tpr, afpr = threshold_at_fpr(ai, real, fpr, oriented_higher_ai)
            entry[f"tpr_at_fpr{int(fpr*100)}"] = {
                "threshold": round(thr, 5), "tpr": round(tpr, 3),
                "actual_fpr": round(afpr, 3)}
        if num_key:
            entry["windowed"] = {}
            for k in windows:
                wa = windowed_auc(labeled, key, num_key, den_key, k)
                if not math.isnan(wa):
                    entry["windowed"][f"k={k}"] = round(max(wa, 1 - wa), 4)
        report["scores"][name] = entry
    return report


# -------------------------------------------------------------- selftest ----

def selftest():
    import numpy as np
    # hand-computable 3-token, 4-vocab case
    base_lp = np.log(np.array([[0.7, 0.1, 0.1, 0.1],
                               [0.25, 0.25, 0.25, 0.25],
                               [0.1, 0.6, 0.2, 0.1]], dtype=np.float64))
    adpt_lp = np.log(np.array([[0.4, 0.3, 0.2, 0.1],
                               [0.1, 0.7, 0.1, 0.1],
                               [0.25, 0.25, 0.25, 0.25]], dtype=np.float64))
    toks = np.array([0, 1, 1])
    nll_base = -base_lp[np.arange(3), toks]
    assert abs(nll_base.sum() - (-math.log(0.7) - math.log(0.25) - math.log(0.6))) < 1e-9
    xent = -(np.exp(adpt_lp) * base_lp).sum(axis=1)
    manual0 = -sum(p * lb for p, lb in zip([0.4, 0.3, 0.2, 0.1],
                                           np.log([0.7, 0.1, 0.1, 0.1])))
    assert abs(xent[0] - manual0) < 1e-9
    # AUC sanity: perfect separation and ties
    assert auc([2, 3, 4], [0, 1, 1.5]) == 1.0
    assert auc([0, 1], [0, 1]) == 0.5
    # pairwise wins: 1>0, 2>0, 3>{0,2,2.5} plus the 2==2 tie -> 5.5/9
    assert abs(auc([1, 2, 3], [0, 2, 2.5]) - (5.5 / 9)) < 1e-9
    # threshold at FPR: with 10 negatives, fpr=0.1 admits ~1
    thr, tpr, afpr = threshold_at_fpr(list(range(10, 20)),
                                      list(range(0, 10)), 0.10, True)
    assert tpr == 1.0 and afpr <= 0.10
    print("selftest OK")


# ------------------------------------------------------------------ main ----

def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    src = ap.add_mutually_exclusive_group()
    src.add_argument("--pairs", help="eval_blinded_ab.json-shaped file "
                     "(trials with incoming/real_seth/ai_response)")
    src.add_argument("--texts", help="JSON list of {text, context?, label?}")
    src.add_argument("--text", help="single text to score")
    ap.add_argument("--context", help="incoming message for --text")
    ap.add_argument("--base", default=DEFAULT_BASE)
    ap.add_argument("--adapter", default=DEFAULT_ADAPTER)
    ap.add_argument("--no-context", action="store_true",
                    help="score text alone (paper-style), ignore incoming")
    ap.add_argument("--cache-dir", help="keep pass-1/2 logprob cache here "
                    "(default: temp dir, removed on exit)")
    ap.add_argument("--stage", choices=["base", "adapted", "combine", "all"],
                    default="all", help="resumable staging (requires --cache-dir)")
    ap.add_argument("--windows", default="5",
                    help="comma-separated window sizes for aggregated AUC")
    ap.add_argument("--out", help="write full results JSON here")
    ap.add_argument("--quiet", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()

    if args.selftest:
        selftest()
        return

    items = load_items(args)
    if args.no_context:
        for it in items:
            it["context"] = ""
    windows = [int(w) for w in args.windows.split(",") if w.strip()]

    tmp = None
    if args.cache_dir:
        cache_dir = args.cache_dir
        os.makedirs(cache_dir, exist_ok=True)
    else:
        if args.stage != "all":
            raise SystemExit("--stage requires --cache-dir")
        tmp = tempfile.TemporaryDirectory(prefix="binoculars_")
        cache_dir = tmp.name

    meta_path = os.path.join(cache_dir, "meta.json")
    try:
        if args.stage in ("base", "all"):
            meta = run_pass(args.base, None, items, cache_dir, "base",
                            quiet=args.quiet)
            with open(meta_path, "w") as f:
                json.dump(meta, f)
        if args.stage in ("adapted", "all"):
            run_pass(args.base, args.adapter, items, cache_dir, "adapted",
                     quiet=args.quiet)
        if args.stage in ("combine", "all"):
            with open(meta_path) as f:
                meta = json.load(f)
            results = combine(items, meta, cache_dir)
            report = analyze(results, windows)
            out = {"base": args.base, "adapter": args.adapter,
                   "contexted": not args.no_context,
                   "n_texts": len(results), "analysis": report,
                   "results": results}
            if args.out:
                with open(args.out, "w") as f:
                    json.dump(out, f, indent=1)
                print(f"wrote {args.out}", file=sys.stderr)
            if report:
                print(json.dumps({"analysis": report}, indent=1))
            else:
                print(json.dumps(out, indent=1))
    finally:
        if tmp:
            tmp.cleanup()


if __name__ == "__main__":
    main()
