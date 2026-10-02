"""Tests for scripts/learned_style_profile.py -- the nightly learned-style
learner (docs/guides/learned-style.md).

Hermetic: every chat.db / memory.db is a synthetic fixture built in a temp dir
with the eval_conversation_quality.py test Fixture; HOME is pointed at the temp
dir so no default path can reach the real ~/.human or chat.db. No network, no
model, no daemon ports.
"""
import datetime as dt
import json
import os
import stat
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import learned_style_profile as lsp  # noqa: E402
from test_eval_conversation_quality import Fixture, MIN, T0  # noqa: E402

NOW = T0 + dt.timedelta(days=10)
NOW_ISO = NOW.strftime("%Y-%m-%dT%H:%M:%SZ")

# Distinctive message texts. The privacy tests assert none of these strings
# (nor any substring of length >= 6) ever reaches a file, a log or stdout.
SECRET_IN = "zebra quokka inbound marker?"
SECRET_OUT = "platypus narwhal reply marker"


@pytest.fixture(autouse=True)
def _isolate_home(tmp_path, monkeypatch):
    """Belt and braces: any expanduser() default lands in the temp dir."""
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    monkeypatch.delenv("HU_PERSONA_DIR", raising=False)
    monkeypatch.delenv("HU_STATE_DIR", raising=False)


# ── shape rule (shared contract with the C runtime) ─────────────────────────

SHAPE_VECTORS = [
    ("you coming tonight?", "question"),
    ("ok", "casual"),
    ("", "casual"),
    ("lol yes", "casual"),
    ("So today was wild. Work ran late and then the car wouldn't start. "
     "Ended up getting a ride home from Dave.", "story"),
    ("Went to the store. Got milk.", "casual"),
    ("I don't even know where to start with this week honestly, it has been "
     "one thing after another and I'm tired", "casual"),
    ("We closed on the house!!! Keys tomorrow. Movers Friday. I can't believe "
     "it's actually happening, finally.", "story"),
    ("what?? no way", "question"),
    ("Long day... really long. Talk later.", "casual"),
    # 140-byte boundary (no '?', no terminators) -> story; 139 -> casual.
    ("a" * 140, "story"),
    ("a" * 139, "casual"),
    # Bytes, not characters: 70 two-byte chars are 140 bytes.
    ("é" * 70, "story"),
    # Whitespace is trimmed before measuring.
    ("   " + "a" * 139 + "   ", "casual"),
    (None, "casual"),
    # A multi-bubble burst joined with "\n" (how both parts classify it).
    ("So today was wild.\nWork ran late and the car wouldn't start.\n"
     "Ended up getting a ride home from Dave.", "story"),
]


@pytest.mark.parametrize("text,expected", SHAPE_VECTORS)
def test_shape_rule_vectors(text, expected):
    assert lsp.shape(text) == expected


def test_shape_counts_each_terminator_run_once():
    # 80+ bytes, one run of "!!!" and one "." -> 2 runs -> story.
    t = "x" * 70 + "!!! and then." + ""
    assert len(t.encode()) >= 80
    assert lsp.shape(t) == "story"
    # Same length, a single run of five terminators -> 1 run -> casual.
    t1 = "x" * 78 + "....."
    assert lsp.shape(t1) == "casual"


# ── weighting and quantiles ────────────────────────────────────────────────

def test_recency_weight_half_life_21_days():
    assert lsp.recency_weight(0) == 1.0
    assert lsp.recency_weight(21) == pytest.approx(0.5)
    assert lsp.recency_weight(42) == pytest.approx(0.25)
    assert lsp.recency_weight(-3) == 1.0  # clock skew never up-weights


def test_weighted_quantile_follows_weights():
    vals = [10, 20, 30, 40]
    assert lsp.weighted_quantile(vals, [1, 1, 1, 1], 0.5) == 20
    assert lsp.weighted_quantile(vals, [1, 1, 1, 1], 0.9) == 40
    assert lsp.weighted_quantile(vals, [1, 1, 1, 97], 0.5) == 40
    assert lsp.weighted_quantile(vals, [97, 1, 1, 1], 0.5) == 10


def _sample(age_days, length, **kw):
    s = {"age_days": age_days, "len": length, "bubbles": 1, "lower": False,
         "emoji": False, "end_punct": False, "latency_s": 60,
         "shape": "casual", "band": "day", "rapid": False}
    s.update(kw)
    return s


