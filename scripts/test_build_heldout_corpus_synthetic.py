#!/usr/bin/env python3
"""
Tests for the synthetic PII-free fixture mode of build_heldout_corpus.py
(Dermot SOTA spec T8 / AC-5).

No chat.db: exercises only generate_synthetic_prompts / is_pii_free, then
round-trips through the real corpus loader to prove the synthetic fixture is
consumable by the eval harness.
"""

import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

from build_heldout_corpus import (
    SYNTHETIC_POOL,
    generate_synthetic_prompts,
    is_pii_free,
    redact,
)
from eval_fidelity_helpers import load_held_out_prompts_from_jsonl


def test_every_pool_entry_is_pii_free():
    for entry in SYNTHETIC_POOL:
        assert is_pii_free(entry), f"pool entry contains PII patterns: {entry!r}"
    assert len(SYNTHETIC_POOL) >= 20, "pool must cover the 20-prompt eval floor"
    print(f"✓ all {len(SYNTHETIC_POOL)} pool entries are PII-free (>=20 floor)")


def test_is_pii_free_detects_real_pii():
    """The check must FAIL on the patterns the real sampler redacts — proving it
    isn't a no-op."""
    assert is_pii_free("call me at +1 555 123 4567") is False, "phone not caught"
    assert is_pii_free("Mindy Ford said hi") is False, "two-cap name not caught"
    assert is_pii_free("see http://example.com/x") is False, "url not caught"
    # And a clean casual line passes.
    assert is_pii_free("wanna grab food later?") is True
    print("✓ is_pii_free catches phone/name/url, passes clean banter")


def test_generate_synthetic_prompts_shape_and_count():
    prompts = generate_synthetic_prompts(20, seed=1)
    assert len(prompts) == 20
    for d in prompts:
        assert set(d.keys()) >= {"prompt", "channel", "context", "source"}
        assert d["source"] == "synthetic_pii_free"
        assert d["channel"] == "imessage"
        assert is_pii_free(d["prompt"])
    # distinct prompts (no accidental dupes within a draw)
    assert len({d["prompt"] for d in prompts}) == 20
    print("✓ generate_synthetic_prompts(20): 20 distinct PII-free dicts, right schema")


def test_deterministic_for_seed():
    a = [d["prompt"] for d in generate_synthetic_prompts(10, seed=7)]
    b = [d["prompt"] for d in generate_synthetic_prompts(10, seed=7)]
    assert a == b, "same seed must yield same order"
    c = [d["prompt"] for d in generate_synthetic_prompts(10, seed=8)]
    assert a != c or len(SYNTHETIC_POOL) <= 10, "different seed should reshuffle"
    print("✓ synthetic generation deterministic per seed")


def test_cap_at_pool_size():
    prompts = generate_synthetic_prompts(9999)
    assert len(prompts) == len(SYNTHETIC_POOL), "must cap at pool size, not invent PII risk"
    print(f"✓ count caps at pool size ({len(SYNTHETIC_POOL)})")


def test_roundtrip_through_corpus_loader():
    """The synthetic fixture written to JSONL must load back via the SAME loader
    the eval harness uses."""
    prompts = generate_synthetic_prompts(20)
    with tempfile.TemporaryDirectory() as td:
        path = Path(td) / "heldout-synthetic.jsonl"
        path.write_text("\n".join(json.dumps(d) for d in prompts) + "\n")
        loaded = load_held_out_prompts_from_jsonl(str(path))
        assert len(loaded) == 20, f"loader returned {len(loaded)} of 20"
        # Every loaded prompt is still PII-free after the round trip.
        for p in loaded:
            text = p["prompt"] if isinstance(p, dict) else p
            assert redact(text) == text.strip()
    print("✓ synthetic fixture round-trips through the eval harness loader, PII-free")


def main():
    tests = [
        test_every_pool_entry_is_pii_free,
        test_is_pii_free_detects_real_pii,
        test_generate_synthetic_prompts_shape_and_count,
        test_deterministic_for_seed,
        test_cap_at_pool_size,
        test_roundtrip_through_corpus_loader,
    ]
    print("=" * 60)
    print("Testing build_heldout_corpus.py (synthetic mode)")
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
