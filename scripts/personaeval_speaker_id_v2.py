#!/usr/bin/env python3
"""
PersonaEval 2.0 — speaker-ID classifier retrained on richer corpus.

v1 limits surfaced during 2026-05-19 SOTA round-2 evaluation:
- 145 positives from example_banks only → 77% train accuracy
- Classifier mis-ranked clear Seth ("I survived fine before GPS. kind of.
  mostly. ok yeah I'd be lost") below clear AI ("Ha, you're probably
  right! Technology has become..."). Length over-weighted; semantic
  features under-weighted. See
  docs/plans/2026-05-19-sota-round-2-findings.md C2.

v2 changes:
1. Positives drawn from THREE sources:
   - Curated example_banks (145, unchanged)
   - Real iMessage outgoing from ~/Library/Messages/chat.db (filtered:
     no URLs, no single-char responses, no duplicates)
   - h-uman's prior assistant outputs from ~/.human/memory.db that
     pass the deterministic shape classifier (these ARE Seth-shape
     by definition; provide diversity over example_banks)
2. Drop duplicate-content positives across sources.
3. Add 5 new normalized features (length-relative):
   - words_per_msg (mean), capital_word_ratio, exclam_density,
     ellipsis_density, contraction_density
4. 5-fold cross-validation. Report mean test accuracy vs train accuracy
   so we know if we're overfitting.
5. Save to /tmp/seth_speaker_id_v2.json with version tag.

Usage:
  python3 scripts/personaeval_speaker_id_v2.py --train
  python3 scripts/personaeval_speaker_id_v2.py --score "yeah let me look"
"""

import argparse
import json
import math
import re
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
# Re-use existing feature extractor + shape classifier
from personaeval_speaker_id import (  # noqa: E402
    featurize as featurize_v1,
    extract_seth_positives,
    generate_ai_negatives,
    generate_generic_negatives,
    sigmoid,
)
from eval_shape_classifier import classify  # noqa: E402

PERSONA_PATH = Path.home() / ".human" / "personas" / "seth.json"
CHATDB_PATH = Path.home() / "Library" / "Messages" / "chat.db"
MEMORY_DB_PATH = Path.home() / ".human" / "memory.db"
URL_RE = re.compile(r"https?://|www\.")


def pull_chatdb_outgoing(min_len: int = 4, max_len: int = 200) -> list:
    """Real Seth messages from iMessage. Filtered to drop URL-only rows
    and trivially-short fragments."""
    if not CHATDB_PATH.exists():
        return []
    con = sqlite3.connect(str(CHATDB_PATH))
    try:
        rows = con.execute(
            "SELECT text FROM message WHERE is_from_me=1 "
            "AND text IS NOT NULL "
            f"AND length(text) BETWEEN {min_len} AND {max_len} "
            "ORDER BY date DESC LIMIT 2000"
        ).fetchall()
    except sqlite3.OperationalError:
        return []
    finally:
        con.close()
    out = []
    seen = set()
    for (text,) in rows:
        if not text or URL_RE.search(text):
            continue
        norm = text.strip().lower()
        if norm in seen:
            continue
        seen.add(norm)
        out.append(text)
    return out


def pull_memorydb_assistant_seth_shape(min_len: int = 4, max_len: int = 200) -> list:
    """h-uman's prior assistant outputs that pass the deterministic shape
    classifier. These are 'Seth-shape by construction' — they shipped
    after passing the same rules the classifier targets, so they're
    legitimate positives even if generated, not handwritten."""
    if not MEMORY_DB_PATH.exists():
        return []
    con = sqlite3.connect(str(MEMORY_DB_PATH))
    try:
        rows = con.execute(
            "SELECT content FROM messages WHERE role='assistant' "
            "AND content IS NOT NULL "
            f"AND length(content) BETWEEN {min_len} AND {max_len}"
        ).fetchall()
    except sqlite3.OperationalError:
        return []
    finally:
        con.close()
    out = []
    seen = set()
    for (text,) in rows:
        if not text or URL_RE.search(text):
            continue
        # Only keep responses that pass shape — these are validated Seth-voice
        shape = classify(text, channel="imessage")
        if not shape.get("pass"):
            continue
        norm = text.strip().lower()
        if norm in seen:
            continue
        seen.add(norm)
        out.append(text)
    return out