def test_recent_samples_dominate_the_median():
    # 6 old long replies (84 days old, weight 1/16 each) vs 3 fresh short ones.
    samples = [_sample(84, 200) for _ in range(6)] + [_sample(0, 20) for _ in range(3)]
    st = lsp.compute_stats(samples)
    assert st["n"] == 9
    assert st["n_eff"] == pytest.approx(3 + 6 / 16, abs=1e-3)
    assert st["len_p50"] == 20          # unweighted median would be 200
    assert st["shrunk"] is False


def test_compute_stats_rates_are_weighted_shares():
    samples = [_sample(0, 10, lower=True, emoji=True, end_punct=False, latency_s=30),
               _sample(0, 10, lower=False, emoji=False, end_punct=True, latency_s=90),
               _sample(0, 10, lower=True, emoji=False, end_punct=True, latency_s=60,
                       bubbles=3)]
    st = lsp.compute_stats(samples)
    assert st["lower_start_rate"] == pytest.approx(2 / 3, abs=1e-4)
    assert st["emoji_rate"] == pytest.approx(1 / 3, abs=1e-4)
    assert st["end_punct_rate"] == pytest.approx(2 / 3, abs=1e-4)
    assert st["latency_p50_s"] == 60
    assert st["bubbles_p50"] == 1.0


# ── shrinkage (worked example from the contract) ───────────────────────────

def test_shrinkage_worked_example():
    child = {"n": 4, "n_eff": 4.0, "len_p25": 10, "len_p50": 20, "len_p90": 40,
             "bubbles_p50": 1.0, "lower_start_rate": 0.0, "emoji_rate": 1.0,
             "end_punct_rate": 0.5, "latency_p50_s": None, "shrunk": False}
    parent = {"n": 100, "n_eff": 60.0, "len_p25": 22, "len_p50": 40, "len_p90": 100,
              "bubbles_p50": 2.5, "lower_start_rate": 0.6, "emoji_rate": 0.1,
              "end_punct_rate": 0.5, "latency_p50_s": 120, "shrunk": False}
    out = lsp.shrink(child, parent)
    # (n_eff * v + K * parent) / (n_eff + K), K = 8:
    assert out["len_p50"] == 33            # (4*20 + 8*40)/12 = 33.33
    assert out["len_p25"] == 18            # (40 + 176)/12 = 18.0
    assert out["len_p90"] == 80            # (160 + 800)/12 = 80.0
    assert out["bubbles_p50"] == pytest.approx(2.0)   # (4 + 20)/12
    assert out["lower_start_rate"] == pytest.approx(0.4)
    assert out["emoji_rate"] == pytest.approx(0.4)    # (4 + 0.8)/12
    assert out["end_punct_rate"] == pytest.approx(0.5)
    assert out["latency_p50_s"] == 120      # child has none: parent value
    assert out["n"] == 4 and out["n_eff"] == 4.0      # counts never shrink
    assert out["shrunk"] is True
    assert isinstance(out["len_p50"], int)


# ── n thresholds ───────────────────────────────────────────────────────────

def test_thresholds_omit_small_contacts_and_buckets():
    samples = {
        "+1big": [_sample(0, 20, shape="question") for _ in range(3)]
                 + [_sample(0, 20, shape="story") for _ in range(2)]
                 + [_sample(0, 20) for _ in range(45)],
        "+2four": [_sample(0, 20) for _ in range(4)],
        "+3five": [_sample(0, 20) for _ in range(5)],
    }
    prof = lsp.build_profile(samples, "seth", NOW)
    assert prof["global"]["n"] == 59
    assert set(prof["contacts"]) == {"+1big", "+3five"}   # n<5 omitted
    b = prof["contacts"]["+1big"]["buckets"]
    assert "shape:question" in b and b["shape:question"]["n"] == 3
    assert "shape:story" not in b                           # n=2 < 3
    assert "pace:rapid" not in b
    assert prof["schema"] == "learned-style/v1"
    assert prof["window_days"] == 180 and prof["half_life_days"] == 21


# ── fixture chat.db: pairing, bubbles, attribution, pace, bands ────────────

def _persona(dirpath, contacts):
    os.makedirs(dirpath, exist_ok=True)
    with open(os.path.join(dirpath, "seth.json"), "w") as f:
        json.dump({"name": "seth", "contacts": contacts}, f)


