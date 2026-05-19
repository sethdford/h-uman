#!/usr/bin/env python3
"""
Layer 1 / Month 2-3 — PersonaEval-style speaker-ID classifier.

Trains a lightweight authorship-verification classifier on Seth's actual
sent messages (from persona JSON example_banks) vs. negative samples
(synthesized AI-assistant text + generic casual). For any candidate
response, returns P(Seth | text) with explainable feature contributions.

This is the SOTA-correlate metric per [PersonaEval (Aug 2025)
arXiv:2508.10014]: a deterministic classifier whose decisions are
reproducible and explainable, complementary to the LLM-judge.

Pure Python (no scikit-learn dependency). Logistic regression trained
via gradient descent on 15 hand-engineered style features. Model is
serialized to JSON for safety + portability.

For production-grade SOTA, swap the feature backbone for a
sentence-transformer (e.g. nomic-embed-text) + LightGBM — that's the
PersonaEval-paper architecture. This pure-Python version is correct-
shape and self-contained for rapid iteration.

Usage:
  python3 scripts/personaeval_speaker_id.py --train --out /tmp/seth_clf.json
  python3 scripts/personaeval_speaker_id.py --classify --model /tmp/seth_clf.json \\
      --text "yeah just sent it"
  python3 scripts/personaeval_speaker_id.py --score-runs --model /tmp/seth_clf.json
"""

import argparse
import json
import math
import re
import sqlite3
import sys
from pathlib import Path

PERSONA_PATH = Path.home() / ".human" / "personas" / "seth.json"
DB_PATH = Path.home() / ".human" / "memory.db"

AI_NEGATIVE_TEMPLATES = [
    "Depending on {x}, here are a few options:\n\n**If you {y}:** {a}\n**If you {z}:** {b}",
    "Of course! I'd be happy to help. {x} {y} {z}",
    "Certainly! Here are a few approaches:\n\n1. {a}\n2. {b}\n3. {c}",
    "Absolutely! When considering {x}, you should keep in mind {y}",
    "That sounds like a great question. Here's what I think: {x}",
    "I appreciate you sharing that. Here are a few things to consider: {a} {b}",
    "I understand. Let me suggest a few things. {x} {y}",
    "Great question! Here are the options:\n* {a}\n* {b}\n* {c}",
    "Here are some thoughts:\n\n### Option 1: The casual approach\n{a}\n\n### Option 2: The direct approach\n{b}",
    "I'm here to help! {x} {y}",
]

GENERIC_NEGATIVE_TEMPLATES = [
    "Sounds good to me!", "Okay sure thing.", "Will do.", "Got it.",
    "Thank you so much!", "I appreciate it.", "That's wonderful news.",
    "Let me know!", "Have a great day!", "Looking forward to it!",
]

_FEATURE_NAMES = [
    "len_chars", "len_words", "n_sentences", "avg_word_len",
    "lowercase_ratio", "has_bullet", "has_numbered", "has_header",
    "has_bold", "is_ai_opener", "is_seth_opener", "has_contraction",
    "has_lol_or_ha", "ends_with_period", "ends_with_q",
]


def extract_seth_positives():
    if not PERSONA_PATH.exists():
        return []
    p = json.loads(PERSONA_PATH.read_text())
    positives = []
    for bank in p.get("example_banks", []):
        for ex in bank.get("examples", []):
            r = ex.get("response", "").strip()
            if r:
                positives.append(r)
    humor = p.get("humor", {})
    for phrase in humor.get("signature_phrases", []):
        if isinstance(phrase, str) and phrase.strip():
            positives.append(phrase.strip())
    return positives


def generate_ai_negatives(n: int = 200):
    fillers = ["the situation", "your preference", "context", "what you mean",
               "are unsure", "are confident", "want clarity", "need details"]
    out = []
    i = 0
    while len(out) < n:
        t = AI_NEGATIVE_TEMPLATES[i % len(AI_NEGATIVE_TEMPLATES)]
        filled = t
        for key in "xyzabc":
            filled = filled.replace("{" + key + "}",
                                     fillers[(i * 7 + ord(key)) % len(fillers)])
        out.append(filled)
        i += 1
    return out


def generate_generic_negatives(n: int = 100):
    out = []
    i = 0
    while len(out) < n:
        out.append(GENERIC_NEGATIVE_TEMPLATES[i % len(GENERIC_NEGATIVE_TEMPLATES)])
        i += 1
    return out


