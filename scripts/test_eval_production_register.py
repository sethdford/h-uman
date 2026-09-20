"""Hermetic tests for eval_production_register: a temp production_outcomes
table, temp cards under HU_PERSONA_DIR, exact numbers, and every refusal
path writing nothing."""
import json
import os
import sqlite3
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import eval_production_register as epr  # noqa: E402

LONG = "should I take the job that pays more even though the work is boring? " * 3  # substantive, no distress marker


def _db(path, rows, age_s=3600):
    con = sqlite3.connect(path)
    con.execute("CREATE TABLE production_outcomes(id INTEGER PRIMARY KEY AUTOINCREMENT, channel TEXT NOT NULL, "
                "target TEXT NOT NULL, message_ref TEXT, prompt TEXT NOT NULL, chosen TEXT NOT NULL, "
                "send_timestamp INTEGER NOT NULL)")
    now = int(time.time())
    con.executemany("INSERT INTO production_outcomes(channel,target,prompt,chosen,send_timestamp) VALUES(?,?,?,?,?)",
                    [("imessage", "t", p, c, now - age_s) for p, c in rows])
    con.commit()
    con.close()


def _cards(d):
    os.makedirs(d, exist_ok=True)
    json.dump({"schema": "style-card/v2", "axes": {"dash_rate": {"value": 0.0, "n": 900}},
               "substantive_reply": {"n": 65, "median_chars": 26, "share_le_60_chars": 0.74,
                                     "answer_first_rate": 0.34, "agreement_opener_rate": 0.06}},
              open(os.path.join(d, "seth.style-card.json"), "w"))
    json.dump({"schema": "emotion-card/v1",
               "distress_reply": {"n": 11, "median_chars": 17, "scaffold_rate": 0.0}},
              open(os.path.join(d, "seth.emotion-card.json"), "w"))


ROWS = [(LONG, "yeah take it"), (LONG, "take it"), (LONG, "exactly — money talks"),
        (LONG, "nah"), (LONG, "totally"),                       # substantive x5: agree 3/5, dash 1/5
        ("ugh worst day 😭", "I'm sorry to hear that. How can I help you"), ("so sad", "damn"),
        ("hey", "yo"), ("wyd", "nm"), ("lol", "lol"), ("k", "k"), ("hi", "hey")]


class Measure(unittest.TestCase):
    def test_registers_and_deltas_are_exact(self):
        with tempfile.TemporaryDirectory() as d:
            _cards(d)
            os.environ["HU_PERSONA_DIR"] = d
            style, emo = epr.load_card("seth", "style"), epr.load_card("seth", "emotion")
        body = epr.measure([(p, c, 0, "imessage") for p, c in ROWS], style, emo, min_register_n=2)
        sub = body["registers"]["substantive"]
        self.assertEqual(sub["n"], 5)
        self.assertAlmostEqual(sub["agreement_opener_rate"], 0.6)
        self.assertAlmostEqual(sub["dash_rate"], 0.2)
        self.assertTrue(sub["measured"])
        dist = body["registers"]["distress"]
        self.assertEqual(dist["n"], 2)
        self.assertAlmostEqual(dist["scaffold_rate"], 0.5)
        self.assertEqual(body["registers"]["casual"]["n"], 5)
        self.assertEqual(body["overall"]["n"], 12)
        d = body["deltas_twin_minus_seth"]
        self.assertAlmostEqual(d["substantive"]["agreement_opener_rate"], 0.54)
        self.assertAlmostEqual(d["overall"]["dash_rate"], 1 / 12, places=3)  # deltas are rounded to 4 places
        self.assertAlmostEqual(d["distress"]["scaffold_rate"], 0.5)
        self.assertTrue(any("agreement_opener_rate" in g for g in body["gaps"]))
        self.assertTrue(any("scaffold" in g for g in body["gaps"]))
        self.assertTrue(any("dash_rate" in g for g in body["gaps"]))

    def test_register_below_min_reports_n_only_and_no_deltas(self):
        body = epr.measure([(p, c, 0, "imessage") for p, c in ROWS], None, None, min_register_n=50)
        self.assertFalse(body["registers"]["substantive"]["measured"])
        self.assertEqual(body["deltas_twin_minus_seth"], {})
        self.assertEqual(body["seth"], {})  # no cards -> no reference, no invented numbers

    def test_no_replies_is_the_empty_shape(self):
        body = epr.measure([], None, None, 1)
        self.assertEqual(body["overall"], {"n": 0})
        self.assertEqual(body["gaps"], [])


class Cli(unittest.TestCase):
    def test_writes_verdict_with_window_and_line(self):
        with tempfile.TemporaryDirectory() as d:
            _cards(d)
            os.environ["HU_PERSONA_DIR"] = d
            db = os.path.join(d, "m.db")
            _db(db, ROWS)
            out = os.path.join(d, "v.json")
            rc = epr.main(["--db", db, "--days", "1", "--min-n", "5", "--min-register-n", "2",
                           "--output-json", out])
            self.assertEqual(rc, 0)
            v = json.load(open(out))
            self.assertEqual(v["schema"], epr.SCHEMA)
            self.assertEqual(v["window"]["n"], 12)
            self.assertEqual(v["window"]["channels"], {"imessage": 12})
            self.assertEqual(v["seth"]["substantive"]["n"], 65)
            for _, reply in ROWS:  # counts only, never text
                self.assertNotIn(reply, json.dumps(v))

    def test_refuses_below_min_n_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as d:
            db = os.path.join(d, "m.db")
            _db(db, ROWS[:3])
            out = os.path.join(d, "v.json")
            self.assertEqual(epr.main(["--db", db, "--days", "1", "--min-n", "20", "--output-json", out]), 3)
            self.assertFalse(os.path.exists(out))

    def test_window_excludes_old_turns(self):
        with tempfile.TemporaryDirectory() as d:
            db = os.path.join(d, "m.db")
            _db(db, ROWS, age_s=40 * 86400)
            out = os.path.join(d, "v.json")
            self.assertEqual(epr.main(["--db", db, "--days", "14", "--min-n", "1", "--output-json", out]), 3)
            self.assertFalse(os.path.exists(out))

    def test_unreadable_db_refuses(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "v.json")
            rc = epr.main(["--db", os.path.join(d, "missing.db"), "--output-json", out])
            self.assertEqual(rc, 3)
            self.assertFalse(os.path.exists(out))


if __name__ == "__main__":
    unittest.main()