def _pairs(fx, contact, n, start, reply_len=20, step=20 * MIN, inbound="hey you around"):
    for i in range(n):
        t = start + i * step
        fx.msg(contact, t, inbound, False)
        fx.msg(contact, t + 30, "r" * reply_len, True)


def test_load_samples_pairs_bubbles_and_excludes_huuman(tmp_path):
    fx = Fixture(str(tmp_path))
    c = "+15550000001"
    # 1) normal pair: question, reply in 2 bubbles 40 s apart, latency 45 s.
    fx.msg(c, 0, "you free?", False)
    fx.msg(c, 45, "yeah", True)
    fx.msg(c, 85, "Whats up.", True)
    # 2) a Seth follow-up 30 min later is not a reply (no inbound before it).
    fx.msg(c, 30 * MIN, "also lol", True)
    # 3) inbound then a reply 7 h later: outside the 6 h pairing window.
    fx.msg(c, 2 * 3600, "ok", False)
    fx.msg(c, 9 * 3600, "sorry just saw", True)
    # 4) an h-uman reply (memory.db assistant row) is never learned.
    fx.msg(c, 12 * 3600, "you there", False)
    fx.msg(c, 12 * 3600 + 20, "Yep I'm here what's going on", True, huuman=True)
    # 5) rapid exchange: inbound < 120 s after Seth's last send, reply in 30 s.
    fx.msg(c, 20 * 3600, "lol", False)
    fx.msg(c, 20 * 3600 + 60, "haha 😂", True)
    fx.msg(c, 20 * 3600 + 100, "stop", False)
    fx.msg(c, 20 * 3600 + 130, "no u", True)
    fx.close()
    out, att = lsp.load_samples(fx.chat_path, fx.mem_path, [c], NOW, dt.timezone.utc)
    s = out[c]
    assert len(s) == 3, s           # pairs 1, 5a, 5b only
    first = s[0]
    assert first["bubbles"] == 2
    assert first["len"] == len("yeah") + len("Whats up.")
    assert first["latency_s"] == 45
    assert first["shape"] == "question"
    assert first["lower"] is True and first["end_punct"] is True
    assert first["band"] == "day"   # 12:00 UTC
    assert first["rapid"] is False
    assert s[1]["emoji"] is True and s[1]["rapid"] is False   # "lol" came 8 h after Seth
    assert s[2]["rapid"] is True    # "stop" 40 s after Seth's "haha", reply in 30 s
    # No text survives into a sample.
    for smp in s:
        assert all(not isinstance(v, str) or v in ("question", "story", "casual",
                                                   "day", "evening", "late")
                   for v in smp.values())


def _burst_shape(tmp_path, bubbles):
    fx = Fixture(str(tmp_path))
    c = "+15550000009"
    fx.msg(c, -600, "earlier", True)                 # Seth's previous send
    for k, b in enumerate(bubbles):
        fx.msg(c, k * 20, b, False)
    fx.msg(c, len(bubbles) * 20 + 30, "ok", True)
    fx.close()
    s = lsp.load_samples(fx.chat_path, fx.mem_path, [c], NOW, dt.timezone.utc)[0][c]
    assert len(s) == 1
    return s[0]["shape"]


def test_shape_uses_the_whole_inbound_burst(tmp_path):
    # Each bubble alone is casual (< 80 bytes); joined they are a story.
    assert _burst_shape(tmp_path, ["So today was wild.",
                                   "Work ran late and the car wouldn't start.",
                                   "Ended up getting a ride home from Dave."]) == "story"


def test_question_in_an_earlier_bubble_makes_the_burst_a_question(tmp_path):
    assert _burst_shape(tmp_path, ["wait are you coming tonight?", "also bring chips"]) == "question"


def test_burst_starts_after_seths_previous_send(tmp_path):
    # The "?" was answered by Seth's previous send; it is not part of this burst.
    fx = Fixture(str(tmp_path))
    c = "+15550000009"
    fx.msg(c, -900, "you around?", False)
    fx.msg(c, -600, "yep", True)
    fx.msg(c, 0, "cool", False)
    fx.msg(c, 30, "ok", True)
    fx.close()
    s = lsp.load_samples(fx.chat_path, fx.mem_path, [c], NOW, dt.timezone.utc)[0][c]
    assert [x["shape"] for x in s] == ["question", "casual"]