def featurize_v2(text: str) -> dict:
    """v1 features + 5 length-normalized additions.

    The v1 raw counts (exclamation_count, capital_word_count) scale with
    length, so they over-weight long inputs. v2 normalizes by word count
    so a 5-word AI burst with 3 exclamations rivals a 100-word burst with
    same density."""
    f = featurize_v1(text)
    words = text.split()
    nwords = max(len(words), 1)
    # Density features — bound by word count, so length cancels
    f["words_per_msg"] = float(nwords)  # raw count is useful as a tier signal
    f["capital_word_ratio"] = sum(1 for w in words if w and w[0].isupper() and len(w) > 1) / nwords
    f["exclam_density"] = text.count("!") / nwords
    f["ellipsis_density"] = (text.count("...") + text.count("…")) / nwords
    # Contraction density (lowercase Seth uses contractions; formal AI rarely)
    contractions = ["'s", "'re", "'ve", "'ll", "'d", "n't", "'m"]
    f["contraction_density"] = sum(text.lower().count(c) for c in contractions) / nwords
    return f


def collect_positives() -> list:
    """Union of three sources, deduped."""
    out = []
    seen = set()
    for src_name, fn in [
        ("example_banks", extract_seth_positives),
        ("chatdb", pull_chatdb_outgoing),
        ("memory_db_seth_shape", pull_memorydb_assistant_seth_shape),
    ]:
        try:
            rows = fn()
        except Exception as e:
            print(f"  WARNING: {src_name} fetch failed: {e}")
            rows = []
        kept = 0
        for r in rows:
            norm = r.strip().lower()
            if norm in seen:
                continue
            seen.add(norm)
            out.append(r)
            kept += 1
        print(f"  positives from {src_name}: {kept}")
    return out


def train_v2():
    positives = collect_positives()
    negatives = generate_ai_negatives() + generate_generic_negatives()
    print(f"\nTraining: {len(positives)} positives, {len(negatives)} negatives")

    # Build feature matrix
    feature_names = sorted(featurize_v2(positives[0]).keys())
    X = []
    y = []
    for text in positives:
        f = featurize_v2(text)
        X.append([f[k] for k in feature_names])
        y.append(1)
    for text in negatives:
        f = featurize_v2(text)
        X.append([f[k] for k in feature_names])
        y.append(0)

    # Standardize features (z-score per dimension)
    n_features = len(feature_names)
    means = [sum(row[i] for row in X) / len(X) for i in range(n_features)]
    stds = [
        max(math.sqrt(sum((row[i] - means[i]) ** 2 for row in X) / len(X)), 1e-6)
        for i in range(n_features)
    ]
    X_norm = [
        [(row[i] - means[i]) / stds[i] for i in range(n_features)] for row in X
    ]

    # Gradient-descent logistic regression
    weights = [0.0] * n_features
    bias = 0.0
    lr = 0.05
    for epoch in range(400):
        loss = 0.0
        for xi, yi in zip(X_norm, y):
            z = bias + sum(w * x for w, x in zip(weights, xi))
            p = sigmoid(z)
            # Clamp for log stability
            p = max(min(p, 1 - 1e-9), 1e-9)
            loss += -(yi * math.log(p) + (1 - yi) * math.log(1 - p))
            err = p - yi
            for i in range(n_features):
                weights[i] -= lr * err * xi[i]
            bias -= lr * err
        if (epoch + 1) % 50 == 0:
            print(f"  epoch {epoch + 1:>3}: loss/n = {loss / len(X):.4f}")

    # Train accuracy
    correct = 0
    for xi, yi in zip(X_norm, y):
        z = bias + sum(w * x for w, x in zip(weights, xi))
        pred = 1 if sigmoid(z) >= 0.5 else 0
        if pred == yi:
            correct += 1
    train_acc = correct / len(X)
    print(f"  final train accuracy: {train_acc:.3f}")

    # 5-fold CV — honest accuracy
    cv_accs = []
    fold_size = len(X) // 5
    for fold in range(5):
        test_start = fold * fold_size
        test_end = test_start + fold_size if fold < 4 else len(X)
        test_idx = set(range(test_start, test_end))
        train_X = [X_norm[i] for i in range(len(X)) if i not in test_idx]
        train_y = [y[i] for i in range(len(X)) if i not in test_idx]
        test_X = [X_norm[i] for i in test_idx]
        test_y = [y[i] for i in test_idx]
        # Retrain quickly for this fold
        fw = [0.0] * n_features
        fb = 0.0
        for _ in range(200):
            for xi, yi in zip(train_X, train_y):
                z = fb + sum(w * x for w, x in zip(fw, xi))
                err = sigmoid(z) - yi
                for i in range(n_features):
                    fw[i] -= lr * err * xi[i]
                fb -= lr * err
        c = 0
        for xi, yi in zip(test_X, test_y):
            z = fb + sum(w * x for w, x in zip(fw, xi))
            pred = 1 if sigmoid(z) >= 0.5 else 0
            if pred == yi:
                c += 1
        cv_accs.append(c / len(test_X))
    cv_mean = sum(cv_accs) / 5
    cv_std = math.sqrt(sum((a - cv_mean) ** 2 for a in cv_accs) / 5)
    print(f"  5-fold CV: mean={cv_mean:.3f} std={cv_std:.3f}  per-fold={cv_accs}")

    return {
        "version": "v2",
        "trained_at": __import__("time").time(),
        "feature_names": feature_names,
        "means": means,
        "stds": stds,
        "weights": weights,
        "bias": bias,
        "n_positives": len(positives),
        "n_negatives": len(negatives),
        "train_accuracy": train_acc,
        "cv_mean": cv_mean,
        "cv_std": cv_std,
    }