def featurize(text: str) -> dict:
    if not text:
        return {f: 0.0 for f in _FEATURE_NAMES}
    n = len(text)
    words = text.split()
    nw = len(words)
    sents = re.split(r"[.!?]+", text)
    nsents = max(1, sum(1 for s in sents if s.strip()))
    lower_chars = sum(1 for c in text if c.islower())
    upper_chars = sum(1 for c in text if c.isupper())
    alpha = lower_chars + upper_chars
    has_bullet = 1.0 if re.search(r"^\s*[\*\-]\s+", text, re.MULTILINE) else 0.0
    has_numbered = 1.0 if re.search(r"^\s*\d+\.\s+", text, re.MULTILINE) else 0.0
    has_header = 1.0 if re.search(r"^#{1,6}\s+", text, re.MULTILINE) else 0.0
    has_bold = 1.0 if re.search(r"\*\*[^\*]{2,}\*\*", text) else 0.0
    first_token = (words[0] if words else "").lower().rstrip(",.!:;")
    ai_openers = {"depending", "certainly", "absolutely", "great", "of", "i", "here", "that"}
    seth_openers = {"yeah", "yo", "lol", "ha", "damn", "nah", "wait", "ugh",
                     "no", "ok", "kk", "fine", "sure", "hey", "hmm", "real"}
    is_ai_opener = 1.0 if first_token in ai_openers else 0.0
    is_seth_opener = 1.0 if first_token in seth_openers else 0.0
    has_contraction = 1.0 if re.search(
        r"\b(don't|i'm|you're|can't|won't|isn't|gonna|wanna|kinda|gotta)\b",
        text, re.IGNORECASE) else 0.0
    has_lol_or_ha = 1.0 if re.search(r"\b(lol|ha+)\b", text, re.IGNORECASE) else 0.0
    return {
        "len_chars": float(n), "len_words": float(nw), "n_sentences": float(nsents),
        "avg_word_len": (sum(len(w) for w in words) / nw) if nw else 0.0,
        "lowercase_ratio": (lower_chars / alpha) if alpha else 0.0,
        "has_bullet": has_bullet, "has_numbered": has_numbered,
        "has_header": has_header, "has_bold": has_bold,
        "is_ai_opener": is_ai_opener, "is_seth_opener": is_seth_opener,
        "has_contraction": has_contraction, "has_lol_or_ha": has_lol_or_ha,
        "ends_with_period": 1.0 if text.rstrip().endswith(".") else 0.0,
        "ends_with_q": 1.0 if text.rstrip().endswith("?") else 0.0,
    }


def sigmoid(z):
    if z < -50: return 0.0
    if z > 50: return 1.0
    return 1.0 / (1.0 + math.exp(-z))


def train_classifier():
    positives = extract_seth_positives()
    negatives = generate_ai_negatives(n=150) + generate_generic_negatives(n=100)
    print(f"Training: {len(positives)} Seth positives, {len(negatives)} negatives")
    X = []
    y = []
    for p in positives:
        feats = featurize(p)
        X.append([feats[k] for k in _FEATURE_NAMES])
        y.append(1)
    for n in negatives:
        feats = featurize(n)
        X.append([feats[k] for k in _FEATURE_NAMES])
        y.append(0)
    n_features = len(_FEATURE_NAMES)
    means = [sum(row[i] for row in X) / len(X) for i in range(n_features)]
    stds = []
    for i in range(n_features):
        var = sum((row[i] - means[i]) ** 2 for row in X) / max(1, len(X) - 1)
        stds.append(max(0.001, var ** 0.5))
    X_norm = [[(row[i] - means[i]) / stds[i] for i in range(n_features)] for row in X]
    weights = [0.0] * n_features
    bias = 0.0
    lr = 0.05
    n_epochs = 200
    for epoch in range(n_epochs):
        loss = 0.0
        grads_w = [0.0] * n_features
        grad_b = 0.0
        for xrow, ylabel in zip(X_norm, y):
            z = bias + sum(weights[i] * xrow[i] for i in range(n_features))
            p = sigmoid(z)
            p = min(max(p, 1e-7), 1 - 1e-7)
            loss -= ylabel * math.log(p) + (1 - ylabel) * math.log(1 - p)
            err = p - ylabel
            grad_b += err
            for i in range(n_features):
                grads_w[i] += err * xrow[i]
        m = len(X_norm)
        for i in range(n_features):
            weights[i] -= lr * grads_w[i] / m
        bias -= lr * grad_b / m
        if epoch == n_epochs - 1:
            print(f"  final loss: {loss/m:.4f}")
    correct = 0
    for xrow, ylabel in zip(X_norm, y):
        z = bias + sum(weights[i] * xrow[i] for i in range(n_features))
        pred = 1 if sigmoid(z) >= 0.5 else 0
        if pred == ylabel:
            correct += 1
    acc = correct / len(X_norm)
    print(f"  train accuracy: {acc:.3f}")
    return {"weights": weights, "bias": bias, "means": means, "stds": stds,
            "feature_names": _FEATURE_NAMES, "train_accuracy": acc,
            "n_positives": len(positives), "n_negatives": len(negatives)}