def test_stale_unanswered_question_does_not_join_a_later_burst(tmp_path):
    # "dinner sunday?" Monday goes unanswered; "lol" Wednesday gets a reply.
    # The daemon's batch on Wednesday holds only "lol": casual, not question.
    fx = Fixture(str(tmp_path))
    c = "+15550000009"
    fx.msg(c, -600, "earlier", True)
    fx.msg(c, 0, "dinner sunday?", False)
    fx.msg(c, 2 * 86400, "lol", False)
    fx.msg(c, 2 * 86400 + 30, "haha", True)
    fx.close()
    s = lsp.load_samples(fx.chat_path, fx.mem_path, [c], NOW, dt.timezone.utc)[0][c]
    assert [x["shape"] for x in s] == ["casual"]
    assert s[0]["burst_changed_shape"] is False


def test_burst_gap_over_10_minutes_breaks_the_burst(tmp_path):
    # Two bubbles 11 minutes apart, both within 6 h of the reply.
    fx = Fixture(str(tmp_path))
    c = "+15550000009"
    fx.msg(c, -600, "earlier", True)
    fx.msg(c, 0, "you coming tonight?", False)
    fx.msg(c, 11 * MIN, "ok whatever", False)
    fx.msg(c, 11 * MIN + 30, "sorry", True)
    fx.close()
    s = lsp.load_samples(fx.chat_path, fx.mem_path, [c], NOW, dt.timezone.utc)[0][c]
    assert s[0]["shape"] == "casual"


def test_three_bubble_story_within_two_minutes_is_story_and_counted(tmp_path):
    fx = Fixture(str(tmp_path))
    c = "+15550000009"
    fx.msg(c, -600, "earlier", True)
    fx.msg(c, 0, "So today was wild.", False)
    fx.msg(c, 50, "Work ran late and the car wouldn't start.", False)
    fx.msg(c, 110, "Ended up getting a ride home from Dave.", False)
    fx.msg(c, 140, "oh no", True)
    fx.close()
    s = lsp.load_samples(fx.chat_path, fx.mem_path, [c], NOW, dt.timezone.utc)[0][c]
    assert s[0]["shape"] == "story"
    assert s[0]["burst_changed_shape"] is True     # last bubble alone: casual


def test_shape_changed_by_burst_n_is_logged(tmp_path):
    e = Env(tmp_path)

    def fill(fx):
        _fill_two()(fx)
        c = "+15550000001"
        base = 100 * 3600
        for k in range(3):                           # 3 replies to split questions
            t = base + k * 3600
            fx.msg(c, t, "you around later?", False)
            fx.msg(c, t + 40, "need a hand", False)
            fx.msg(c, t + 70, "yep", True)
    e.build(fill)
    assert e.run() == 0
    line = e.log_lines()[-1]
    assert line["shape_changed_by_burst_n"] == 3
    assert isinstance(line["shape_changed_by_burst_n"], int)


def test_time_bands():
    utc = dt.timezone.utc
    assert lsp.time_band(dt.datetime(2026, 1, 1, 6, 0, tzinfo=utc), utc) == "day"
    assert lsp.time_band(dt.datetime(2026, 1, 1, 17, 59, tzinfo=utc), utc) == "day"
    assert lsp.time_band(dt.datetime(2026, 1, 1, 18, 0, tzinfo=utc), utc) == "evening"
    assert lsp.time_band(dt.datetime(2026, 1, 1, 22, 59, tzinfo=utc), utc) == "evening"
    assert lsp.time_band(dt.datetime(2026, 1, 1, 23, 0, tzinfo=utc), utc) == "late"
    assert lsp.time_band(dt.datetime(2026, 1, 1, 5, 59, tzinfo=utc), utc) == "late"


def test_owner_test_contacts_are_excluded():
    contacts = {"+1": {"name": "A"}, "+2": {"name": "Me", "relationship": "test"}}
    assert lsp.learnable_contacts(contacts) == ["+1"]


# ── main(): write, cap, refusal, history, privacy ──────────────────────────

