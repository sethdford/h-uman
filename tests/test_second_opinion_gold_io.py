"""Reference replies stay out of every export until Seth rates them good."""
import csv
import sqlite3
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import gold_export, gold_rate, store  # noqa: E402


def seeded():
    s = store.open_store(":memory:")
    for i in range(3):
        store.add_reference(s, "+1a", i, f"ctx {i}", f"reply {i}", "g@local", "reference-v1", 1)
    return s


def write_sheet(sheet, rows):
    with open(sheet, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=["id", "context", "reply", "good"])
        w.writeheader()
        w.writerows(rows)


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
    write_sheet(sheet, rows)
    assert gold_rate.import_ratings(s, str(sheet)) == {"rated_good": 1, "rated_bad": 1,
                                                       "skipped": 1, "unknown_ids": 0}
    out = tmp_path / "export.csv"
    assert gold_export.export_rated(s, str(out)) == 1
    assert [r["reply"] for r in csv.DictReader(open(out))] == [rows[0]["reply"]]
    assert gold_rate.write_rating_sheet(s, str(sheet)) == 1  # only the skipped one remains


def test_write_rating_sheet_escapes_formula_prefixed_context_and_reply(tmp_path):
    s = store.open_store(":memory:")
    store.add_reference(s, "+1a", 0, "=cmd|'/bin/calc'!A0", "=HYPERLINK(\"http://evil\")",
                        "g@local", "reference-v1", 1)
    store.add_reference(s, "+1a", 1, "normal context", "normal reply", "g@local",
                        "reference-v1", 2)
    sheet = tmp_path / "rate.csv"
    assert gold_rate.write_rating_sheet(s, str(sheet)) == 2
    rows = list(csv.DictReader(open(sheet)))
    assert rows[0]["context"] == "'=cmd|'/bin/calc'!A0"
    assert rows[0]["reply"] == "'=HYPERLINK(\"http://evil\")"
    assert rows[1]["context"] == "normal context" and rows[1]["reply"] == "normal reply"


def test_import_ratings_accepts_shared_yes_no_words_case_insensitively(tmp_path):
    s = seeded()
    sheet = tmp_path / "rate.csv"
    gold_rate.write_rating_sheet(s, str(sheet))
    rows = list(csv.DictReader(open(sheet)))
    rows[0]["good"], rows[1]["good"], rows[2]["good"] = "Yes", "No", ""
    write_sheet(sheet, rows)
    assert gold_rate.import_ratings(s, str(sheet)) == {"rated_good": 1, "rated_bad": 1,
                                                       "skipped": 1, "unknown_ids": 0}


def test_import_ratings_reports_unknown_id_and_changes_nothing(tmp_path):
    s = seeded()
    sheet = tmp_path / "rate.csv"
    write_sheet(sheet, [{"id": "9999", "context": "c", "reply": "r", "good": "y"}])
    result = gold_rate.import_ratings(s, str(sheet))
    assert result == {"rated_good": 0, "rated_bad": 0, "skipped": 0, "unknown_ids": 1}
    assert [r[0] for r in s.execute("SELECT rated FROM reference_replies")] == [None, None, None]


def test_import_ratings_skips_non_integer_id_but_applies_valid_rows(tmp_path):
    s = seeded()
    sheet = tmp_path / "rate.csv"
    gold_rate.write_rating_sheet(s, str(sheet))
    rows = list(csv.DictReader(open(sheet)))
    rows[0]["good"] = "y"
    rows[0]["id"] = "abc"                # corrupt id, otherwise-valid rating
    rows[1]["good"] = "n"
    write_sheet(sheet, rows)
    result = gold_rate.import_ratings(s, str(sheet))
    assert result == {"rated_good": 0, "rated_bad": 1, "skipped": 2, "unknown_ids": 0}
    assert dict(s.execute("SELECT id, rated FROM reference_replies"))[int(rows[1]["id"])] == 0


def test_import_ratings_rolls_back_everything_on_unexpected_error(tmp_path):
    s = seeded()
    sheet = tmp_path / "rate.csv"
    gold_rate.write_rating_sheet(s, str(sheet))
    rows = list(csv.DictReader(open(sheet)))
    rows[0]["good"], rows[1]["good"], rows[2]["good"] = "y", "n", "y"
    write_sheet(sheet, rows)

    class FlakyConn:
        """Wraps the real connection; the SECOND UPDATE raises, simulating a
        mid-import failure (disk full, corrupt row, etc.)."""

        def __init__(self, real):
            self._real, self._updates = real, 0

        def execute(self, sql, params=()):
            if sql.strip().upper().startswith("UPDATE"):
                self._updates += 1
                if self._updates == 2:
                    raise sqlite3.OperationalError("simulated failure")
            return self._real.execute(sql, params)

        def commit(self):
            self._real.commit()

        def rollback(self):
            self._real.rollback()

    with pytest.raises(sqlite3.OperationalError):
        gold_rate.import_ratings(FlakyConn(s), str(sheet))

    # Someone else's LATER commit on the same underlying connection must not
    # resurrect a partial import: the rollback already happened inside
    # import_ratings, so nothing is left staged to commit.
    s.commit()
    assert [r[0] for r in s.execute("SELECT rated FROM reference_replies ORDER BY id")] == \
        [None, None, None]


# ---------------------------------------------------------------------------
# Final-review fix round: I3 — human-facing CSVs default under
# ~/.human/second_opinion/ (0700 dir, 0600 files), never the repo checkout.
# ---------------------------------------------------------------------------

def test_gold_rate_and_export_default_to_private_dir(tmp_path, monkeypatch):
    import os
    home, cwd = tmp_path / "home", tmp_path / "repo"
    home.mkdir()
    cwd.mkdir()
    monkeypatch.setenv("HOME", str(home))
    monkeypatch.chdir(cwd)
    so = str(tmp_path / "so.db")
    s = store.open_store(so)
    for i in range(2):
        store.add_reference(s, "+1a", i, f"ctx {i}", f"reply {i}", "g@local", "reference-v1", 1)
    s.close()
    assert gold_rate.main(["--store", so, "--write"]) == 0
    d = home / ".human" / "second_opinion"
    sheet = d / "rate.csv"
    assert (d.stat().st_mode & 0o777) == 0o700 and (sheet.stat().st_mode & 0o777) == 0o600
    rows = list(csv.DictReader(open(sheet)))
    assert len(rows) == 2
    write_sheet(sheet, [{"id": rows[0]["id"], "context": "", "reply": "", "good": "y"},
                        {"id": rows[1]["id"], "context": "", "reply": "", "good": "n"}])
    assert gold_rate.main(["--store", so, "--import"]) == 0
    assert gold_export.main(["--store", so]) == 0
    out = d / "gold_export.csv"
    assert (out.stat().st_mode & 0o777) == 0o600
    assert len(list(csv.DictReader(open(out)))) == 1
    assert os.listdir(cwd) == []          # nothing landed in the checkout


def test_explicit_output_dir_is_not_chmodded(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    mine = tmp_path / "mine"
    mine.mkdir(mode=0o755)
    mine.chmod(0o755)
    assert gold_export.main([str(mine / "x.csv"), "--store", str(tmp_path / "so.db")]) == 0
    assert (mine.stat().st_mode & 0o777) == 0o755
    assert ((mine / "x.csv").stat().st_mode & 0o777) == 0o600
