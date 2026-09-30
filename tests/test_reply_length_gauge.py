"""Tests for scripts/reply_length_gauge.py -- hermetic: synthetic chat.db +
memory.db built with the eval_conversation_quality.py test fixture, no real
messages, no ~/.human, no chat.db, no network."""
import datetime as dt
import json
import os
import stat
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import reply_length_gauge as rlg  # noqa: E402
from test_eval_conversation_quality import Fixture, MIN, T0  # noqa: E402

SINCE = dt.datetime(2000, 1, 1, tzinfo=dt.timezone.utc)  # covers every fixture time
NOW_ISO = "2026-09-05T00:00:00+00:00"  # a few days after T0, inside the default window


def _fill(fx, contact, lens, huuman, start, step=20 * MIN):
    """Insert len(lens) from_me messages for contact, each len(text) == the
    given length, spaced far enough apart (20 min default) that no message
    falls inside another's 15-minute assistant-match window."""
    for i, n in enumerate(lens):
        fx.msg(contact, start + i * step, "a" * n, True, huuman=huuman)


def _ts(secs):
    return (T0 + dt.timedelta(seconds=secs)).strftime("%Y-%m-%d %H:%M:%S")


def _build(tmp_path, fill):
    fx = Fixture(str(tmp_path))
    fill(fx)
    fx.close()
    return fx.chat_path, fx.mem_path


class TestMeasure:
    def test_metric_values_on_hand_computed_fixture(self, tmp_path):
        # seth: 10..19 chars -> p10=10, p50=14, p90=18.
        seth_lens = list(range(10, 20))
        # huuman: 3 below seth's p10, 3 in the middle, 4 above seth's p90.
        huuman_lens = [5, 8, 9, 14, 14, 14, 20, 22, 25, 30]

        def fill(fx):
            _fill(fx, "+1", seth_lens, False, start=0)
            _fill(fx, "+1", huuman_lens, True, start=100000)

        chat_path, mem_path = _build(tmp_path, fill)
        result = rlg.measure(chat_path, mem_path, SINCE, min_n=10)
        assert result is not None
        c = result["contacts"]["c1"]

        assert c["n_seth"] == 10 and c["n_huuman"] == 10
        assert (c["seth"]["p10"], c["seth"]["p50"], c["seth"]["p90"]) == (10, 14, 18)
        assert c["huuman"]["p50"] == 14
        assert c["median_ratio"] == 14 / 14
        assert c["brevity_rate"] == 0.3        # 5, 8, 9 < 10
        assert c["excess_rate"] == 0.4         # 20, 22, 25, 30 > 18
        assert c["excess_chars_mean"] == 2.5   # (2+4+7+12)/10
        assert c["deficit_chars_mean"] == 0.8  # (5+2+1)/10

        ov = result["overall"]
        assert ov["contacts_measured"] == 1
        assert ov["contacts_skipped_min_n"] == 0
        # One measured contact: overall (each reply weighted equally) must
        # equal that contact's own numbers.
        assert ov["median_ratio"] == c["median_ratio"]
        assert ov["brevity_rate"] == c["brevity_rate"]

    def test_bytes_p50_differs_from_char_p50_for_multibyte_text(self, tmp_path):
        seth_lens = [10] * 10  # ASCII: chars == bytes

        def fill(fx):
            _fill(fx, "+1", seth_lens, False, start=0)
            for i in range(10):
                fx.msg("+1", 100000 + i * 20 * MIN, "gn \U0001f618", True, huuman=True)

        chat_path, mem_path = _build(tmp_path, fill)
        result = rlg.measure(chat_path, mem_path, SINCE, min_n=10)
        h = result["contacts"]["c1"]["huuman"]
        assert h["p50"] == 4          # "gn " + one emoji codepoint == 4 chars
        assert h["bytes_p50"] == 7    # 3 ascii bytes + a 4-byte emoji

    def test_ambiguous_replies_are_excluded_from_every_count(self, tmp_path):
        def fill(fx):
            _fill(fx, "+1", [10] * 5, False, start=0)
            _fill(fx, "+1", [10] * 5, True, start=50000)
            # h-uman logged a reply but a DIFFERENT text was delivered: this
            # from_me send is "ambiguous" per eval_conversation_quality, and
            # must not be counted toward either side.
            fx.mem.execute(
                "insert into messages(session_id,role,content,created_at) values (?,?,?,?)",
                ("+1", "assistant", "totally different logged text and a much longer one",
                 _ts(100000)))
            fx.msg("+1", 100000 + 60, "something else was delivered here today", True)

        chat_path, mem_path = _build(tmp_path, fill)
        result = rlg.measure(chat_path, mem_path, SINCE, min_n=5)
        c = result["contacts"]["c1"]
        assert c["n_seth"] == 5
        assert c["n_huuman"] == 5

    def test_contact_below_min_n_is_skipped_not_measured(self, tmp_path):
        def fill(fx):
            # +1 meets the bar on both sides.
            _fill(fx, "+1", [10] * 10, False, start=0)
            _fill(fx, "+1", [10] * 10, True, start=100000)
            # +2 has plenty of seth traffic but only 3 huuman replies.
            _fill(fx, "+2", [10] * 10, False, start=300000)
            _fill(fx, "+2", [10] * 3, True, start=400000)

        chat_path, mem_path = _build(tmp_path, fill)
        result = rlg.measure(chat_path, mem_path, SINCE, min_n=10)
        assert list(result["contacts"]) == ["c1"]
        assert result["overall"]["contacts_measured"] == 1
        assert result["overall"]["contacts_skipped_min_n"] == 1

    def test_refuses_on_zero_measured_contacts(self, tmp_path):
        def fill(fx):
            _fill(fx, "+1", [10] * 3, False, start=0)
            _fill(fx, "+1", [10] * 3, True, start=100000)

        chat_path, mem_path = _build(tmp_path, fill)
        assert rlg.measure(chat_path, mem_path, SINCE, min_n=10) is None

    def test_refuses_on_unreadable_chat_db(self, tmp_path):
        missing = str(tmp_path / "nope.db")
        fx = Fixture(str(tmp_path))
        fx.close()
        assert rlg.measure(missing, fx.mem_path, SINCE, min_n=1) is None

    def test_refuses_on_unreadable_memory_db(self, tmp_path):
        missing = str(tmp_path / "nope-mem.db")
        fx = Fixture(str(tmp_path))
        fx.close()
        assert rlg.measure(fx.chat_path, missing, SINCE, min_n=1) is None


