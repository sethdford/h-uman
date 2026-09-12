#!/usr/bin/env python3
"""Hermetic tests for the emotional-register axis.

No :8741, no chat.db, no ~/.human, no network: a fake judge answers from a
keyword table, twin replies come from a temp sqlite file, cards go to a
temp dir. Each test pins one contract of
scripts/emotion_register.py / measure_emotion_card.py / eval_emotion_register.py:
the taxonomy, lenient parsing, exact aggregation, JSD properties, and —
most importantly — that every refusal path writes NOTHING.

Run: python3 scripts/test_emotion_register.py
"""
import argparse
import datetime
import json
import os
import sqlite3
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import emotion_register as er  # noqa: E402
import eval_emotion_register as ev  # noqa: E402
import measure_emotion_card as mec  # noqa: E402

T0 = datetime.datetime(2026, 8, 1, 12, 0, 0)


class FakeJudge:
    """Answers with the label named in the text ('[amusement 0.4]'), else
    neutral. Optionally garbles every k-th reply so parse failures can be
    exercised. `wrapped` returns fenced JSON like the real server."""

    def __init__(self, model="fake-model", garble_every=0, wrapped=False):
        self._model = model
        self.garble_every = garble_every
        self.wrapped = wrapped
        self.calls = 0
        self.base_url = "fake://judge"

    def model(self):
        return self._model

    def id(self):
        return er.judge_id(self._model)

    def label(self, text):
        self.calls += 1
        if self.garble_every and self.calls % self.garble_every == 0:
            return "I cannot tell."
        emotion, intensity = er.NEUTRAL, 0.0
        if "[" in text and "]" in text:
            inner = text[text.index("[") + 1:text.index("]")]
            parts = inner.rsplit(" ", 1)
            emotion, intensity = parts[0], float(parts[1])
        body = json.dumps({"emotion": emotion, "intensity": intensity})
        return f"```json\n{body}\n```" if self.wrapped else body


class DownJudge:
    base_url = "fake://down"

    def model(self):
        raise er.JudgeUnavailable("connection refused")

    def id(self):
        raise er.JudgeUnavailable("connection refused")

    def label(self, text):
        raise er.JudgeUnavailable("connection refused")


def messages(spec):
    """spec: list of (emotion, intensity) -> synthetic (ts, text) pairs."""
    out = []
    for i, (emotion, intensity) in enumerate(spec):
        text = f"msg {i} [{emotion} {intensity}]" if emotion != er.NEUTRAL else f"msg {i} ok"
        out.append((T0 + datetime.timedelta(minutes=i), text))
    return out


def seth_spec(n=200):
    """80% neutral, 10% amusement 0.4, 5% interest 0.3, 5% satisfaction 0.3."""
    spec = []
    for i in range(n):
        r = i % 20
        if r < 16:
            spec.append((er.NEUTRAL, 0.0))
        elif r < 18:
            spec.append(("amusement", 0.4))
        elif r == 18:
            spec.append(("interest", 0.3))
        else:
            spec.append(("satisfaction", 0.3))
    return spec


def card_args(tmp, **over):
    a = argparse.Namespace(db="/nonexistent", persona="cardtest", days=60, end="2026-09-01",
                           min_n=100, max_n=300, max_parse_failure_rate=0.10,
                           mlx_url="fake://judge", n_resamples=200, seed=1,
                           out=os.path.join(tmp, "cardtest.emotion-card.json"), dry_run=False)
    for k, v in over.items():
        setattr(a, k, v)
    return a


def eval_args(tmp, card_path, **over):
    a = argparse.Namespace(memory_db=os.path.join(tmp, "memory.db"), persona="cardtest",
                           card=card_path, days=14, min_n=20, max_n=200, jsd_max=0.15,
                           max_parse_failure_rate=0.10, mlx_url="fake://judge",
                           n_resamples=200, seed=1,
                           output_json=os.path.join(tmp, "verdict.json"), dry_run=False)
    for k, v in over.items():
        setattr(a, k, v)
    return a


