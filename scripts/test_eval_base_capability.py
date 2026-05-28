#!/usr/bin/env python3
"""
Tests for eval_base_capability.py — the deterministic base-capability scorer
that guards the personalization gate's safety axis (AC-3).

No model is invoked: responses are supplied directly, so every checker branch
is pinned in isolation.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from eval_base_capability import (
    check_probe,
    load_probes,
    probes_sha256,
    score_base_capability,
)


def test_json_keys_checker():
    assert check_probe({"type": "json_keys", "keys": ["name", "city"]},
                       '{"name": "Ada", "city": "Dublin"}') is True
    assert check_probe({"type": "json_keys", "keys": ["name", "city"]},
                       '{"name": "Ada"}') is False
    print("✓ json_keys: full keys pass, missing key fails")


def test_json_keys_tolerates_code_fence_and_prose():
    fenced = '```json\n{"active": true}\n```'
    assert check_probe({"type": "json_keys", "keys": ["active"]}, fenced) is True
    embedded = 'Here you go: {"active": true} hope that helps'
    assert check_probe({"type": "json_keys", "keys": ["active"]}, embedded) is True
    print("✓ json_keys: tolerates code fence + surrounding prose")


def test_json_equals_checker():
    assert check_probe({"type": "json_equals", "value": [1, 2, 3]}, "[1, 2, 3]") is True
    assert check_probe({"type": "json_equals", "value": [1, 2, 3]}, "[3, 1, 2]") is False
    print("✓ json_equals: exact array match passes, wrong order fails")


def test_numeric_equals_checker():
    assert check_probe({"type": "numeric_equals", "value": 42}, "42") is True
    assert check_probe({"type": "numeric_equals", "value": 42}, "The answer is 42.") is True
    assert check_probe({"type": "numeric_equals", "value": 42}, "1,234 then 42") is False  # first num wins
    assert check_probe({"type": "numeric_equals", "value": 144}, "12 * 12 = 144") is False  # first num is 12
    assert check_probe({"type": "numeric_equals", "value": 42}, "forty-two") is False
    print("✓ numeric_equals: first-number match, prose-tolerant, word-form fails")


def test_regex_checkers_case_sensitivity():
    assert check_probe({"type": "regex", "pattern": "HELLO"}, "HELLO") is True
    assert check_probe({"type": "regex", "pattern": "HELLO"}, "hello") is False
    assert check_probe({"type": "regex_i", "pattern": "merci"}, "Merci beaucoup") is True
    assert check_probe({"type": "regex_i", "pattern": "^\\W*tokyo\\W*$"}, "Tokyo.") is True
    assert check_probe({"type": "regex_i", "pattern": "^\\W*tokyo\\W*$"}, "The capital is Tokyo") is False
    print("✓ regex case-sensitive, regex_i case-insensitive + anchors")


def test_exact_checker():
    assert check_probe({"type": "exact", "value": "yes"}, "  YES  ") is True
    assert check_probe({"type": "exact", "value": "yes"}, "yes please") is False
    print("✓ exact: trim+casefold equal passes, extra text fails")


def test_error_markers_fail_closed():
    for marker in ("[timeout]", "[empty]", "[gen_err: boom]"):
        assert check_probe({"type": "regex", "pattern": ".*"}, marker) is False, marker
    print("✓ error markers ([timeout]/[empty]/[gen_err]) fail closed")


def test_json_array_answer_not_misread_as_error_marker():
    """A legitimate JSON-array answer starts with '[' but must NOT be treated
    as an error marker."""
    assert check_probe({"type": "json_equals", "value": [1, 2, 3]}, "[1, 2, 3]") is True
    print("✓ JSON-array answer not confused with [error] markers")


def test_unknown_checker_fails_closed():
    assert check_probe({"type": "telepathy"}, "anything") is False
    print("✓ unknown checker type fails closed")


def test_score_mean_and_alignment():
    probes = [
        {"id": "a", "check": {"type": "numeric_equals", "value": 1}},
        {"id": "b", "check": {"type": "numeric_equals", "value": 2}},
        {"id": "c", "check": {"type": "numeric_equals", "value": 3}},
        {"id": "d", "check": {"type": "numeric_equals", "value": 4}},
    ]
    per_probe, mean = score_base_capability(["1", "99", "3", "4"], probes)
    assert mean == 0.75, f"expected 0.75, got {mean}"
    assert per_probe[1]["passed"] is False and per_probe[1]["id"] == "b"
    print(f"✓ score mean + per-probe alignment (0.75)")


def test_short_responses_count_missing_as_fail():
    probes = [
        {"id": "a", "check": {"type": "numeric_equals", "value": 1}},
        {"id": "b", "check": {"type": "numeric_equals", "value": 2}},
    ]
    per_probe, mean = score_base_capability(["1"], probes)  # missing b
    assert mean == 0.5 and per_probe[1]["passed"] is False
    print("✓ missing responses count as failures")


def test_frozen_probe_set_loads_and_hashes_stable():
    probes = load_probes()
    assert len(probes) >= 10, f"expected >=10 frozen probes, got {len(probes)}"
    for p in probes:
        assert "id" in p and "prompt" in p and "check" in p, f"malformed probe: {p}"
        assert p["check"]["type"] in (
            "json_keys", "json_equals", "numeric_equals", "exact", "regex", "regex_i"
        ), f"unknown checker in frozen set: {p['check']}"
    h1, h2 = probes_sha256(), probes_sha256()
    assert h1 == h2 and len(h1) == 64, "probe-set hash must be stable 64-hex"
    print(f"✓ frozen probe set: {len(probes)} probes, stable sha256 {h1[:12]}…")


def test_frozen_set_scores_perfectly_on_ideal_answers():
    """Sanity: ideal answers to the actual frozen probes score 1.0, proving the
    checkers and the prompts are mutually consistent."""
    ideal = {
        "json_extract_city": '{"name": "Ada", "city": "Dublin"}',
        "json_array_numbers": "[1, 2, 3]",
        "translate_fr_thanks": "Merci",
        "translate_es_house": "la casa",
        "arith_sum": "42",
        "arith_mult": "144",
        "sort_words": "apple, banana, cherry",
        "one_word_capital": "Tokyo",
        "yes_no_boolean": "yes",
        "extract_email": "sam@example.com",
        "count_words": "4",
        "uppercase_word": "HELLO",
        "json_bool_field": '{"active": true}',
        "last_of_list": "blue",
        "subtract": "63",
    }
    probes = load_probes()
    responses = [ideal[p["id"]] for p in probes]
    per_probe, mean = score_base_capability(responses, probes)
    failed = [pp["id"] for pp in per_probe if not pp["passed"]]
    assert mean == 1.0, f"ideal answers must score 1.0; failed: {failed}"
    print("✓ ideal answers to the frozen set score 1.0 (prompts↔checkers consistent)")


def main():
    tests = [
        test_json_keys_checker,
        test_json_keys_tolerates_code_fence_and_prose,
        test_json_equals_checker,
        test_numeric_equals_checker,
        test_regex_checkers_case_sensitivity,
        test_exact_checker,
        test_error_markers_fail_closed,
        test_json_array_answer_not_misread_as_error_marker,
        test_unknown_checker_fails_closed,
        test_score_mean_and_alignment,
        test_short_responses_count_missing_as_fail,
        test_frozen_probe_set_loads_and_hashes_stable,
        test_frozen_set_scores_perfectly_on_ideal_answers,
    ]
    print("=" * 60)
    print("Testing eval_base_capability.py")
    print("=" * 60)
    passed = failed = 0
    for t in tests:
        try:
            t()
            passed += 1
        except AssertionError as e:
            print(f"✗ {t.__name__}: {e}")
            failed += 1
        except Exception as e:
            print(f"✗ {t.__name__}: {type(e).__name__}: {e}")
            failed += 1
    print("=" * 60)
    print(f"Results: {passed} passed, {failed} failed")
    print("=" * 60)
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
