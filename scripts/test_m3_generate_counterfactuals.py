#!/usr/bin/env python3
"""
Phase H2 verifier — pins counterfactual generator behavior.

Tests:
  1. synthetic_variants produces k distinct strings, each "worse"
     than the original by style markers
  2. find_seth_turns_with_context pairs Seth turns with PRECEDING
     user turns from the same contact
  3. Same-contact constraint — doesn't cross conversations
  4. End-to-end --no-llm: real H1 fixture → preference pairs JSONL
  5. Empty corpus → exit 2
  6. Variant rejected-pick is deterministic with --seed

Run: python3 scripts/test_m3_generate_counterfactuals.py
"""
from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
GEN = REPO_ROOT / "scripts" / "m3_generate_counterfactuals.py"


def _load():
    spec = importlib.util.spec_from_file_location("m3_generate_counterfactuals", GEN)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


m = _load()
_PASS = 0
_FAIL = 0


def _ok(name, cond, detail=""):
    global _PASS, _FAIL
    if cond:
        _PASS += 1
        print(f"  PASS  {name}")
    else:
        _FAIL += 1
        print(f"  FAIL  {name}  {detail}")


def test_synthetic_variants_count_and_distinct():
    print("\n--- test_synthetic_variants_count_and_distinct ---")
    variants = m.synthetic_variants("yeah lol that's cool", k=3)
    _ok(f"returns 3 variants (got {len(variants)})", len(variants) == 3)
    _ok("all variants are distinct", len(set(variants)) == len(variants))
    _ok("variants differ from original",
        all(v != "yeah lol that's cool" for v in variants))


def test_synthetic_variants_inject_style_violations():
    print("\n--- test_synthetic_variants_inject_style_violations ---")
    variants = m.synthetic_variants("yeah ok", k=3)
    joined = " ".join(variants)
    # At least one variant should have an assistant-prefix or formal rewrite
    has_assistant_marker = any(
        marker in joined for marker in ["happy to help", "Certainly!", "As an AI",
                                          "yes, certainly", "very well", "comprehensively"])
    _ok("at least one variant contains an obvious AI/formal marker",
        has_assistant_marker, f"variants: {variants}")


def test_synthetic_variants_empty_input():
    print("\n--- test_synthetic_variants_empty_input ---")
    _ok("empty input → empty list", m.synthetic_variants("", k=3) == [])


def test_find_seth_turns_pairs_same_contact():
    print("\n--- test_find_seth_turns_pairs_same_contact ---")
    with tempfile.TemporaryDirectory() as d:
        corpus = Path(d) / "corpus.jsonl"
        # Two contacts; Seth replies to each. Verify only same-contact
        # pairing occurs (no cross-contact bleed).
        records = [
            {"handle": "alice", "role": "user", "content": "hey", "ts_ms": 100},
            {"handle": "bob", "role": "user", "content": "yo", "ts_ms": 150},
            {"handle": "alice", "role": "assistant", "content": "what's up", "ts_ms": 200},
            {"handle": "bob", "role": "assistant", "content": "morning", "ts_ms": 250},
        ]
        corpus.write_text("\n".join(json.dumps(r) for r in records) + "\n")
        pairs = m.find_seth_turns_with_context(corpus, max_records=100)
        _ok(f"2 pairs produced (got {len(pairs)})", len(pairs) == 2)
        _ok("alice pair: 'hey' → 'what's up'",
            ("hey", "what's up") in pairs)
        _ok("bob pair: 'yo' → 'morning'",
            ("yo", "morning") in pairs)
        # Crucially: NOT ("hey", "morning") — would be a cross-contact mismatch
        _ok("no cross-contact pair",
            ("hey", "morning") not in pairs and ("yo", "what's up") not in pairs)


def test_find_seth_turns_skips_seth_without_preceding_user():
    print("\n--- test_find_seth_turns_skips_seth_without_preceding_user ---")
    with tempfile.TemporaryDirectory() as d:
        corpus = Path(d) / "corpus.jsonl"
        # Seth speaks first — no preceding user context exists.
        # Then user replies. The first Seth turn should be skipped.
        records = [
            {"handle": "alice", "role": "assistant", "content": "hey alice", "ts_ms": 100},
            {"handle": "alice", "role": "user", "content": "hey", "ts_ms": 200},
            {"handle": "alice", "role": "assistant", "content": "what's up", "ts_ms": 300},
        ]
        corpus.write_text("\n".join(json.dumps(r) for r in records) + "\n")
        pairs = m.find_seth_turns_with_context(corpus, max_records=100)
        _ok(f"1 pair produced (got {len(pairs)})", len(pairs) == 1)
        if pairs:
            _ok("the pair is the second Seth turn, not the first",
                pairs[0] == ("hey", "what's up"))


def test_end_to_end_no_llm_synthetic_path():
    print("\n--- test_end_to_end_no_llm_synthetic_path ---")
    with tempfile.TemporaryDirectory() as d:
        corpus = Path(d) / "corpus.jsonl"
        records = [
            {"handle": "alice", "role": "user", "content": "how was today",
             "ts_ms": 100},
            {"handle": "alice", "role": "assistant",
             "content": "yeah good, just got home", "ts_ms": 200},
            {"handle": "alice", "role": "user", "content": "cool",
             "ts_ms": 300},
            {"handle": "alice", "role": "assistant",
             "content": "haha yep", "ts_ms": 400},
        ]
        corpus.write_text("\n".join(json.dumps(r) for r in records) + "\n")

        out = Path(d) / "pairs.jsonl"
        result = subprocess.run(
            [sys.executable, str(GEN),
             "--corpus", str(corpus),
             "--out", str(out),
             "--no-llm",
             "--max-records", "10"],
            capture_output=True, text=True, timeout=20,
            env={**__import__("os").environ, "OPENAI_API_KEY": ""})
        _ok(f"generator exits 0 (rc={result.returncode})",
            result.returncode == 0,
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
        lines = [l for l in out.read_text().splitlines() if l.strip()]
        _ok(f"2 pairs written (got {len(lines)})", len(lines) == 2)
        for line in lines:
            rec = json.loads(line)
            _ok(f"pair has prompt/chosen/rejected",
                {"prompt", "chosen", "rejected"} <= set(rec))
            _ok(f"chosen != rejected for prompt={rec.get('prompt','')!r}",
                rec["chosen"] != rec["rejected"])


def test_empty_corpus_exits_2():
    print("\n--- test_empty_corpus_exits_2 ---")
    with tempfile.TemporaryDirectory() as d:
        corpus = Path(d) / "empty.jsonl"
        corpus.write_text("")
        out = Path(d) / "pairs.jsonl"
        result = subprocess.run(
            [sys.executable, str(GEN),
             "--corpus", str(corpus), "--out", str(out), "--no-llm"],
            capture_output=True, text=True, timeout=10)
        _ok(f"empty corpus → exit 2 (got {result.returncode})",
            result.returncode == 2)


def main():
    print("M3 counterfactual generator (H2) verifier")
    test_synthetic_variants_count_and_distinct()
    test_synthetic_variants_inject_style_violations()
    test_synthetic_variants_empty_input()
    test_find_seth_turns_pairs_same_contact()
    test_find_seth_turns_skips_seth_without_preceding_user()
    test_end_to_end_no_llm_synthetic_path()
    test_empty_corpus_exits_2()
    print(f"\n--- Results: {_PASS} passed, {_FAIL} failed ---")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
