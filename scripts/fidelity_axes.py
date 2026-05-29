#!/usr/bin/env python3
"""
Wave 7 / W7-2 — per-axis voice-fidelity decomposition.

"Sounds like me" is not one number. This decomposes it into stylometric axes —
vocabulary, length/rhythm, casing, abbreviation, emoji, hedging, punctuation
energy, humor — and scores each by comparing a set of MODEL responses against a
REFERENCE corpus of the target's real messages. Output names which axes are
close and which are off, so we optimize the actual gap instead of an aggregate.

Deterministic (no model/network) — this is authorship-style stylometry, the
rigorous-but-cheap complement to LLM-judge and blind human eval. Each axis
returns a similarity in [0,1] (1 = matches the reference distribution).

Usage:
  scripts/fidelity_axes.py --reference real.jsonl --responses model.jsonl \\
      [--output-json axes.json]

Input files: JSONL with a text field (text/response/real_response/content) or
plain-text, one message per line.
"""

import argparse
import json
import statistics
import sys
from pathlib import Path

# Casual/abbreviation tokens characteristic of texting voice.
ABBREV = {
    "u", "ur", "r", "rn", "ty", "thx", "tbh", "lol", "lmao", "lmfao", "idk", "imo",
    "imho", "btw", "omg", "ngl", "fr", "fyi", "ofc", "pls", "plz", "gonna", "wanna",
    "gotta", "kinda", "sorta", "dunno", "yeah", "nah", "yep", "nope", "ok", "k",
}
HEDGES = {"maybe", "probably", "i think", "i guess", "kinda", "sorta", "might", "perhaps", "possibly", "idk", "not sure"}
HUMOR = {"lol", "lmao", "lmfao", "haha", "hahaha", "jk", "lolol", "rofl"}
STOPWORDS = {
    "the", "a", "an", "and", "or", "but", "to", "of", "in", "on", "for", "with",
    "is", "it", "i", "you", "im", "its", "that", "this", "be", "do", "so", "at",
    "as", "we", "he", "she", "they", "my", "me", "your", "are", "was", "if", "have",
}


def _words(text):
    return "".join(c.lower() if (c.isalnum() or c.isspace() or c == "'") else " " for c in text).split()


def _emoji_count(text):
    # Count UTF-8 emoji in the U+1F000–U+1FAFF + misc symbol ranges (approx).
    n = 0
    for ch in text:
        o = ord(ch)
        if 0x1F000 <= o <= 0x1FAFF or 0x2600 <= o <= 0x27BF or 0x1F300 <= o <= 0x1F9FF:
            n += 1
    return n


def message_features(text):
    """Per-message scalar features (all normalized so corpora are comparable)."""
    text = text or ""
    words = _words(text)
    nwords = max(len(words), 1)
    alpha = [c for c in text if c.isalpha()]
    lower_ratio = (sum(1 for c in alpha if c.islower()) / len(alpha)) if alpha else 0.0
    abbrev = sum(1 for w in words if w in ABBREV) / nwords
    hedge = sum(1 for h in HEDGES if h in text.lower()) / nwords
    humor = sum(1 for w in words if w in HUMOR) / nwords
    emoji = _emoji_count(text) / nwords
    punct_energy = sum(text.count(c) for c in "!?") / nwords + text.count("...") / nwords
    return {
        "len_chars": float(len(text)),
        "words": float(len(words)),
        "lower_ratio": lower_ratio,
        "abbrev_ratio": abbrev,
        "hedge_ratio": hedge,
        "humor_ratio": humor,
        "emoji_ratio": emoji,
        "punct_energy": punct_energy,
    }


def corpus_profile(messages):
    """Mean of each per-message feature + a content-word frequency set."""
    feats = [message_features(m) for m in messages] or [message_features("")]
    keys = feats[0].keys()
    profile = {k: statistics.mean(f[k] for f in feats) for k in keys}
    content = {}
    for m in messages:
        for w in _words(m):
            if w not in STOPWORDS and len(w) > 2:
                content[w] = content.get(w, 0) + 1
    profile["_content_words"] = content
    return profile


