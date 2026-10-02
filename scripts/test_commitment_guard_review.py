#!/usr/bin/env python3
"""Hermetic tests for scripts/commitment_guard_review.py: a temp log and a temp
chat.db-shaped sqlite file; never the real chat.db."""
import io
import json
import os
import sqlite3
import sys
import tempfile
from contextlib import redirect_stdout
from datetime import datetime

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import commitment_guard_review as r  # noqa: E402

TS = "2026-10-02T19:00:05"
LINES = [
    f"{TS} INFO  [commitment_guard] [HU_COMMITMENT_GUARD shadow] prefilter=1 audit=0 "
    "detector=ok kind=plan stakes=low when=1 conf=0.90 calendar=busy decision=rewrite_conflict\n",
    f"{TS} INFO  [commitment_guard] [HU_COMMITMENT_GUARD shadow] prefilter=1 audit=0 "
    "detector=ok kind=none stakes=- when=0 conf=0.95 calendar=n/a decision=allow\n",
    f"{TS} INFO  [commitment_guard] [HU_COMMITMENT_GUARD live] prefilter=1 audit=0 "
    "detector=ok kind=money stakes=high when=0 conf=0.90 calendar=n/a decision=hold\n",
    "unrelated line\n",
]


def test_parse_keeps_only_detected_shadow_events():
    ev = r.parse_events(LINES)
    assert len(ev) == 1
    assert ev[0]["kind"] == "plan" and ev[0]["decision"] == "rewrite_conflict"


def test_tally_reports_none_not_zero_on_empty():
    assert r.tally([]) == {}
    t = r.tally([("plan", True), ("plan", False), ("money", True)])
    assert t["plan"]["precision"] == 0.5 and t["all"]["yes"] == 2


def test_end_to_end_pairs_event_with_sent_message():
    with tempfile.TemporaryDirectory() as d:
        log = os.path.join(d, "service-loop.log")
        with open(log, "w") as f:
            f.writelines(LINES)
        db_path = os.path.join(d, "chat.db")
        db = sqlite3.connect(db_path)
        db.execute("CREATE TABLE message (text TEXT, attributedBody BLOB, is_from_me INT, date INT)")
        sent = datetime.strptime(TS, "%Y-%m-%dT%H:%M:%S").timestamp() + 20
        db.execute("INSERT INTO message VALUES (?, NULL, 1, ?)",
                   ("yeah saturday works", int((sent - r.APPLE_EPOCH) * 1e9)))
        db.commit()
        db.close()
        out = os.path.join(d, "tally.json")
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = r.main(["--log", log, "--chatdb", db_path, "--answers", "y", "--out", out])
        assert rc == 0
        res = json.load(open(out))
        assert res["labelled"] == 1 and res["by_kind"]["plan"]["precision"] == 1.0
        assert "yeah saturday works" not in open(out).read()  # tally carries no text


def test_no_events_is_not_a_result():
    with tempfile.TemporaryDirectory() as d:
        log = os.path.join(d, "x.log")
        open(log, "w").write("nothing here\n")
        assert r.main(["--log", log, "--chatdb", os.path.join(d, "none.db")]) == 2


if __name__ == "__main__":
    failed = 0
    for name, fn in sorted(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"PASS {name}")
            except Exception as e:  # noqa: BLE001
                failed += 1
                print(f"FAIL {name}: {e!r}")
    sys.exit(1 if failed else 0)
