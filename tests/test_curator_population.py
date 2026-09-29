"""Who the sleep-time curator reads (spec §3). Pure: no chat.db, no model."""
import datetime as dt
import json
import sqlite3
import sys
from pathlib import Path

import pytest

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


def test_window_day_30_is_inclusive():
    tl = {"+15550000009": msgs(10, 5, days_ago=30)}
    assert cp.eligible_handles(tl, set(), NOW) == ["+15550000009"]


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


def test_suppressed_unreadable_db_refuses(tmp_path):
    with pytest.raises(sqlite3.Error):
        cp.load_suppressed(str(tmp_path / "nope" / "m.db"))


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


def test_only_e164_or_email_handles_are_eligible():
    """R12 m1: spec §3 -- the handle is +E.164 or an email."""
    tl = {"urn:biz:x": msgs(12, 6), "5551234": msgs(12, 6), "+15550000010": msgs(12, 6),
          "a@b.co": msgs(12, 6), "+123456": msgs(12, 6)}
    assert cp.eligible_handles(tl, set(), NOW) == ["+15550000010", "a@b.co"]


def test_normalize_handle_phones_and_emails():
    assert cp.normalize_handle("5550000042") == "+15550000042"
    assert cp.normalize_handle("(555) 000-0042") == "+15550000042"
    assert cp.normalize_handle("15550000042") == "+15550000042"
    assert cp.normalize_handle("+44 20 7946 0958") == "+442079460958"
    assert cp.normalize_handle("Friend@Example.COM") == "friend@example.com"


def test_never_entries_match_normalized_handles(tmp_path):
    """R12 I6: a never-file written by hand ("5550000042", mixed-case email)
    must still exclude the chat.db handle it means."""
    p = tmp_path / "never.json"
    p.write_text(json.dumps(["5550000042", "Friend@Example.com"]))
    never = cp.load_never(str(p))
    assert cp.exclusion_reason("+15550000042", set(), never) == "never"
    assert cp.exclusion_reason("friend@example.com", set(), never) == "never"
    assert cp.exclusion_reason("+15550000043", set(), never) is None


def test_unmatched_never_entries_are_counted():
    assert cp.unmatched_never({"5550000042", "nobody@x.com"}, ["+15550000042", "a@b.co"]) == 1
    assert cp.unmatched_never(set(), ["+15550000042"]) == 0


def test_loopback_handles_are_excluded_from_eligibility():
    """R12 I7: the daemon's own loopback handle is never a contact."""
    tl = {"+15550000011": msgs(12, 6), "+15550000012": msgs(12, 6)}
    assert cp.eligible_handles(tl, set(), NOW, exclude={"5550000011"}) == ["+15550000012"]


def test_load_loopback_handles_str_list_missing(tmp_path):
    cfg = tmp_path / "config.json"
    assert cp.load_loopback_handles(str(cfg)) == set()
    cfg.write_text(json.dumps({"channels": {"imessage": {"loopback_handle": "5550000011"}}}))
    assert cp.load_loopback_handles(str(cfg)) == {"+15550000011"}
    cfg.write_text(json.dumps({"channels": {"imessage": {"loopback_handle":
                                                         ["+15550000011", "Me@X.com"]}}}))
    assert cp.load_loopback_handles(str(cfg)) == {"+15550000011", "me@x.com"}
    cfg.write_text(json.dumps({"channels": {"imessage": {}}}))
    assert cp.load_loopback_handles(str(cfg)) == set()