def make_memory_db(path, spec, now):
    con = sqlite3.connect(path)
    con.execute("CREATE TABLE production_outcomes(id INTEGER PRIMARY KEY, channel TEXT, "
                "target TEXT, prompt TEXT, chosen TEXT, send_timestamp INTEGER)")
    for i, (emotion, intensity) in enumerate(spec):
        text = f"reply {i} [{emotion} {intensity}]" if emotion != er.NEUTRAL else f"reply {i} ok"
        ts = int((now - datetime.timedelta(hours=i)).timestamp())
        con.execute("INSERT INTO production_outcomes(channel,target,prompt,chosen,send_timestamp) "
                    "VALUES('imessage','+1000','p',?,?)", (text, ts))
    # rows the query must ignore: other channel, empty text, too old
    con.execute("INSERT INTO production_outcomes(channel,target,prompt,chosen,send_timestamp) "
                "VALUES('slack','x','p','[horror 1.0]',?)", (int(now.timestamp()),))
    con.execute("INSERT INTO production_outcomes(channel,target,prompt,chosen,send_timestamp) "
                "VALUES('imessage','x','p','',?)", (int(now.timestamp()),))
    con.execute("INSERT INTO production_outcomes(channel,target,prompt,chosen,send_timestamp) "
                "VALUES('imessage','x','p','[horror 1.0]',?)",
                (int((now - datetime.timedelta(days=400)).timestamp()),))
    con.commit()
    con.close()


class TaxonomyTests(unittest.TestCase):
    def test_27_distinct_categories_plus_neutral_each_with_a_valence(self):
        self.assertEqual(len(er.CATEGORIES), 27)
        self.assertEqual(len(set(er.CATEGORIES)), 27)
        self.assertNotIn(er.NEUTRAL, er.CATEGORIES)
        self.assertEqual(set(er.VALENCE), set(er.LABELS))
        self.assertTrue(all(v in (-1, 0, 1) for v in er.VALENCE.values()))

    def test_judge_id_pins_model_taxonomy_and_prompt(self):
        jid = er.judge_id("m")
        self.assertEqual(jid, f"m|{er.TAXONOMY_VERSION}|{er.PROMPT_SHA}")
        self.assertEqual(len(er.PROMPT_SHA), 12)

    def test_prompt_lists_every_category_and_the_message(self):
        p = er.render_prompt("hey there")
        for c in er.CATEGORIES:
            self.assertIn(f"- {c}", p)
        self.assertIn("hey there", p)
        self.assertIn('"neutral"', p)


class ParseTests(unittest.TestCase):
    def test_bare_json(self):
        self.assertEqual(er.parse_label('{"emotion": "joy", "intensity": 0.5}'), ("joy", 0.5))

    def test_fenced_json_and_think_block(self):
        raw = '<think>hmm</think>```json\n{"emotion":"Amusement","intensity":"0.4"}\n```'
        self.assertEqual(er.parse_label(raw), ("amusement", 0.4))

    def test_intensity_clamped_to_unit_interval(self):
        self.assertEqual(er.parse_label('{"emotion":"joy","intensity":7}'), ("joy", 1.0))
        self.assertEqual(er.parse_label('{"emotion":"joy","intensity":-1}'), ("joy", 0.0))

    def test_rejects_off_taxonomy_junk_and_nan(self):
        self.assertIsNone(er.parse_label('{"emotion":"happy","intensity":0.5}'))
        self.assertIsNone(er.parse_label("no json here"))
        self.assertIsNone(er.parse_label(""))
        self.assertIsNone(er.parse_label(None))
        self.assertIsNone(er.parse_label('{"emotion":"joy","intensity":"x"}'))
        self.assertIsNone(er.parse_label('[1,2]'))


class AggregateTests(unittest.TestCase):
    def test_exact_shares_and_axes(self):
        labels = [(er.NEUTRAL, 0.0)] * 6 + [("amusement", 0.5)] * 3 + [("sadness", 1.0)]
        agg = er.aggregate(labels, n_resamples=50, seed=1)
        self.assertEqual(agg["n"], 10)
        self.assertEqual(agg["parse_failures"], 0)
        self.assertAlmostEqual(agg["distribution"]["neutral"]["share"], 0.6)
        self.assertAlmostEqual(agg["distribution"]["amusement"]["share"], 0.3)
        self.assertAlmostEqual(agg["distribution"]["sadness"]["share"], 0.1)
        self.assertAlmostEqual(agg["neutral_share"]["value"], 0.6)
        self.assertAlmostEqual(agg["mean_intensity"]["value"], 0.25)
        self.assertAlmostEqual(agg["valence_mean"]["value"], 0.2)  # (3 - 1) / 10
        self.assertEqual([t["emotion"] for t in agg["top"]], ["amusement", "sadness"])
        self.assertLessEqual(agg["neutral_share"]["ci_lo"], 0.6)
        self.assertGreaterEqual(agg["neutral_share"]["ci_hi"], 0.6)

    def test_counts_parse_failures_and_refuses_when_nothing_parsed(self):
        agg = er.aggregate([("joy", 0.1), None, None], n_resamples=10)
        self.assertEqual(agg["n"], 1)
        self.assertEqual(agg["parse_failures"], 2)
        with self.assertRaises(er.MeasurementRefused):
            er.aggregate([None, None], n_resamples=10)


