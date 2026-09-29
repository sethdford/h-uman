"""Reference replies stay out of every export until Seth rates them good."""
import csv
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import gold_export, gold_rate, store  # noqa: E402


def seeded():
    s = store.open_store(":memory:")
    for i in range(3):
        store.add_reference(s, "+1a", i, f"ctx {i}", f"reply {i}", "g@local", "reference-v1", 1)
    return s


def test_unrated_replies_are_never_exported(tmp_path):
    s = seeded()
    out = tmp_path / "export.csv"
    assert gold_export.export_rated(s, str(out)) == 0
    assert list(csv.DictReader(open(out))) == []


def test_rate_then_export_only_good(tmp_path):
    s = seeded()
    sheet = tmp_path / "rate.csv"
    assert gold_rate.write_rating_sheet(s, str(sheet)) == 3
    rows = list(csv.DictReader(open(sheet)))
    rows[0]["good"], rows[1]["good"], rows[2]["good"] = "y", "n", "maybe"
    with open(sheet, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["id", "context", "reply", "good"])
        w.writeheader()
        w.writerows(rows)
    assert gold_rate.import_ratings(s, str(sheet)) == {"rated_good": 1, "rated_bad": 1,
                                                       "skipped": 1}
    out = tmp_path / "export.csv"
    assert gold_export.export_rated(s, str(out)) == 1
    assert [r["reply"] for r in csv.DictReader(open(out))] == [rows[0]["reply"]]
    assert gold_rate.write_rating_sheet(s, str(sheet)) == 1  # only the skipped one remains
