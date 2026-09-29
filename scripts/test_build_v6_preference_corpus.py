#!/usr/bin/env python3
"""Hermetic tests for the emoji-neutralisation step in
scripts/build_v6_preference_corpus.py. Synthetic rows only — no memory.db,
no rated sheet, no cycle-4 triples.

WHY (2026-09-05): the v6 / v6.1 preference corpora had emoji in 0.5% of
chosen rows and 7.3% of rejected rows (all of it from the synthetic
generated_v2 source, where the "generic assistant" rejected side used emoji
and the terse chosen side did not). ORPO learned "emoji => rejected" and the
served adapter emitted 0 emoji in 68/68 replies against Seth's measured
12.6%. Emoji must not be a chosen/rejected discriminator.
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import build_v6_preference_corpus as b  # noqa: E402


def _row(chosen, rejected, src="synthetic"):
    return {"prompt": "ctx", "chosen": chosen, "rejected": rejected, "_src": src, "_id": ""}


def test_strip_emoji_removes_emoji_and_tidies_whitespace():
    assert b.strip_emoji("sounds good 🙂") == "sounds good"
    assert b.strip_emoji("🔥 lets go 🔥🔥") == "lets go"
    assert b.strip_emoji("on my way 🚗 be there soon") == "on my way be there soon"


def test_strip_emoji_keeps_words_punctuation_and_non_emoji_symbols():
    assert b.strip_emoji("ok! see you at 5?") == "ok! see you at 5?"
    assert b.strip_emoji("costs $40 + tax") == "costs $40 + tax"
    assert b.strip_emoji("") == ""


def test_strip_emoji_handles_zwj_sequences_and_variation_selectors():
    # family ZWJ sequence + a keycap/variation-selector emoji
    assert b.strip_emoji("us 👨‍👩‍👧 later") == "us later"
    assert b.strip_emoji("done ✔️") == "done"


def test_neutralize_strips_rejected_only_emoji():
    rows = [_row("yeah works for me", "I'd be happy to help! 😊")]
    n = b.neutralize_emoji_pairs(rows)
    assert n == 1
    assert rows[0]["rejected"] == "I'd be happy to help!"
    assert rows[0]["chosen"] == "yeah works for me"


def test_neutralize_keeps_chosen_emoji_and_both_sided_emoji():
    rows = [_row("miss you 🙂", "I miss you too."),          # chosen-only: Seth's real emoji stays
            _row("lol 😂", "haha that is funny 😂")]        # both sides: not a discriminator, untouched
    n = b.neutralize_emoji_pairs(rows)
    assert n == 0
    assert rows[0]["chosen"] == "miss you 🙂" and rows[0]["rejected"] == "I miss you too."
    assert rows[1]["chosen"] == "lol 😂" and rows[1]["rejected"] == "haha that is funny 😂"


def test_neutralize_never_empties_or_collapses_a_pair():
    # A rejected reply that is ONLY emoji would become empty -> row must be dropped, not emptied.
    rows = [_row("sure", "👍"), _row("k", "k 👍")]  # second would collapse to chosen == rejected
    n = b.neutralize_emoji_pairs(rows)
    assert n == 2
    assert rows == [] or all(r["rejected"] and r["rejected"] != r["chosen"] for r in rows)
    assert len(rows) == 0


def test_emoji_stats_reports_both_sides():
    rows = [_row("a 🙂", "b"), _row("c", "d 😂"), _row("e", "f")]
    s = b.emoji_stats(rows)
    assert s == {"n": 3, "chosen_rate": 1 / 3, "rejected_rate": 1 / 3,
                 "rejected_only": 1, "chosen_only": 1, "margin": 0.0}


def test_validate_refuses_when_rejected_emoji_margin_survives():
    rows = [_row("c%d" % i, "r%d 😊" % i) for i in range(50)] + [_row("x", "y")]
    try:
        b.validate(rows, floor=1)
    except SystemExit as e:
        assert "emoji" in str(e)
    else:
        raise AssertionError("validate must refuse a corpus where emoji still marks the rejected side")


def test_validate_passes_after_neutralisation():
    rows = [_row("c%d" % i, "r%d 😊" % i) for i in range(50)] + [_row("x 🙂", "y")]
    b.neutralize_emoji_pairs(rows)
    b.validate(rows, floor=1)  # must not raise


if __name__ == "__main__":
    import pytest
    sys.exit(pytest.main([__file__, "-v"]))
