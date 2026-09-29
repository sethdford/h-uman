"""Who the sleep-time curator reads (spec §3). Pure: no chat.db, no model."""
import datetime as dt
import json
import sqlite3
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import curator_population as cp  # noqa: E402

NOW = dt.datetime(2026, 9, 28, 12, 0, tzinfo=dt.timezone.utc)


def msgs(them, me, days_ago=1):
    t = NOW - dt.timedelta(days=days_ago)
    return ([{"from_me": False, "t": t} for _ in range(them)] +
            [{"from_me": True, "t": t} for _ in range(me)])


def test_threshold_boundaries():
    tl = {"+15550000001": msgs(10, 5), "+15550000002": msgs(9, 5),
          "+15550000003": msgs(10, 4)}
    assert cp.eligible_handles(tl, set(), NOW) == ["+15550000001"]


def test_window_edge_excludes_old_traffic():
    tl = {"+15550000004": msgs(10, 5, days_ago=31)}
    assert cp.eligible_handles(tl, set(), NOW) == []


def test_short_codes_never_eligible_and_email_is():
    tl = {"72975": msgs(50, 50), "friend@example.com": msgs(12, 6)}
    assert cp.eligible_handles(tl, set(), NOW) == ["friend@example.com"]
    assert cp.is_short_code("72975") and not cp.is_short_code("+15550000001")


def test_persona_contacts_are_skipped_by_the_wide_pass():
    tl = {"+15550000005": msgs(30, 30)}
    assert cp.eligible_handles(tl, {"+15550000005"}, NOW) == []


def test_suppressed_table_missing_means_empty(tmp_path):
    db = tmp_path / "m.db"
    sqlite3.connect(db).close()
    assert cp.load_suppressed(str(db)) == set()


def test_suppressed_handles_are_loaded(tmp_path):
    db = tmp_path / "m.db"
    c = sqlite3.connect(db)
    c.execute("CREATE TABLE contact_suppressions (contact TEXT, ts INT, reason TEXT, excerpt TEXT)")
    c.execute("INSERT INTO contact_suppressions VALUES ('+15550000006', 1, 'r', 'x')")
    c.commit(); c.close()
    assert cp.load_suppressed(str(db)) == {"+15550000006"}


def test_never_file_missing_empty_and_malformed_refuses(tmp_path):
    assert cp.load_never(str(tmp_path / "absent.json")) == set()
    good = tmp_path / "never.json"; good.write_text(json.dumps(["+15550000007"]))
    assert cp.load_never(str(good)) == {"+15550000007"}
    bad = tmp_path / "bad.json"; bad.write_text('{"not": "a list"}')
    try:
        cp.load_never(str(bad))
        assert False, "malformed never-file must refuse"
    except ValueError:
        pass


def test_exclusion_reason_order():
    assert cp.exclusion_reason("a", {"a"}, {"a"}) == "suppressed"
    assert cp.exclusion_reason("b", set(), {"b"}) == "never"
    assert cp.exclusion_reason("c", set(), set()) is None