def classify_text(model: dict, text: str) -> dict:
    feats = featurize(text)
    x_raw = [feats[k] for k in model["feature_names"]]
    x_norm = [(x_raw[i] - model["means"][i]) / model["stds"][i]
              for i in range(len(x_raw))]
    z = model["bias"] + sum(model["weights"][i] * x_norm[i] for i in range(len(x_norm)))
    p = sigmoid(z)
    contribs = sorted(
        ((model["feature_names"][i], model["weights"][i] * x_norm[i])
         for i in range(len(x_norm))),
        key=lambda t: abs(t[1]), reverse=True,
    )
    return {"p_seth": p, "predicted_seth": p >= 0.5, "top_features": contribs[:5]}


def score_all_eval_runs(model: dict):
    con = sqlite3.connect(DB_PATH)
    rows = con.execute(
        "SELECT er.run_id, er.task_id, er.actual_output, runs.suite_name "
        "FROM eval_results er JOIN eval_runs runs ON runs.id = er.run_id "
        "WHERE er.actual_output IS NOT NULL AND length(er.actual_output) > 0 "
        "ORDER BY er.run_id, er.id"
    ).fetchall()
    print(f"Scoring {len(rows)} non-NULL eval responses with PersonaEval speaker-ID\n")
    by_run = {}
    for run_id, task_id, text, suite in rows:
        r = classify_text(model, text)
        by_run.setdefault((run_id, suite), []).append(r["p_seth"])
    print(f"{'run_id':>7} {'suite':<30} {'n':>4} {'mean_P(seth)':>14}")
    print("-" * 60)
    for (rid, suite), ps in sorted(by_run.items()):
        avg = sum(ps) / len(ps)
        print(f"{rid:>7} {suite:<30} {len(ps):>4} {avg:>14.3f}")


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--train", action="store_true")
    p.add_argument("--classify", action="store_true")
    p.add_argument("--text")
    p.add_argument("--score-runs", action="store_true")
    p.add_argument("--out", default="/tmp/seth_speaker_id.json")
    p.add_argument("--model", default="/tmp/seth_speaker_id.json")
    args = p.parse_args()
    if args.train:
        model = train_classifier()
        Path(args.out).write_text(json.dumps(model, indent=2))
        print(f"Saved: {args.out}")
        for sample in ["yeah just sent it", "Of course! Here are a few options",
                       "lol same", "Depending on your situation"]:
            r = classify_text(model, sample)
            print(f"  P(Seth | {sample!r}) = {r['p_seth']:.3f} "
                  f"({'SETH' if r['predicted_seth'] else 'NOT-SETH'})")
        return
    if args.classify:
        if not args.text:
            print("ERROR: --classify needs --text", file=sys.stderr)
            sys.exit(2)
        model = json.loads(Path(args.model).read_text())
        r = classify_text(model, args.text)
        print(f"P(Seth | text) = {r['p_seth']:.3f}")
        print(f"Predicted: {'SETH' if r['predicted_seth'] else 'NOT-SETH'}")
        for fname, contrib in r["top_features"]:
            sign = "+" if contrib >= 0 else "-"
            print(f"  {sign} {fname:<25} contribution={contrib:+.3f}")
        return
    if args.score_runs:
        model = json.loads(Path(args.model).read_text())
        score_all_eval_runs(model)
        return
    p.print_help()


if __name__ == "__main__":
    main()