class TestMain:
    def test_writes_nothing_on_refusal(self, tmp_path, capsys):
        chat_path = str(tmp_path / "missing-chat.db")
        mem_path = str(tmp_path / "missing-mem.db")
        out_dir = str(tmp_path / "logs")
        rc = rlg.main(["--chat-db", chat_path, "--memory-db", mem_path,
                       "--out-dir", out_dir, "--now", NOW_ISO])
        assert rc == 2
        assert not os.path.isdir(out_dir) or os.listdir(out_dir) == []

    def test_writes_private_json_with_no_text_or_handles(self, tmp_path, capsys):
        planted_text = "a very distinctive planted sentence nobody else writes"
        handle = "+15551230099"

        def fill(fx):
            _fill(fx, handle, [10] * 10, False, start=0)
            for i in range(10):
                fx.msg(handle, 100000 + i * 20 * MIN, planted_text, True, huuman=True)

        chat_path, mem_path = _build(tmp_path, fill)
        out_dir = str(tmp_path / "logs")
        rc = rlg.main(["--chat-db", chat_path, "--memory-db", mem_path,
                       "--out-dir", out_dir, "--min-n", "10", "--now", NOW_ISO])
        assert rc == 0
        out_path = os.path.join(out_dir, "reply-length-20260905.json")
        assert os.path.isfile(out_path)

        raw = open(out_path).read()
        assert planted_text not in raw
        assert handle not in raw

        payload = json.loads(raw)
        assert "c1" in payload["contacts"]
        assert payload["overall"]["contacts_measured"] == 1

        mode = stat.S_IMODE(os.stat(out_path).st_mode)
        assert mode == 0o600