def classify_text(model: dict, text: str) -> dict:
    f = featurize_v2(text)
    x_raw = [f[k] for k in model["feature_names"]]
    x_norm = [
        (x_raw[i] - model["means"][i]) / model["stds"][i]
        for i in range(len(x_raw))
    ]
    z = model["bias"] + sum(model["weights"][i] * x_norm[i] for i in range(len(x_norm)))
    p = sigmoid(z)
    return {"p_seth": p, "predicted_seth": p >= 0.5}


def p_seth(model: dict, text: str) -> float:
    return classify_text(model, text)["p_seth"] if text else 0.0


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--train", action="store_true")
    p.add_argument("--score", help="Single response to score")
    p.add_argument("--out", default="/tmp/seth_speaker_id_v2.json")
    p.add_argument("--compare-v1", action="store_true",
                   help="Compare v1 and v2 on a held-out probe set")
    args = p.parse_args()
    if args.train:
        model = train_v2()
        Path(args.out).write_text(json.dumps(model, indent=2))
        print(f"\nSaved: {args.out}")
        # Quick probe set
        probes = [
            ("yeah just sent it", True),
            ("Of course! Here are a few options", False),
            ("lol same", True),
            ("I survived fine before GPS. kind of. mostly. ok yeah I'd be lost", True),
            ("Ha, you're probably right! Technology has become...", False),
            ("damn that's actually wild", True),
            ("Certainly! I would be happy to help.", False),
        ]
        print("\nv2 probes:")
        for text, expected_seth in probes:
            s = classify_text(model, text)["p_seth"]
            tag = "SETH" if s >= 0.5 else "NOT-SETH"
            correct = "✓" if (s >= 0.5) == expected_seth else "✗"
            print(f"  {correct}  P(Seth | {text!r:<70}) = {s:.3f} ({tag})")
        return
    if args.score:
        model = json.loads(Path(args.out).read_text())
        s = classify_text(model, args.score)
        print(f"P(Seth | {args.score!r}) = {s['p_seth']:.3f}")
        return
    if args.compare_v1:
        from personaeval_speaker_id import (
            load_classifier as load_v1,
            p_seth as p_seth_v1,
        )
        v1 = load_v1("/tmp/seth_speaker_id.json")
        v2 = json.loads(Path(args.out).read_text())
        probes = [
            ("yeah just sent it", True),
            ("Of course! Here are a few options for you", False),
            ("lol same", True),
            ("I survived fine before GPS. kind of. mostly. ok yeah I'd be lost", True),
            ("Ha, you're probably right! Technology has become...", False),
            ("damn that's actually wild", True),
            ("hello. what's up", True),
            ("Hello there!! 👋 How wonderful to hear from you!", False),
            ("seriously? that's rough. happy birthday though for real", True),
            ("Oh no, I'm so sorry to hear that! Happy birthday! 🎂", False),
        ]
        print(f"{'expect':<10} {'v1 P':>7} {'v2 P':>7}  text")
        print("-" * 80)
        v1_correct = 0
        v2_correct = 0
        for text, expected in probes:
            p1 = p_seth_v1(v1, text)
            p2 = p_seth(v2, text)
            v1_correct += (p1 >= 0.5) == expected
            v2_correct += (p2 >= 0.5) == expected
            label = "SETH" if expected else "AI"
            print(f"{label:<10} {p1:>7.3f} {p2:>7.3f}  {text[:55]!r}")
        print(f"\naccuracy on probe set: v1 {v1_correct}/{len(probes)}  "
              f"v2 {v2_correct}/{len(probes)}")
        return
    p.print_help()


if __name__ == "__main__":
    main()