class Env:
    def __init__(self, tmp_path):
        self.tmp = tmp_path
        self.pdir = str(tmp_path / "personas")
        self.logs = str(tmp_path / "logs")
        self.contacts = {"+15550000001": {"name": "A"}, "+15550000002": {"name": "B"}}
        _persona(self.pdir, self.contacts)
        self.out = os.path.join(self.pdir, "seth.learned-style.json")

    def build(self, fill):
        self.builds = getattr(self, "builds", 0) + 1
        d = self.tmp / f"db{self.builds}"
        d.mkdir()
        fx = Fixture(str(d))
        fill(fx)
        fx.close()
        self.chat, self.mem = fx.chat_path, fx.mem_path

    def run(self, *extra):
        return lsp.main(["--persona", "seth", "--persona-dir", self.pdir,
                         "--chat-db", self.chat, "--memory-db", self.mem,
                         "--log-dir", self.logs, "--now", NOW_ISO, "--tz", "utc", *extra])

    def load(self):
        with open(self.out) as f:
            return json.load(f)

    def log_lines(self):
        with open(os.path.join(self.logs, "learned-style.jsonl")) as f:
            return [json.loads(x) for x in f]


def _fill_two(reply_len=20, n=30, inbound=SECRET_IN, reply=None):
    def fill(fx):
        for k, c in enumerate(("+15550000001", "+15550000002")):
            for i in range(n):
                t = k * 40 * 3600 + i * 20 * MIN
                fx.msg(c, t, inbound, False)
                fx.msg(c, t + 30, reply if reply else "r" * reply_len, True)
    return fill


def test_first_run_writes_atomic_0600_file_without_cap(tmp_path):
    e = Env(tmp_path)
    e.build(_fill_two(reply_len=20))
    assert not os.path.exists(e.out)
    assert e.run() == 0
    assert stat.S_IMODE(os.stat(e.out).st_mode) == 0o600
    prof = e.load()
    assert prof["global"]["n"] == 60
    assert prof["global"]["len_p50"] == 20
    assert prof["generated_at"] == NOW_ISO
    assert set(prof["contacts"]) == set(e.contacts)
    assert "shape:question" in prof["contacts"]["+15550000001"]["buckets"]
    leftovers = [f for f in os.listdir(e.pdir) if f.endswith(".tmp") or f.startswith(".")]
    assert leftovers == []
    line = e.log_lines()[-1]
    assert line["status"] == "written" and line["first_run"] is True
    assert line["contacts"] == 2 and line["clamped_n"] == 0


def test_per_run_cap_clamps_and_logs(tmp_path):
    e = Env(tmp_path)
    e.build(_fill_two(reply_len=100))
    assert e.run() == 0
    assert e.load()["global"]["len_p50"] == 100
    e.build(_fill_two(reply_len=200))
    assert e.run() == 0
    prof = e.load()
    assert prof["global"]["len_p50"] == 130          # 100 + 30% cap, not 200
    line = e.log_lines()[-1]
    assert line["clamped_n"] >= 1
    assert line["max_rel_change"] == pytest.approx(1.0, abs=1e-3)
    assert line["clamped_fields"]["len_p50"] >= 1


def test_cap_allows_10_absolute_for_short_lengths():
    prev = {"len_p25": 10, "len_p50": 20, "len_p90": 30, "bubbles_p50": 1.0,
            "lower_start_rate": 0.0, "emoji_rate": 0.5, "end_punct_rate": 0.5,
            "latency_p50_s": 100}
    new = dict(prev, len_p50=29, len_p90=60, lower_start_rate=0.9, emoji_rate=0.55)
    out, clamped, _ = lsp.cap_stats(new, prev)
    assert out["len_p50"] == 29                 # +9 <= max(6, 10): allowed
    assert out["len_p90"] == 40                 # +30 > max(9, 10): clamped to +10
    assert out["lower_start_rate"] == pytest.approx(0.05)   # prev 0: absolute floor
    assert out["emoji_rate"] == pytest.approx(0.55)          # +10% relative: allowed
    assert set(clamped) == {"len_p90", "lower_start_rate"}


def test_clamped_quantiles_stay_ordered_without_breaking_the_cap():
    # A malformed previous file (p25 > p50) is the only way clamping can
    # unorder the quantiles; the fix lowers the higher one, never raises
    # p50 / p90 past their own cap.
    prev = {"len_p25": 100, "len_p50": 40, "len_p90": 200, "bubbles_p50": 1.0,
            "lower_start_rate": 0.5, "emoji_rate": 0.5, "end_punct_rate": 0.5,
            "latency_p50_s": 60}
    new = dict(prev, len_p25=150)
    out, clamped, _ = lsp.cap_stats(new, prev)
    assert clamped == ["len_p25"]
    assert out["len_p25"] <= out["len_p50"] <= out["len_p90"]
    assert out["len_p50"] == 40                  # not raised to 130
    assert out["len_p90"] == 200


