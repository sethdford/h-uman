#!/usr/bin/env python3
"""
Phase D7 verifier — pins the DPO-from-rewrites consumer.

Tests:
  1. parse_pairs handles blank lines, malformed JSON, and the
     ≥3-char minimum filter
  2. summarize_pairs produces correct aggregates (counts, avg lens,
     delta range, turn kinds)
  3. export_alpaca_dpo writes the right schema (prompt/chosen/rejected)
     with the right field mapping (accepted→chosen)

Run: python3 scripts/test_m3_dpo_from_rewrites.py
"""
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DPO_MOD = REPO_ROOT / "scripts" / "m3_dpo_from_rewrites.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("m3_dpo_from_rewrites", DPO_MOD)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


dpo = _load_module()


_PASS = 0
_FAIL = 0


def _ok(name: str, cond: bool, detail: str = ""):
    global _PASS, _FAIL
    if cond:
        _PASS += 1
        print(f"  PASS  {name}")
    else:
        _FAIL += 1
        print(f"  FAIL  {name}  {detail}")


def test_parse_pairs_filters_short_and_malformed():
    print("\n--- test_parse_pairs_filters_short_and_malformed ---")
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "pairs.jsonl"
        p.write_text(
            '{"t":1,"prompt":"hello","rejected":"I am an AI","accepted":"hey"}\n'
            '\n'  # blank
            'this is not json\n'  # malformed
            '{"t":2,"prompt":"a","rejected":"b","accepted":"c"}\n'  # too short
            '{"t":3,"prompt":"morning","rejected":"As a model","accepted":"morning"}\n'
        )
        pairs = dpo.parse_pairs(p)
        _ok(f"2 valid pairs extracted (got {len(pairs)})", len(pairs) == 2)
        _ok("first pair has expected prompt",
            pairs[0]["prompt"] == "hello")


def test_summarize_pairs_correct_aggregates():
    print("\n--- test_summarize_pairs_correct_aggregates ---")
    pairs = [
        {"prompt": "10char ABC", "rejected": "long rejected response here",
         "accepted": "short", "k": 1},
        {"prompt": "20char ABCDEFGHIJ!!", "rejected": "another rejected one",
         "accepted": "ok", "k": 2},
        {"prompt": "5cha", "rejected": "5cha", "accepted": "5cha", "k": 1},  # too short
    ]
    # summarize takes pre-filtered pairs; test on all 3 even though
    # parse_pairs would've dropped the last
    s = dpo.summarize_pairs(pairs)
    _ok(f"count=3 (got {s['count']})", s["count"] == 3)
    _ok("turn_kinds has 2 entries (kinds 1, 2)", set(s["turn_kinds"].keys()) == {1, 2})
    _ok("avg prompt len is positive", s["prompt_avg_len"] > 0)
    # All 3 examples have accepted < rejected → delta should be negative
    _ok(f"delta_avg < 0 (got {s['delta_avg']})",
        s["delta_avg"] < 0, "delta should be negative for guard-shortening cases")


def test_export_alpaca_dpo_schema():
    print("\n--- test_export_alpaca_dpo_schema ---")
    with tempfile.TemporaryDirectory() as d:
        out = Path(d) / "out.jsonl"
        pairs = [
            {"prompt": "hi", "rejected": "as an AI...", "accepted": "hey there"},
            {"prompt": "yo", "rejected": "language model", "accepted": "hey"},
        ]
        dpo.export_alpaca_dpo(pairs, out)
        _ok("output exists", out.exists())
        lines = out.read_text().splitlines()
        _ok(f"2 lines written (got {len(lines)})", len(lines) == 2)
        rec0 = json.loads(lines[0])
        _ok("record has prompt+chosen+rejected keys",
            set(rec0.keys()) == {"prompt", "chosen", "rejected"})
        _ok("chosen = accepted (semantic mapping is correct)",
            rec0["chosen"] == "hey there")
        _ok("rejected stays rejected",
            rec0["rejected"] == "as an AI...")


def test_empty_input_handled_gracefully():
    print("\n--- test_empty_input_handled_gracefully ---")
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "empty.jsonl"
        p.write_text("")
        pairs = dpo.parse_pairs(p)
        _ok("empty file → empty pairs", pairs == [])
        s = dpo.summarize_pairs(pairs)
        _ok("summary of zero pairs returns count=0", s["count"] == 0)


def main():
    print("M3 DPO-from-rewrites (D7) consumer verifier")
    test_parse_pairs_filters_short_and_malformed()
    test_summarize_pairs_correct_aggregates()
    test_export_alpaca_dpo_schema()
    test_empty_input_handled_gracefully()
    print(f"\n--- Results: {_PASS} passed, {_FAIL} failed ---")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
