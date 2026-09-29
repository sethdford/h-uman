"""scripts/insight_overuse_report.py — parity with the C counter and the aggregate.

The report compares the twin (daemon log lines) with Seth's own replies scored
by a Python mirror of hu_insight_overuse_count. If the two tokenizers drift the
comparison is meaningless, so the C test's fixture is pinned here with the
same expected numbers (tests/test_daemon_insight_overuse.c).
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))

import insight_overuse_report as rep  # noqa: E402

BLOCK = ("- her sister mindy lives in tampa (as of Sep 2026)\n"
         "- planning a cabo trip in november (as of Aug 2026)\n"
         "- hates cilantro, loves the taco truck on central (as of Jul 2026)\n")


def test_count_matches_the_c_fixture():
    inbound = "how's mindy doing? we should do tacos this week"
    reply = "mindy's good, still in tampa. and yes the taco truck on central, cabo talk after"
    assert rep.count(BLOCK, inbound, reply) == (14, 6, 1)


def test_count_is_whole_word_case_folded_and_skips_digits_and_stop_words():
    assert rep.count("- got a new mac for work (as of Sep 2026)", "", "my stomach hurts. WORK was fine") == (1, 1, 0)
    assert rep.count("- tampa tampa tampa, really just about tampa (as of Sep 2026)", "", "tampa!") == (1, 1, 0)
    assert rep.count("", "x", "y") == (0, 0, 0)


def test_parse_log_and_aggregate():
    lines = [
        "2026-09-19T20:01:02 INFO  [insight-overuse] shadow: injected=14 surfaced=6 prompted=1 unprompted=5 for +15550000001",
        "unrelated line",
        "2026-09-19T20:05:00 INFO  [insight-overuse] shadow: injected=0 surfaced=0 prompted=0 unprompted=0 for +15550000002",
        "2026-09-19T20:09:00 INFO  [insight-overuse] shadow: injected=10 surfaced=2 prompted=2 unprompted=0 for +15550000001",
    ]
    rows = rep.parse_log(lines)
    assert [r["contact"] for r in rows] == ["+15550000001", "+15550000002", "+15550000001"]
    agg = rep.aggregate(rows)
    assert agg["n"] == 3 and agg["n_with_block"] == 2
    assert agg["unprompted_per_reply"] == round(5 / 3, 3)
    assert agg["unprompted_rate"] == round(5 / 24, 4)  # (5+0) unprompted over 14+10 injected
    assert agg["share_replies_with_unprompted"] == round(1 / 3, 3)
    assert rep.aggregate([]) == {"n": 0}