def test_cap_holds_on_every_field_for_ordered_previous_files():
    # Ruling: ordering is enforced by lowering, and it can override a cap
    # ONLY when the previous file itself was unordered. For any ordered
    # previous file every clamped field stays within its own cap.
    import random
    rng = random.Random(7)
    for _ in range(2000):
        p = sorted(rng.randint(1, 400) for _ in range(3))
        v = sorted(rng.randint(1, 600) for _ in range(3))
        prev = {"len_p25": p[0], "len_p50": p[1], "len_p90": p[2], "bubbles_p50": 1.0,
                "lower_start_rate": 0.5, "emoji_rate": 0.5, "end_punct_rate": 0.5,
                "latency_p50_s": 60}
        new = dict(prev, len_p25=v[0], len_p50=v[1], len_p90=v[2])
        out, _, _ = lsp.cap_stats(new, prev)
        assert out["len_p25"] <= out["len_p50"] <= out["len_p90"]
        for f in ("len_p25", "len_p50", "len_p90"):
            allowed = max(0.3 * prev[f], 10)
            assert abs(out[f] - prev[f]) <= allowed + 1e-9, (prev, new, out)


def _ambiguous_fill(n_ambiguous):
    """_fill_two plus n_ambiguous Seth replies that have a NON-matching
    assistant row within 15 min (attribution 'ambiguous'), and one
    outbound_sends record that never resolves (exact_unmatched = 1)."""
    base = _fill_two()

    def fill(fx):
        base(fx)
        for i in range(n_ambiguous):
            t = (T0 + dt.timedelta(seconds=i * 20 * MIN + 30)).strftime("%Y-%m-%d %H:%M:%S")
            fx.mem.execute("insert into messages(session_id,role,content,created_at) "
                           "values (?,?,?,?)",
                           ("+15550000001", "assistant", "completely unrelated draft", t))
        fx.outbound("+19990000000", 9 * 86400, "never delivered", 0)
    return fill


def test_ambiguity_and_unmatched_provenance_are_logged(tmp_path):
    e = Env(tmp_path)
    e.build(_ambiguous_fill(2))                  # 2 of 60 sends = 3.3%
    assert e.run() == 0
    line = e.log_lines()[-1]
    assert line["status"] == "written"
    assert line["ambiguous_n"] == 2 and line["sent_n"] == 60
    assert line["ambiguous_frac"] == pytest.approx(2 / 60, abs=1e-4)
    assert line["exact_unmatched"] == 1
    assert e.load()["global"]["n"] == 58         # the ambiguous turns are not learned


def test_refuses_when_ambiguous_attribution_exceeds_5_percent(tmp_path, capsys):
    e = Env(tmp_path)
    e.build(_ambiguous_fill(4))                  # 4 of 60 = 6.7% > 5%
    assert e.run() == 2
    assert not os.path.exists(e.out)
    line = e.log_lines()[-1]
    assert line["status"] == "refused_ambiguous"
    assert line["ambiguous_n"] == 4 and line["exact_unmatched"] == 1
    assert "ambiguous" in capsys.readouterr().err


def test_max_ambiguous_frac_flag_raises_the_limit(tmp_path):
    e = Env(tmp_path)
    e.build(_ambiguous_fill(4))
    assert e.run("--max-ambiguous-frac", "0.1") == 0
    assert e.log_lines()[-1]["status"] == "written"


def test_defaults_honour_hu_state_dir(tmp_path, monkeypatch):
    state = tmp_path / "state"
    monkeypatch.setenv("HU_STATE_DIR", str(state))
    a = lsp.parse_args([])
    assert a.persona_dir == str(state / "personas")
    assert a.out_dir == str(state / "personas")
    assert a.memory_db == str(state / "memory.db")
    assert a.log_dir == str(state / "logs")
    monkeypatch.setenv("HU_PERSONA_DIR", str(tmp_path / "p"))   # still wins
    assert lsp.parse_args([]).persona_dir == str(tmp_path / "p")


def test_no_cap_flag_reseeds(tmp_path):
    e = Env(tmp_path)
    e.build(_fill_two(reply_len=100))
    assert e.run() == 0
    e.build(_fill_two(reply_len=200))
    assert e.run("--no-cap") == 0
    assert e.load()["global"]["len_p50"] == 200