def _topset(content, k=50):
    return {w for w, _ in sorted(content.items(), key=lambda x: -x[1])[:k]}


def _sim_scalar(ref, model, denom):
    if denom <= 0:
        denom = 1.0
    return max(0.0, 1.0 - min(1.0, abs(ref - model) / denom))


def axis_scores(ref_profile, model_profile):
    """Per-axis similarity in [0,1] (1 = model matches reference distribution)."""
    rlen = ref_profile["len_chars"]
    rwords = ref_profile["words"]
    length_rhythm = (
        _sim_scalar(rlen, model_profile["len_chars"], max(rlen, 40.0))
        + _sim_scalar(rwords, model_profile["words"], max(rwords, 8.0))
    ) / 2.0

    ref_top = _topset(ref_profile["_content_words"])
    mod_top = _topset(model_profile["_content_words"])
    if ref_top or mod_top:
        vocab = len(ref_top & mod_top) / max(len(ref_top | mod_top), 1)
    else:
        vocab = 1.0

    return {
        "vocabulary": round(vocab, 3),
        "length_rhythm": round(length_rhythm, 3),
        "casing": round(_sim_scalar(ref_profile["lower_ratio"], model_profile["lower_ratio"], 1.0), 3),
        "abbreviation": round(_sim_scalar(ref_profile["abbrev_ratio"], model_profile["abbrev_ratio"], 0.3), 3),
        "hedging": round(_sim_scalar(ref_profile["hedge_ratio"], model_profile["hedge_ratio"], 0.3), 3),
        "emoji": round(_sim_scalar(ref_profile["emoji_ratio"], model_profile["emoji_ratio"], 0.3), 3),
        "punct_energy": round(_sim_scalar(ref_profile["punct_energy"], model_profile["punct_energy"], 0.5), 3),
        "humor": round(_sim_scalar(ref_profile["humor_ratio"], model_profile["humor_ratio"], 0.2), 3),
    }


def decompose(ref_messages, model_messages):
    ref = corpus_profile(ref_messages)
    mod = corpus_profile(model_messages)
    axes = axis_scores(ref, mod)
    aggregate = round(statistics.mean(axes.values()), 3)
    weakest = sorted(axes.items(), key=lambda x: x[1])[:3]
    return {
        "aggregate": aggregate,
        "axes": axes,
        "weakest_axes": [{"axis": a, "score": s} for a, s in weakest],
        "n_reference": len(ref_messages),
        "n_model": len(model_messages),
    }


def load_messages(path):
    out = []
    for line in Path(path).read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
            if isinstance(obj, dict):
                out.append(obj.get("text") or obj.get("response") or obj.get("real_response")
                           or obj.get("content") or "")
                continue
        except (json.JSONDecodeError, ValueError):
            pass
        out.append(line)
    return out


def main():
    ap = argparse.ArgumentParser(description="Per-axis voice-fidelity decomposition (W7-2)")
    ap.add_argument("--reference", type=Path, required=True, help="real target messages")
    ap.add_argument("--responses", type=Path, required=True, help="model responses to score")
    ap.add_argument("--output-json", type=Path, default=None)
    args = ap.parse_args()
    for p in (args.reference, args.responses):
        if not p.exists():
            print(f"[error] not found: {p}", file=sys.stderr)
            return 1
    result = decompose(load_messages(args.reference), load_messages(args.responses))
    print(json.dumps(result, indent=2))
    print(f"\n=== FIDELITY {result['aggregate']:.2f} — weakest axes ===", file=sys.stderr)
    for w in result["weakest_axes"]:
        print(f"  - {w['axis']}: {w['score']}", file=sys.stderr)
    if args.output_json:
        args.output_json.write_text(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