class JsdTests(unittest.TestCase):
    def test_identical_is_zero_disjoint_is_one_symmetric(self):
        a = {"neutral": {"share": 0.5}, "joy": {"share": 0.5}}
        b = {"sadness": {"share": 1.0}}
        self.assertAlmostEqual(er.jsd(a, a), 0.0)
        self.assertAlmostEqual(er.jsd(a, b), 1.0)
        c = {"neutral": {"share": 0.7}, "joy": {"share": 0.3}}
        self.assertAlmostEqual(er.jsd(a, c), er.jsd(c, a))
        self.assertTrue(0.0 < er.jsd(a, c) < 1.0)


class CardTests(unittest.TestCase):
    def test_writes_a_v1_card_from_the_fake_judge(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = card_args(tmp)
            judge = FakeJudge(wrapped=True)
            rc = mec.run(args, messages=messages(seth_spec(200)), judge=judge)
            self.assertEqual(rc, 0)
            card = json.load(open(args.out))
            self.assertEqual(card["schema"], "emotion-card/v1")
            self.assertEqual(card["n"], 200)
            self.assertEqual(card["judge"]["id"], er.judge_id("fake-model"))
            self.assertAlmostEqual(card["neutral_share"]["value"], 0.8)
            self.assertAlmostEqual(card["distribution"]["amusement"]["share"], 0.1)
            self.assertEqual(card["top"][0]["emotion"], "amusement")
            self.assertEqual(card["window"], {"start": "2026-07-03", "end": "2026-09-01", "days": 60})
            self.assertEqual(judge.calls, 200)

    def test_samples_at_most_max_n_deterministically(self):
        msgs = messages(seth_spec(400))
        start, end = T0 - datetime.timedelta(days=1), T0 + datetime.timedelta(days=1)
        a, n_window = mec.select_window(msgs, start, end, 150, seed=7)
        b, _ = mec.select_window(msgs, start, end, 150, seed=7)
        self.assertEqual(n_window, 400)
        self.assertEqual(len(a), 150)
        self.assertEqual(a, b)
        outside, _ = mec.select_window(msgs, end, end + datetime.timedelta(days=1), 150, 7)
        self.assertEqual(outside, [])

    def test_refuses_below_min_n_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = card_args(tmp)
            judge = FakeJudge()
            rc = mec.run(args, messages=messages(seth_spec(60)), judge=judge)
            self.assertEqual(rc, 3)
            self.assertFalse(os.path.exists(args.out))
            self.assertEqual(judge.calls, 0)  # refused BEFORE spending judge time

    def test_defers_when_judge_down_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = card_args(tmp)
            rc = mec.run(args, messages=messages(seth_spec(200)), judge=DownJudge())
            self.assertEqual(rc, 2)
            self.assertFalse(os.path.exists(args.out))

    def test_refuses_when_judge_stops_labeling(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = card_args(tmp)
            rc = mec.run(args, messages=messages(seth_spec(200)), judge=FakeJudge(garble_every=5))
            self.assertEqual(rc, 3)  # 20% failures > 10%
            self.assertFalse(os.path.exists(args.out))

    def test_dry_run_prints_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            args = card_args(tmp, dry_run=True)
            rc = mec.run(args, messages=messages(seth_spec(120)), judge=FakeJudge())
            self.assertEqual(rc, 0)
            self.assertFalse(os.path.exists(args.out))


class EvalTests(unittest.TestCase):
    def _card(self, tmp, judge=None):
        args = card_args(tmp)
        rc = mec.run(args, messages=messages(seth_spec(200)), judge=judge or FakeJudge())
        self.assertEqual(rc, 0)
        return args.out

    def test_twin_matching_the_card_measures_jsd_near_zero(self):
        with tempfile.TemporaryDirectory() as tmp:
            card = self._card(tmp)
            now = T0
            make_memory_db(os.path.join(tmp, "memory.db"), seth_spec(60), now)
            args = eval_args(tmp, card)
            rc = ev.run(args, judge=FakeJudge(), now=now)
            self.assertEqual(rc, 0)
            v = json.load(open(args.output_json))
            self.assertEqual(v["schema"], "emotion-register/v1")
            self.assertEqual(v["verdict"], "MEASURED")
            self.assertEqual(v["n"], 60)  # slack / empty / 400-day-old rows excluded
            self.assertAlmostEqual(v["jsd"]["value"], 0.0, places=6)
            self.assertFalse(v["gap"])
            self.assertAlmostEqual(v["deltas"]["neutral_share"], 0.0)
            self.assertEqual(v["judge"]["id"], er.judge_id("fake-model"))

    def test_performed_enthusiasm_shows_as_a_gap(self):
        with tempfile.TemporaryDirectory() as tmp:
            card = self._card(tmp)
            now = T0
            twin = [("excitement", 0.8)] * 30 + [("joy", 0.7)] * 10
            make_memory_db(os.path.join(tmp, "memory.db"), twin, now)
            args = eval_args(tmp, card)
            self.assertEqual(ev.run(args, judge=FakeJudge(), now=now), 0)
            v = json.load(open(args.output_json))
            self.assertTrue(v["gap"])
            self.assertGreater(v["jsd"]["value"], 0.5)
            self.assertAlmostEqual(v["deltas"]["neutral_share"], -0.8)
            self.assertGreater(v["deltas"]["mean_intensity"], 0.6)
            self.assertEqual(v["largest_shifts"][0]["emotion"], "neutral")
            self.assertEqual(v["twin"]["top"][0]["emotion"], "excitement")

    def test_refuses_a_different_judge_and_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            card = self._card(tmp, judge=FakeJudge(model="model-A"))
            make_memory_db(os.path.join(tmp, "memory.db"), seth_spec(60), T0)
            args = eval_args(tmp, card)
            rc = ev.run(args, judge=FakeJudge(model="model-B"), now=T0)
            self.assertEqual(rc, 3)
            self.assertFalse(os.path.exists(args.output_json))

    def test_refuses_below_min_n_without_spending_judge_calls(self):
        with tempfile.TemporaryDirectory() as tmp:
            card = self._card(tmp)
            make_memory_db(os.path.join(tmp, "memory.db"), seth_spec(10), T0)
            args = eval_args(tmp, card)
            judge = FakeJudge()
            self.assertEqual(ev.run(args, judge=judge, now=T0), 3)
            self.assertFalse(os.path.exists(args.output_json))
            self.assertEqual(judge.calls, 0)

    def test_defers_when_judge_down(self):
        with tempfile.TemporaryDirectory() as tmp:
            card = self._card(tmp)
            make_memory_db(os.path.join(tmp, "memory.db"), seth_spec(60), T0)
            args = eval_args(tmp, card)
            self.assertEqual(ev.run(args, judge=DownJudge(), now=T0), 2)
            self.assertFalse(os.path.exists(args.output_json))

    def test_refuses_missing_or_foreign_card(self):
        with tempfile.TemporaryDirectory() as tmp:
            make_memory_db(os.path.join(tmp, "memory.db"), seth_spec(60), T0)
            args = eval_args(tmp, os.path.join(tmp, "missing.json"))
            self.assertEqual(ev.run(args, judge=FakeJudge(), now=T0), 3)
            foreign = os.path.join(tmp, "style.json")
            json.dump({"schema": "style-card/v2"}, open(foreign, "w"))
            args = eval_args(tmp, foreign)
            self.assertEqual(ev.run(args, judge=FakeJudge(), now=T0), 3)
            self.assertFalse(os.path.exists(args.output_json))

    def test_fetch_twin_replies_reads_only_imessage_in_window(self):
        with tempfile.TemporaryDirectory() as tmp:
            db = os.path.join(tmp, "memory.db")
            make_memory_db(db, seth_spec(5), T0)
            since = int((T0 - datetime.timedelta(days=14)).timestamp())
            rows = ev.fetch_twin_replies(db, since, 200)
            self.assertEqual(len(rows), 5)
            self.assertTrue(all(t.startswith("reply") for _, t in rows))
            self.assertEqual(len(ev.fetch_twin_replies(db, since, 2)), 2)


if __name__ == "__main__":
    unittest.main()