def test_history_keeps_previous_file_and_retains_14(tmp_path):
    e = Env(tmp_path)
    e.build(_fill_two())
    hist = os.path.join(e.pdir, "learned-style-history")
    os.makedirs(hist)
    for i in range(20):
        p = os.path.join(hist, f"seth.2026010{i // 10}T0000{i % 10:02d}Z.json")
        with open(p, "w") as f:
            f.write("{}")
    assert e.run() == 0
    first = e.load()
    assert e.run() == 0
    files = sorted(os.listdir(hist))
    assert len(files) == 14
    newest = os.path.join(hist, files[-1])
    with open(newest) as f:
        assert json.load(f) == first
    assert stat.S_IMODE(os.stat(newest).st_mode) == 0o600


def test_refuses_when_global_n_under_50(tmp_path, capsys):
    e = Env(tmp_path)
    e.build(_fill_two(n=20))                    # 40 replies
    assert e.run() == 2
    assert not os.path.exists(e.out)
    assert e.log_lines()[-1]["status"] == "refused_global_n"
    assert "50" in capsys.readouterr().err


def test_refuses_when_most_previous_contacts_vanish(tmp_path):
    e = Env(tmp_path)
    prev = {"schema": "learned-style/v1", "persona": "seth", "generated_at": NOW_ISO,
            "window_days": 180, "half_life_days": 21, "global": {},
            "contacts": {f"+1555000{i:04d}": {} for i in range(10)}}
    os.makedirs(e.pdir, exist_ok=True)
    with open(e.out, "w") as f:
        json.dump(prev, f)
    before = open(e.out).read()
    e.build(_fill_two())                         # only 2 contacts survive, 0 of prev
    assert e.run() == 2
    assert open(e.out).read() == before
    assert e.log_lines()[-1]["status"] == "refused_contacts_vanished"


def test_refuses_when_memory_db_unreadable(tmp_path):
    e = Env(tmp_path)
    e.build(_fill_two())
    e.mem = str(tmp_path / "missing.db")
    assert e.run() == 2
    assert not os.path.exists(e.out)


def test_dry_run_writes_nothing_and_prints_counts_only(tmp_path, capsys):
    e = Env(tmp_path)
    e.build(_fill_two(reply=SECRET_OUT))
    assert e.run("--dry-run") == 0
    assert not os.path.exists(e.out)
    assert not os.path.exists(e.logs)
    out = capsys.readouterr()
    summary = json.loads(out.out.strip().splitlines()[-1])
    assert summary["dry_run"] is True and summary["samples"] == 60
    assert summary["contacts"] == 2
    for v in summary.values():
        assert isinstance(v, (int, float, bool)) or v is None
    _assert_no_text(out.out + out.err)
    assert "+1555" not in out.out + out.err      # never a handle on stdout


def _leaves(node, path=()):
    if isinstance(node, dict):
        for k, v in node.items():
            yield from _leaves(v, path + (k,))
    elif isinstance(node, list):
        for i, v in enumerate(node):
            yield from _leaves(v, path + (i,))
    else:
        yield path, node


def _assert_no_text(blob):
    for secret in (SECRET_IN, SECRET_OUT):
        for i in range(0, len(secret) - 6):
            assert secret[i:i + 6] not in blob, secret[i:i + 6]


def test_output_file_has_no_text_leaves(tmp_path, capsys):
    e = Env(tmp_path)
    e.build(_fill_two(reply=SECRET_OUT))
    assert e.run() == 0
    prof = e.load()
    metadata = {("schema",): "learned-style/v1", ("persona",): "seth",
                ("generated_at",): NOW_ISO}
    leaves = list(_leaves(prof))
    assert len(leaves) > 40
    for path, v in leaves:
        if isinstance(v, str):
            assert path in metadata and metadata[path] == v, path
        else:
            assert v is None or isinstance(v, (bool, int, float)), (path, v)
    with open(e.out) as f:
        _assert_no_text(f.read())
    with open(os.path.join(e.logs, "learned-style.jsonl")) as f:
        log = f.read()
    _assert_no_text(log)
    assert "+1555" not in log                    # counts only, no handles
    out = capsys.readouterr()
    _assert_no_text(out.out + out.err)


def test_persona_name_cannot_escape_the_directory(tmp_path):
    with pytest.raises(SystemExit):
        lsp.main(["--persona", "../evil", "--persona-dir", str(tmp_path), "--dry-run"])
