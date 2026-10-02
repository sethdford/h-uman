"""Tests for the learned-style/v2 behaviour fields
(scripts/learned_style_profile.py + scripts/learned_style_v2.py,
docs/guides/learned-style.md "v2 behaviour fields").

Hermetic: synthetic chat.db / memory.db / m3-corpus.jsonl built in a temp
dir, HOME redirected. Never the real chat.db, ~/.human, the network or a
daemon port.
"""
import datetime as dt
import hashlib
import json
import math
import os
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import learned_style_profile as lsp  # noqa: E402
import learned_style_v2 as v2  # noqa: E402
from test_eval_conversation_quality import Fixture, MIN, T0, apple_ns  # noqa: E402

NOW = T0 + dt.timedelta(days=10)
NOW_ISO = NOW.strftime("%Y-%m-%dT%H:%M:%SZ")
UTC = dt.timezone.utc
HOUR = 3600
A, B = "+15550000001", "+15550000002"
SECRET_IN = "zebra quokka inbound marker?"
SECRET_OUT = "platypus narwhal reply marker"
SECRET_OLD = "wombat axolotl corpus marker"


@pytest.fixture(autouse=True)
def _isolate_home(tmp_path, monkeypatch):
    monkeypatch.setenv("HOME", str(tmp_path / "home"))
    monkeypatch.delenv("HU_PERSONA_DIR", raising=False)
    monkeypatch.delenv("HU_STATE_DIR", raising=False)


class Fx(Fixture):
    """The shared fixture plus the chat.db columns v2 reads (reactions from
    Seth, audio, attachments, balloons) and memory.db daemon activity."""

    def __init__(self, d):
        super().__init__(d)
        self.chat.executescript(
            "alter table message add column is_audio_message integer default 0;"
            "alter table message add column cache_has_attachments integer default 0;"
            "alter table message add column balloon_bundle_id text;"
            "alter table message add column associated_message_emoji text;"
            "create table attachment(ROWID integer primary key, mime_type text);"
            "create table message_attachment_join(message_id integer, attachment_id integer);")

    def raw(self, contact, secs, from_me, text=None, **cols):
        self.n += 1
        guid = f"G{self.n}"
        keys = ["guid", "text", "handle_id", "is_from_me", "date"] + list(cols)
        vals = [guid, text, self.handle(contact), 1 if from_me else 0,
                apple_ns(T0 + dt.timedelta(seconds=secs))] + list(cols.values())
        cur = self.chat.execute(
            f"insert into message({','.join(keys)}) values ({','.join('?' * len(vals))})", vals)
        return guid, cur.lastrowid

    def react(self, contact, secs, target, kind=2000, from_me=True, emoji=None):
        return self.raw(contact, secs, from_me, associated_message_guid=f"p:0/{target}",
                        associated_message_type=kind, associated_message_emoji=emoji)[0]

    def audio(self, contact, secs):
        g, rid = self.raw(contact, secs, True, "￼", is_audio_message=1,
                          cache_has_attachments=1)
        self._attach(rid, "audio/x-m4a")
        return g

    def media(self, contact, secs, mime):
        g, rid = self.raw(contact, secs, True, "￼", cache_has_attachments=1)
        self._attach(rid, mime)
        return g

    def _attach(self, rid, mime):
        cur = self.chat.execute("insert into attachment(mime_type) values (?)", (mime,))
        self.chat.execute("insert into message_attachment_join values (?,?)", (rid, cur.lastrowid))

    def daemon(self, contact, secs, role="user"):
        self.mem.execute("insert into messages(session_id,role,content,created_at) values (?,?,?,?)",
                         (contact, role, "daemon saw something",
                          (T0 + dt.timedelta(seconds=secs)).strftime("%Y-%m-%d %H:%M:%S")))


def _units(fx, contact=A):
    fx.close()
    att, meta, act = v2_inputs(fx)
    return v2.response_units(att["messages"][contact], att["labels"], meta,
                             act.get(contact, []), NOW, UTC)


def v2_inputs(fx):
    since = NOW - dt.timedelta(days=lsp.WINDOW_DAYS)
    att = lsp.cq.attribute(fx.chat_path, fx.mem_path, since)
    meta = v2.load_meta(fx.chat_path, since)
    act = v2.load_daemon_activity(fx.mem_path, since)
    return att, meta, act


# ── golden: v1 fields are byte-identical to the v1 learner ─────────────────

def _rich_fill(fx):
    """A mixed fixture: questions, stories, rapid pairs, multi-bubble turns,
    evening/late replies, emoji, one h-uman reply, two contacts."""
    for k, c in enumerate((A, B)):
        base = k * 50 * HOUR
        for i in range(32):
            t = base + i * 40 * MIN
            if i % 4 == 0:
                fx.msg(c, t, "you coming tonight?", False)
            elif i % 4 == 1:
                fx.msg(c, t, "So today was wild. Work ran late and then the car wouldn't "
                             "start. Ended up getting a ride home from Dave.", False)
            else:
                fx.msg(c, t, "lol ok", False)
            lat = 20 + (i * 37) % 900
            fx.msg(c, t + lat, ("Yeah " if i % 3 else "yeah ") + "r" * (5 + (i * 11) % 60)
                   + ("." if i % 5 == 0 else "") + (" 😂" if i % 7 == 0 else ""), True)
            if i % 6 == 0:
                fx.msg(c, t + lat + 40, "second bubble", True)
        fx.msg(c, base + 30 * HOUR, "you there", False)
        fx.msg(c, base + 30 * HOUR + 20, "Yep here", True, huuman=True)


def _v1_view(doc):
    keys = ("n", "n_eff", "len_p25", "len_p50", "len_p90", "bubbles_p50", "lower_start_rate",
            "emoji_rate", "end_punct_rate", "latency_p50_s", "shrunk")
    out = {"global": {k: doc["global"][k] for k in keys}, "contacts": {}}
    for c, e in doc["contacts"].items():
        out["contacts"][c] = {"overall": {k: e["overall"][k] for k in keys},
                              "buckets": {b: {k: st[k] for k in keys}
                                          for b, st in e["buckets"].items()}}
    return out


class Env:
    def __init__(self, tmp_path, contacts=None):
        self.tmp = tmp_path
        self.pdir = str(tmp_path / "personas")
        self.logs = str(tmp_path / "logs")
        os.makedirs(self.pdir, exist_ok=True)
        with open(os.path.join(self.pdir, "seth.json"), "w") as f:
            json.dump({"name": "seth", "contacts": contacts or {A: {"name": "A"},
                                                                B: {"name": "B"}}}, f)
        self.out = os.path.join(self.pdir, "seth.learned-style.json")
        self.builds = 0

    def build(self, fill):
        self.builds += 1
        d = self.tmp / f"db{self.builds}"
        d.mkdir()
        fx = Fx(str(d))
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


def test_v1_fields_are_identical_to_the_v1_learner(tmp_path):
    e = Env(tmp_path)
    e.build(_rich_fill)
    assert e.run() == 0
    golden = json.loads((Path(__file__).parent / "fixtures" /
                         "learned_style_v1_golden.json").read_text())
    assert _v1_view(e.load()) == golden


def test_schema_is_v2_and_previous_v1_file_is_still_capped(tmp_path):
    e = Env(tmp_path)
    e.build(_rich_fill)
    assert e.run() == 0
    doc = e.load()
    assert doc["schema"] == "learned-style/v2"
    # Pretend the previous file was written by v1: the cap must still apply.
    doc["schema"] = "learned-style/v1"
    doc["global"]["len_p50"] = 5
    with open(e.out, "w") as f:
        json.dump(doc, f)
    assert e.run() == 0
    assert e.load()["global"]["len_p50"] == 15            # 5 + 10 absolute floor
    assert e.log_lines()[-1]["prev"] == "ok"


# ── tapbacks ───────────────────────────────────────────────────────────────

def test_response_kinds_tapback_only_with_text_and_text(tmp_path):
    fx = Fx(str(tmp_path))
    g1 = fx.msg(A, 0, "look at this", False)
    fx.react(A, 30, g1, 2000)                              # tapback only
    g2 = fx.msg(A, 2 * HOUR, "got the job!", False)
    fx.react(A, 2 * HOUR + 10, g2, 2004)                   # tapback + text
    fx.msg(A, 2 * HOUR + 40, "huge congrats", True)
    fx.msg(A, 4 * HOUR, "dinner at 7?", False)
    fx.msg(A, 4 * HOUR + 60, "yep", True)                  # text only
    fx.msg(A, 6 * HOUR, "ok", False)
    fx.react(A, 13 * HOUR, "G0", 2001)                     # 7 h later: no response
    us = _units(fx)
    assert [u["kind"] for u in us] == ["tapback_only", "tapback_text", "text"]
    assert all(u["tap_ok"] for u in us)
    assert us[0]["reactions"] == ["love"] and us[1]["reactions"] == ["emphasize"]
    assert us[0]["latency_s"] == 30


def test_tapback_rates_kinds_and_custom_emoji(tmp_path):
    fx = Fx(str(tmp_path))
    for i in range(6):
        t = i * 2 * HOUR
        g = fx.msg(A, t, "hey", False)
        if i < 3:
            fx.react(A, t + 20, g, 2003)                   # laugh only
        elif i == 3:
            fx.react(A, t + 20, g, 2006, emoji="🔥")        # custom emoji only
        else:
            fx.msg(A, t + 30, "ha", True)
    us = _units(fx)
    st = v2.unit_stats(us)
    assert st["tapback_n"] == 6
    assert st["tapback_only_rate"] == pytest.approx(4 / 6, abs=5e-3)   # recency weights
    assert st["tapback_with_text_rate"] == 0.0
    assert st["reaction_n"] == 4
    assert st["tapback_types.laugh"] == pytest.approx(0.75, abs=5e-3)
    assert st["tapback_types.emoji"] == pytest.approx(0.25, abs=5e-3)
    assert st["tapback_types.love"] == 0.0
    assert st["self_reaction_rate"] == 0.0


def test_self_reaction_is_counted(tmp_path):
    fx = Fx(str(tmp_path))
    fx.msg(A, 0, "hey", False)
    mine = fx.msg(A, 30, "lol yes", True)
    fx.react(A, 40, mine, 2004)                            # Seth emphasizes his own text
    us = _units(fx)
    st = v2.unit_stats(us)
    assert us[0]["kind"] == "text"                         # a self-reaction is not a tapback reply
    assert st["reaction_n"] == 1 and st["self_reaction_rate"] == 1.0


def test_tapbacks_near_daemon_activity_are_excluded_from_the_tapback_sample(tmp_path):
    # The twin's tapbacks write no provenance; the daemon saves the inbound
    # batch as memory.db 'user' rows when it handles it. Any burst the daemon
    # touched is out of the tapback sample (text OR tapback, so the rate is
    # not biased toward text).
    fx = Fx(str(tmp_path))
    g = fx.msg(A, 0, "look", False)
    fx.react(A, 25, g, 2000)
    fx.daemon(A, 5)                                        # daemon handled this burst
    fx.msg(A, 2 * HOUR, "dinner?", False)
    fx.msg(A, 2 * HOUR + 30, "yes", True)
    fx.daemon(A, 2 * HOUR + 3)                             # and this one
    g3 = fx.msg(A, 5 * HOUR, "pic", False)
    fx.react(A, 5 * HOUR + 30, g3, 2001)                   # daemon not near
    us = _units(fx)
    assert [u["tap_ok"] for u in us] == [False, False, True]
    st = v2.unit_stats(us)
    assert st["tapback_n"] == 1 and st["tapback_only_rate"] == 1.0


def test_twin_text_in_a_response_excludes_the_unit(tmp_path):
    fx = Fx(str(tmp_path))
    fx.msg(A, 0, "you there", False)
    fx.msg(A, 20, "Yep here", True, huuman=True)
    us = _units(fx)
    assert us[0]["att_ok"] is False and us[0]["tap_ok"] is False
    assert v2.unit_stats(us)["tapback_n"] == 0


# ── modality ───────────────────────────────────────────────────────────────

def test_voice_gif_and_share_rates(tmp_path):
    fx = Fx(str(tmp_path))
    fx.msg(A, 0, "hey", False)
    fx.audio(A, 30)
    fx.msg(A, 2 * HOUR, "hey", False)
    fx.media(A, 2 * HOUR + 30, "image/gif")
    fx.msg(A, 4 * HOUR, "hey", False)
    fx.msg(A, 4 * HOUR + 30, "see https://example.com/x", True)
    fx.msg(A, 6 * HOUR, "hey", False)
    fx.media(A, 6 * HOUR + 30, "image/jpeg")
    fx.msg(A, 8 * HOUR, "hey", False)
    g = fx.msg(A, 8 * HOUR + 30, "plain", True)
    fx.msg(A, 10 * HOUR, "hey", False)
    fx.react(A, 10 * HOUR + 30, g, 2000)                   # tapback-only: not a message response
    st = v2.unit_stats(_units(fx))
    assert st["modality_n"] == 5
    assert st["voice_memo_rate"] == pytest.approx(0.2, abs=5e-3)    # recency weights
    assert st["gif_rate"] == pytest.approx(0.2, abs=5e-3)
    assert st["share_rate"] == pytest.approx(0.4, abs=5e-3)    # link + photo


def test_meta_tolerates_a_chat_db_without_v2_columns(tmp_path):
    fx = Fixture(str(tmp_path))                            # v1 fixture: no audio/attachment columns
    fx.msg(A, 0, "hey", False)
    fx.close()
    assert v2.load_meta(fx.chat_path, NOW - dt.timedelta(days=180)) == {}


# ── latency, bubbles, double text (on the v1 reply samples) ────────────────

def _s(age, **kw):
    s = {"age_days": age, "len": 10, "bubbles": 1, "lower": False, "emoji": False,
         "end_punct": False, "latency_s": 60, "shape": "casual", "band": "day",
         "rapid": False, "gaps": [], "double_text": False, "double_text_gap_s": None}
    s.update(kw)
    return s


def test_latency_quantiles_and_bubbles_p90():
    ss = [_s(0, latency_s=v, bubbles=b) for v, b in
          [(10, 1), (20, 1), (30, 1), (40, 2), (50, 1), (60, 1), (70, 1), (80, 3), (90, 1), (100, 4)]]
    st = v2.sample_stats(ss)
    assert (st["latency_p25_s"], st["latency_p75_s"], st["latency_p90_s"]) == (30, 80, 90)
    assert st["bubbles_p90"] == 3.0


def test_inter_bubble_gap_and_double_text_stats():
    ss = [_s(0, gaps=[10, 30]), _s(0, gaps=[50]),
          _s(0, double_text=True, double_text_gap_s=600),
          _s(0, double_text=True, double_text_gap_s=1200),
          _s(0, double_text=None)]                         # censored / unknown
    st = v2.sample_stats(ss)
    assert st["inter_bubble_gap_n"] == 3 and st["inter_bubble_gap_s_p50"] == 30
    assert st["double_text_n"] == 4
    assert st["double_text_rate"] == pytest.approx(0.5)
    assert st["double_text_gap_n"] == 2 and st["double_text_gap_s_p50"] == 600


def test_samples_carry_gaps_and_double_text(tmp_path):
    fx = Fx(str(tmp_path))
    fx.msg(A, 0, "you free?", False)
    fx.msg(A, 30, "yeah", True)
    fx.msg(A, 70, "whats up", True)                        # same turn, gap 40
    fx.msg(A, 70 + 20 * MIN, "hello?", True)               # double text 20 min later
    fx.msg(A, 3 * HOUR, "sorry", False)
    fx.msg(A, 3 * HOUR + 30, "np", True)
    fx.msg(A, 3 * HOUR + 30 + 2 * HOUR, "also", True, huuman=True)   # twin follow-up: unknown
    fx.msg(A, 8 * HOUR, "k", False)
    fx.msg(A, 8 * HOUR + 30, "cool", True)
    fx.msg(A, 9 * HOUR, "lol", False)                      # they replied: not a double text
    fx.close()
    s = lsp.load_samples(fx.chat_path, fx.mem_path, [A], NOW, UTC)[0][A]
    assert s[0]["gaps"] == [40]
    assert s[0]["double_text"] is True and s[0]["double_text_gap_s"] == 20 * MIN
    assert s[1]["double_text"] is None
    assert s[2]["double_text"] is False


def test_double_text_is_censored_near_now():
    tl = [{"guid": "a", "from_me": False, "t": NOW - dt.timedelta(minutes=10), "text": "hi"},
          {"guid": "b", "from_me": True, "t": NOW - dt.timedelta(minutes=9), "text": "yo"}]
    s = lsp.samples_from_timeline(tl, {"b": "seth"}, NOW, UTC)
    assert s[0]["double_text"] is None


# ── initiation ─────────────────────────────────────────────────────────────

def test_initiation_starts_after_six_hours_of_silence(tmp_path):
    fx = Fx(str(tmp_path))
    fx.msg(A, 0, "first ever", False)                      # first message: unknown start
    fx.msg(A, 60, "hi", True)
    fx.msg(A, 7 * HOUR, "morning", True)                   # Seth starts (gap 7 h)
    fx.msg(A, 7 * HOUR + 60, "hey", False)
    fx.msg(A, 20 * HOUR, "yo", False)                      # contact starts
    fx.msg(A, 40 * HOUR, "ping", True, huuman=True)        # twin start: unknown
    fx.msg(A, 41 * HOUR, "ok", False)
    fx.msg(A, 60 * HOUR, "hey you", True)                  # Seth starts
    fx.close()
    att = lsp.cq.attribute(fx.chat_path, fx.mem_path, NOW - dt.timedelta(days=180))
    starts, seg = v2.initiation_starts(att["timelines"][A], att["labels"], NOW, UTC)
    assert [s["who"] for s in starts] == ["seth", "contact", "unknown", "seth"]
    assert seg[0] == pytest.approx(10.0, abs=1e-6)          # first message 10 days ago
    st = v2.initiation_stats(starts, [seg])
    assert st["initiation_n"] == 3
    w = [lsp.recency_weight(s["age_days"]) for s in starts]
    assert st["initiation_share"] == pytest.approx((w[0] + w[3]) / (w[0] + w[1] + w[3]), abs=1e-4)
    h = lsp.HALF_LIFE_DAYS
    exposure_weeks = h / math.log(2) * (1 - 0.5 ** (10 / h)) / 7
    assert st["initiation_rate_per_week"] == pytest.approx((w[0] + w[3]) / exposure_weeks, abs=1e-3)


# ── shrinkage, cap and ordering of v2 fields ───────────────────────────────

def test_v2_shrink_uses_each_groups_own_n_eff():
    child = {"n": 20, "n_eff": 20.0, "tapback_n": 4, "tapback_n_eff": 4.0,
             "tapback_only_rate": 1.0, "tapback_with_text_rate": 0.0}
    parent = {"tapback_only_rate": 0.4, "tapback_with_text_rate": 0.3}
    out = v2.shrink_v2(child, parent, ("tapback",))
    assert out["tapback_only_rate"] == pytest.approx((4 * 1.0 + 8 * 0.4) / 12, abs=1e-4)
    assert out["tapback_with_text_rate"] == pytest.approx((0 + 8 * 0.3) / 12, abs=1e-4)


def test_v2_group_with_no_data_inherits_the_parent():
    child = {"tapback_n": 0, "tapback_n_eff": 0.0, "tapback_only_rate": None,
             "tapback_with_text_rate": None}
    parent = {"tapback_only_rate": 0.4, "tapback_with_text_rate": 0.3}
    out = v2.shrink_v2(child, parent, ("tapback",))
    assert out["tapback_only_rate"] == 0.4 and out["tapback_n"] == 0


def test_cap_applies_to_v2_fields_with_floors():
    prev = {"latency_p90_s": 600, "tapback_only_rate": 0.0, "initiation_rate_per_week": 2.0,
            "tapback_types": {"love": 0.5}}
    new = {"latency_p90_s": 3000, "tapback_only_rate": 0.9, "initiation_rate_per_week": 2.2,
           "tapback_types": {"love": 0.1}}
    out, clamped = v2.cap_v2(new, prev)
    assert out["latency_p90_s"] == 780                    # +30%
    assert out["tapback_only_rate"] == pytest.approx(0.05)    # floor from 0
    assert out["initiation_rate_per_week"] == pytest.approx(2.2)  # +10%: allowed
    # The mix is a distribution: capped as a whole by TV in the final pass
    # (enforce_global_bounds), never share by share here.
    assert out["tapback_types"]["love"] == pytest.approx(0.1)
    assert set(clamped) == {"latency_p90_s", "tapback_only_rate"}


def test_v2_cap_clamps_are_logged_by_main(tmp_path):
    e = Env(tmp_path)
    e.build(_rich_fill)
    assert e.run() == 0
    doc = e.load()
    p90 = doc["global"]["latency_p90_s"]
    prev = 3 * p90                                         # an ordered previous file, far above
    doc["global"]["latency_p90_s"] = prev
    with open(e.out, "w") as f:
        json.dump(doc, f)
    assert e.run() == 0
    assert e.load()["global"]["latency_p90_s"] == prev - math.floor(0.3 * prev)
    assert e.log_lines()[-1]["clamped_fields"]["latency_p90_s"] >= 1


def test_latency_quantiles_stay_ordered_around_v1_p50():
    st = {"latency_p25_s": 90, "latency_p50_s": 60, "latency_p75_s": 50, "latency_p90_s": 40,
          "bubbles_p50": 2.0, "bubbles_p90": 1.0}
    v2.order_v2(st)
    assert st["latency_p50_s"] == 60                       # v1 field never moved
    assert st["latency_p25_s"] <= 60 <= st["latency_p75_s"] <= st["latency_p90_s"]
    assert st["bubbles_p90"] >= st["bubbles_p50"]


# ── the profile: every stats group carries the v2 fields ───────────────────

def _behaviour_fill(fx):
    _rich_fill(fx)
    base = 120 * HOUR
    for i in range(8):                                     # tapbacks + memos, daemon idle
        t = base + i * 3 * HOUR
        g = fx.msg(A, t, "check this", False)
        if i % 2:
            fx.react(A, t + 15, g, 2001)
        else:
            fx.audio(A, t + 40)


def test_every_stats_group_has_the_v2_fields(tmp_path):
    e = Env(tmp_path)
    e.build(_behaviour_fill)
    assert e.run() == 0
    doc = e.load()
    nodes = [("global", doc["global"])]
    for c, ent in doc["contacts"].items():
        nodes.append((c, ent["overall"]))
        nodes += [(b, st) for b, st in ent["buckets"].items()]
    for name, st in nodes:
        for f in v2.V2_FIELDS:
            if f.startswith("initiation_") and name.split(":")[0] in ("shape", "pace"):
                assert f not in st, (name, f)
                continue
            assert f in st, (name, f)
        assert set(st["tapback_types"]) == set(v2.TAPBACK_KINDS)
    a = doc["contacts"][A]["overall"]
    assert a["tapback_n"] >= 4 and a["tapback_only_rate"] > doc["contacts"][B]["overall"]["tapback_only_rate"]
    assert a["voice_memo_rate"] > 0
    assert doc["global"]["initiation_n"] >= 1
    assert "initiation_rate_per_week" in doc["contacts"][A]["buckets"]["time:day"]


def test_output_leaves_are_numbers_only_with_v2_fields(tmp_path, capsys):
    e = Env(tmp_path)
    e.build(lambda fx: (_behaviour_fill(fx), fx.msg(A, 200 * HOUR, SECRET_IN, False),
                        fx.msg(A, 200 * HOUR + 30, SECRET_OUT, True)))
    assert e.run() == 0
    doc = e.load()
    meta = {("schema",): "learned-style/v2", ("persona",): "seth", ("generated_at",): NOW_ISO,
            ("provenance", "schema"): "learned-style/v2",
            ("provenance", "generated_at"): NOW_ISO,
            ("provenance", "window", "start"):
                (NOW - dt.timedelta(days=180)).strftime("%Y-%m-%dT%H:%M:%SZ"),
            ("provenance", "window", "end"): NOW_ISO}
    leaves = list(_leaves(doc))
    v2_leaves = [p for p, _ in leaves if p[-1] in v2.TAPBACK_KINDS or p[-1] in v2.V2_FIELDS]
    assert len(v2_leaves) > 100
    for path, v in leaves:
        if isinstance(v, str):
            assert meta.get(path) == v, path
        else:
            assert v is None or isinstance(v, (bool, int, float)), (path, v)
    blob = open(e.out).read() + open(os.path.join(e.logs, "learned-style.jsonl")).read()
    out = capsys.readouterr()
    _assert_no_text(blob + out.out + out.err)


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
    for secret in (SECRET_IN, SECRET_OUT, SECRET_OLD):
        for i in range(0, len(secret) - 6):
            assert secret[i:i + 6] not in blob, secret[i:i + 6]


# ── --extra-history: the m3 corpus (older real history) ────────────────────

def _h8(handle):
    return hashlib.sha256(handle.encode()).hexdigest()[:8]


def _corpus(path, rows):
    with open(path, "w") as f:
        for r in rows:
            f.write(json.dumps(r) + "\n")


def _old(secs):
    return int((T0 - dt.timedelta(days=60) + dt.timedelta(seconds=secs)).timestamp() * 1000)


def _corpus_rows(n=20, contact=A, twin_every=0):
    rows = []
    for i in range(n):
        t = i * 2 * HOUR
        sent = SECRET_OLD if i == 0 else "yep " + "x" * i
        rows.append({"channel": "imessage", "ts_ms": _old(t), "handle": _h8(contact),
                     "role": "user", "content": "you around?"})
        rows.append({"channel": "imessage", "ts_ms": _old(t + 50), "handle": _h8(contact),
                     "role": "assistant", "content": sent})
        if twin_every and i % twin_every == 0:
            # The daemon's own reply, logged in memory.db; the generator wrote
            # created_at (UTC) through time.mktime, i.e. as LOCAL time. Under
            # --tz utc that is the identity.
            rows.append({"channel": "memory_db", "ts_ms": _old(t + 50), "handle": _h8(contact),
                         "role": "assistant", "content": sent})
    return rows


def test_extra_history_maps_roles_and_hashed_handles(tmp_path):
    p = tmp_path / "m3.jsonl"
    rows = _corpus_rows(10) + [
        {"channel": "imessage", "ts_ms": _old(30 * HOUR), "handle": _h8(A), "role": "assistant",
         "content": "Loved “you around?”"},                  # tapback-as-text row
        {"channel": "imessage", "ts_ms": _old(31 * HOUR), "handle": "", "role": "assistant",
         "content": "group or unknown"},
        {"channel": "imessage", "ts_ms": _old(32 * HOUR), "handle": _h8("+19990000000"),
         "role": "user", "content": "not a persona contact"},
        {"channel": "memory_db", "ts_ms": _old(33 * HOUR), "handle": _h8(A), "role": "user",
         "content": "daemon copy of an inbound"}]
    _corpus(p, rows)
    tls, counts = v2.load_extra_history(str(p), [A, B], {}, NOW, UTC, chat_min_t=None)
    tl = tls[A]
    assert len(tl) == 20
    assert [m["from_me"] for m in tl[:2]] == [False, True]
    assert counts["extra_rows_n"] == len(rows)
    assert counts["extra_reaction_rows_dropped"] == 1
    assert counts["extra_unmapped_dropped"] == 2
    assert counts["extra_twin_rows"] == 0


def test_extra_history_excludes_the_twins_sends(tmp_path):
    p = tmp_path / "m3.jsonl"
    _corpus(p, _corpus_rows(10, twin_every=5))             # 2 of 10 sends are the twin's
    tls, counts = v2.load_extra_history(str(p), [A], {}, NOW, UTC, chat_min_t=None)
    labels = counts.pop("_labels")
    sent = [m for m in tls[A] if m["from_me"]]
    assert counts["extra_sent_n"] == 10 and counts["extra_twin_rows"] == 2
    assert counts["extra_huuman_n"] == 2
    assert sum(labels[m["guid"]] == "seth" for m in sent) == 8


def test_extra_history_drops_rows_overlapping_chat_db(tmp_path):
    p = tmp_path / "m3.jsonl"
    _corpus(p, _corpus_rows(10))
    cut = dt.datetime.fromtimestamp(_old(5 * 2 * HOUR) / 1000, UTC)
    tls, counts = v2.load_extra_history(str(p), [A], {}, NOW, UTC, chat_min_t=cut)
    assert len(tls[A]) == 10 and counts["extra_overlap_dropped"] == 10


def test_main_extra_history_adds_samples_but_not_tapbacks(tmp_path, capsys):
    e = Env(tmp_path)
    e.build(_behaviour_fill)
    assert e.run() == 0
    base = e.load()
    p = tmp_path / "m3.jsonl"
    _corpus(p, _corpus_rows(30))
    os.unlink(e.out)
    assert e.run("--extra-history", str(p)) == 0
    doc = e.load()
    assert doc["global"]["n"] == base["global"]["n"] + 30
    for f in v2.CHATDB_ONLY_GROUPS_N:                       # tapback/modality samples unchanged
        assert doc["global"][f] == base["global"][f], f
    line = e.log_lines()[-1]
    assert line["extra_history"] is True and line["extra_samples"] == 30
    out = capsys.readouterr()
    _assert_no_text(open(e.out).read() + json.dumps(line) + out.out + out.err)


def test_main_drops_an_ambiguous_extra_history_instead_of_learning_it(tmp_path):
    e = Env(tmp_path)
    e.build(_rich_fill)
    p = tmp_path / "m3.jsonl"
    # The daemon logged a reply next to every corpus send, but no text
    # matches: every send is ambiguous, so the whole source is dropped.
    rows = _corpus_rows(10)
    rows += [dict(r, channel="memory_db", content="totally different draft")
             for r in _corpus_rows(10) if r["role"] == "assistant"]
    _corpus(p, rows)
    assert e.run("--extra-history", str(p)) == 0
    line = e.log_lines()[-1]
    assert line["extra_refused_ambiguous"] is True and line["extra_samples"] == 0
    assert line["extra_ambiguous_n"] == 10


def test_non_utc_timezone_handling(tmp_path, monkeypatch):
    """Verify that timestamp parsing is timezone-independent."""
    fx = Fx(str(tmp_path))
    # Set a non-UTC timezone to verify _memdb_true_t doesn't depend on local TZ
    monkeypatch.setenv("TZ", "America/Los_Angeles")
    fx.msg(A, 0, "hey", False)
    fx.msg(A, 30, "hi", True)
    fx.msg(A, 2 * HOUR, "hey", False)
    fx.msg(A, 2 * HOUR + 30, "bye", True)

    # Parse with explicit UTC timezone - should give consistent results
    fx.close()
    att, meta, act = v2_inputs(fx)
    us = v2.response_units(att["messages"][A], att["labels"], meta, act.get(A, []), NOW, UTC)

    # Verify the units were created correctly regardless of TZ env
    assert len(us) == 2
    assert us[0]["latency_s"] == 30
    assert us[1]["latency_s"] == 30


def test_dry_run_prints_v2_counts_only(tmp_path, capsys):
    e = Env(tmp_path)
    e.build(_behaviour_fill)
    p = tmp_path / "m3.jsonl"
    _corpus(p, _corpus_rows(10))
    assert e.run("--dry-run", "--extra-history", str(p)) == 0
    assert not os.path.exists(e.out) and not os.path.exists(e.logs)
    out = capsys.readouterr()
    summary = json.loads(out.out.strip().splitlines()[-1])
    for k in ("tapback_units_n", "tapback_daemon_near_n", "reactions_n", "modality_n",
              "double_text_n", "initiation_starts_n", "extra_samples", "extra_sent_n"):
        assert k in summary, k
    for v in summary.values():
        assert isinstance(v, (int, float, bool)) or v is None
    _assert_no_text(out.out + out.err)
    assert "+1555" not in out.out + out.err


# ── review round 1 (PR #603) ───────────────────────────────────────────────

def test_extra_history_segment_ends_at_chat_min_t(tmp_path):
    """The corpus exposure segment must end where chat.db begins, even when
    the corpus itself runs on past it, or the overlap is counted twice."""
    import argparse

    def fill(fx):
        for i in range(6):
            fx.msg(A, i * 2 * HOUR, "you around?", False)
            fx.msg(A, i * 2 * HOUR + 50, "yep", True)

    e = Env(tmp_path, contacts={A: {"name": "A"}})
    e.build(fill)
    p = tmp_path / "m3.jsonl"
    overlap = [{"channel": "imessage", "ts_ms": int((T0 + dt.timedelta(hours=h)).timestamp() * 1000),
                "handle": _h8(A), "role": "user", "content": "still here"} for h in (3, 30)]
    _corpus(p, _corpus_rows(10) + overlap)               # corpus runs 30 h into chat.db
    samples, _, beh, attribution = lsp.load_all(e.chat, e.mem, [A], NOW, UTC)
    a = argparse.Namespace(extra_history=str(p), memory_db=e.mem, max_ambiguous_frac=0.05)
    out = lsp._extra_history(a, [A], samples, beh, attribution, NOW, UTC)
    assert out["extra_overlap_dropped"] == 2
    chat_seg, corpus_seg = beh[A]["segments"]
    chat_min_age = (NOW - T0).total_seconds() / 86400
    assert chat_seg[0] == pytest.approx(chat_min_age, abs=1e-6)
    assert corpus_seg[1] == pytest.approx(chat_min_age, abs=1e-6)   # ends at chat_min_t
    # Disjoint: total exposure equals the exposure of the union.
    assert v2._exposure_days([chat_seg, corpus_seg]) == pytest.approx(
        v2._exposure_days([(corpus_seg[0], chat_seg[1])]), rel=1e-9)


def _dt_timeline(gap):
    """Contact asks, Seth replies, then Seth's next outbound `gap` s after
    his reply with nothing inbound in between."""
    t0 = NOW - dt.timedelta(days=3)
    tl = [{"guid": "in1", "from_me": False, "t": t0, "text": "hey"},
          {"guid": "me1", "from_me": True, "t": t0 + dt.timedelta(seconds=30), "text": "yo"},
          {"guid": "me2", "from_me": True, "t": t0 + dt.timedelta(seconds=30 + gap),
           "text": "also"},
          {"guid": "in2", "from_me": False, "t": t0 + dt.timedelta(seconds=30 + gap + 60),
           "text": "ok"}]
    return tl, {"me1": "seth", "me2": "seth"}


@pytest.mark.parametrize("gap,double,start", [
    (2 * HOUR, True, False),            # <= 2 h: double text
    (2 * HOUR + 1, False, False),       # just past 2 h: neither
    (6 * HOUR - 1, False, False),       # just under the initiation gap: neither
    (6 * HOUR, False, True),            # initiation, never a double text
])
def test_double_text_window_is_2h_and_disjoint_from_initiation(gap, double, start):
    tl, labels = _dt_timeline(gap)
    s = lsp.samples_from_timeline(tl, labels, NOW, UTC)[0]
    assert s["double_text"] is double
    assert s["double_text_gap_s"] == (gap if double else None)
    starts, _ = v2.initiation_starts(tl, labels, NOW, UTC)
    is_start = any(st["who"] == "seth" for st in starts)
    assert is_start is start
    assert not (s["double_text"] and is_start)


def _unit(age=1.0, kind="tapback_only", reactions=(), **kw):
    u = {"age_days": age, "latency_s": 30, "shape": "casual", "band": "day", "rapid": False,
         "kind": kind, "reactions": list(reactions), "self_reactions": [], "att_ok": True,
         "tap_ok": True, "daemon_near": False, "daemon_tapback": False, "has_msg": False,
         "voice": False, "gif": False, "share": False}
    u.update(kw)
    return u


def _tv(p, q):
    return 0.5 * sum(abs(p[k] - q[k]) for k in v2.TAPBACK_KINDS)


def _mix_doc(cap=None):
    """Contact A only ever 'love's (always with text), B only 'laugh's
    (tapback only): each sits TV 0.5 from the pooled mix and 0.5 from the
    pooled tapback-only rate before the bounds."""
    samples = {c: [_s(1.0) for _ in range(6)] for c in (A, B)}
    beh = {A: {"units": [_unit(kind="tapback_text", has_msg=True, reactions=["love"])
                         for _ in range(40)], "starts": [],
               "segments": []},
           B: {"units": [_unit(reactions=["laugh"]) for _ in range(40)], "starts": [],
               "segments": []}}
    return lsp.build_profile(samples, "seth", NOW, beh)


def test_tapback_mix_is_tv_capped_to_global_and_sums_to_one():
    doc = _mix_doc()
    g = doc["global"]["tapback_types"]
    assert abs(sum(g.values()) - 1.0) <= 1e-9
    cap = v2.TAPBACK_MIX_TV_CAP
    assert cap == 0.25
    for c in (A, B):
        nodes = [doc["contacts"][c]["overall"]] + list(doc["contacts"][c]["buckets"].values())
        for st in nodes:
            mix = st["tapback_types"]
            assert abs(sum(mix.values()) - 1.0) <= 1e-9, mix
            assert _tv(mix, g) <= cap + 1e-9, (c, _tv(mix, g))
            assert all(v >= 0 for v in mix.values())
    # At the cap, not collapsed onto the global mix: A still leans 'love'.
    a = doc["contacts"][A]["overall"]["tapback_types"]
    assert _tv(a, g) == pytest.approx(cap, abs=1e-9)
    assert a["love"] > g["love"]


def test_daemon_tapback_provenance_excludes_its_own_tapbacks(tmp_path):
    fx = Fx(str(tmp_path))
    g1 = fx.msg(A, 0, "lol look", False)
    fx.react(A, 20, g1, 2003)                             # Seth's own tapback
    g2 = fx.msg(A, 3 * HOUR, "dinner?", False)
    prior = fx.max_rowid()
    fx.react(A, 3 * HOUR + 20, g2, 2000)                  # the daemon's tapback
    fx.outbound(A, 3 * HOUR + 21, None, prior, kind="tapback")
    fx.close()
    att, meta, _ = v2_inputs(fx)
    since = NOW - dt.timedelta(days=lsp.WINDOW_DAYS)
    prov = v2.load_tapback_provenance(fx.mem_path, since)
    assert len(prov[A]) == 1
    us = v2.response_units(att["messages"][A], att["labels"], meta, [], NOW, UTC, prov.get(A, []))
    assert [u["tap_ok"] for u in us] == [True, False]
    assert [u["daemon_tapback"] for u in us] == [False, True]
    assert v2.unit_stats(us)["tapback_types.laugh"] == 1.0   # only Seth's reaction learned
    # Without the provenance row the bot's tapback would have been learned.
    us0 = v2.response_units(att["messages"][A], att["labels"], meta, [], NOW, UTC, [])
    assert [u["tap_ok"] for u in us0] == [True, True]


def test_tapback_provenance_rows_never_claim_a_text_send(tmp_path):
    fx = Fx(str(tmp_path))
    fx.msg(A, 0, "you up?", False)
    prior = fx.max_rowid()
    fx.msg(A, 30, "yeah", True)                           # Seth's text
    fx.outbound(A, 31, None, prior, kind="tapback")       # a daemon tapback record nearby
    fx.close()
    att = lsp.cq.attribute(fx.chat_path, fx.mem_path, NOW - dt.timedelta(days=180))
    assert [lab for _, lab in att["labeled"][A]] == ["seth"]


@pytest.mark.parametrize("prior", [-1, None])
def test_unbounded_provenance_row_claims_nothing(prior):
    """A row with no chat.db boundary (group target, chat.db unreadable) could
    claim Seth's own tapback by time alone, so it claims nothing."""
    rec_t = NOW - dt.timedelta(days=1)
    msgs = [{"guid": "t", "rowid": 50, "from_me": True, "atype": 2000,
             "t": rec_t - dt.timedelta(seconds=5)}]
    assert v2.claim_bot_tapbacks(msgs, [(rec_t, prior)]) == set()
    assert v2.claim_bot_tapbacks(msgs, [(rec_t, 10)]) == {"t"}     # bounded: claimed


def _tap(guid, rowid, t):
    return {"guid": guid, "rowid": rowid, "from_me": True, "atype": 2000, "t": t}


def test_each_row_claims_the_nearest_tapback_not_the_first():
    rec_t = NOW - dt.timedelta(days=1)
    msgs = [_tap("seth", 50, rec_t - dt.timedelta(seconds=100)),   # Seth's, in the window
            _tap("bot", 51, rec_t + dt.timedelta(seconds=1))]
    assert v2.claim_bot_tapbacks(msgs, [(rec_t, 10)]) == {"bot"}


def test_two_quick_reacts_claim_two_distinct_tapbacks():
    """Claiming is one-to-one and nearest-first: two reacts 20 s apart claim
    their own two tapbacks, not Seth's earlier one plus the first bot one."""
    t1 = NOW - dt.timedelta(days=1)
    t2 = t1 + dt.timedelta(seconds=20)
    msgs = [_tap("seth", 50, t1 - dt.timedelta(seconds=100)),
            _tap("bot1", 51, t1 + dt.timedelta(seconds=1)),
            _tap("bot2", 52, t2 + dt.timedelta(seconds=1))]
    assert v2.claim_bot_tapbacks(msgs, [(t1, 10), (t2, 10)]) == {"bot1", "bot2"}
    # Two rows, one tapback: claimed once; the other row claims nothing.
    assert v2.claim_bot_tapbacks(msgs[1:2], [(t1, 10), (t2, 10)]) == {"bot1"}


def _unbounded_fill(fx):
    _behaviour_fill(fx)
    g = fx.msg(B, 300 * HOUR, "check this", False)
    fx.react(B, 300 * HOUR + 15, g, 2000)
    fx.outbound(B, 300 * HOUR + 16, None, -1, kind="tapback")     # no boundary


def test_main_logs_unbounded_provenance_rows_and_does_not_claim_them(tmp_path):
    e = Env(tmp_path)
    e.build(_unbounded_fill)
    assert e.run() == 0
    line = e.log_lines()[-1]
    assert line["tapback_provenance_rows_n"] == 1
    assert line["tapback_provenance_no_boundary_n"] == 1
    assert line["tapback_provenance_excluded_n"] == 0


def _golden_fill(fx):
    _behaviour_fill(fx)
    g = fx.msg(B, 300 * HOUR, "check this", False)
    prior = fx.max_rowid()
    fx.react(B, 300 * HOUR + 15, g, 2000)                 # the daemon's own tapback
    fx.outbound(B, 300 * HOUR + 16, None, prior, kind="tapback")


def test_main_logs_the_excluded_tapback_share_as_counts(tmp_path, capsys):
    e = Env(tmp_path)
    e.build(_golden_fill)
    assert e.run() == 0
    line = e.log_lines()[-1]
    assert line["tapback_provenance_rows_n"] == 1
    assert line["tapback_provenance_excluded_n"] == 1
    assert 0 < line["tapback_provenance_excluded_share"] < 1
    for k in ("tapback_provenance_rows_n", "tapback_provenance_excluded_n",
              "tapback_provenance_excluded_share"):
        assert isinstance(line[k], (int, float)) and not isinstance(line[k], bool)
    assert "+1555" not in json.dumps(line)


def test_contact_priors_move_at_most_25pct_from_global():
    glob = {"tapback_only_rate": 0.4, "voice_memo_rate": 0.1, "inter_bubble_gap_s_p50": 100,
            "double_text_gap_s_p50": 1000, "double_text_rate": 0.2}
    node = {"tapback_only_rate": 0.9, "voice_memo_rate": 0.0, "inter_bubble_gap_s_p50": 400,
            "double_text_gap_s_p50": 100, "double_text_rate": 0.22}
    out, clamped = v2.bound_to_global(dict(node), glob)
    assert out["tapback_only_rate"] == pytest.approx(0.5)          # 0.4 + 25%
    assert out["voice_memo_rate"] == pytest.approx(0.05)           # 0.1 - 0.05 floor
    assert out["inter_bubble_gap_s_p50"] == 125                    # 100 + 25%
    assert out["double_text_gap_s_p50"] == 750                     # 1000 - 25%
    assert out["double_text_rate"] == pytest.approx(0.22)          # inside: untouched
    assert set(clamped) == {"tapback_only_rate", "voice_memo_rate", "inter_bubble_gap_s_p50",
                            "double_text_gap_s_p50"}


def test_built_profile_contacts_stay_within_the_global_prior_bound():
    doc = _mix_doc()
    g = doc["global"]
    # B never tapbacks with text; A always does: raw rates 0 and 1.
    assert g["tapback_only_rate"] == pytest.approx(0.5)
    for c in (A, B):
        for st in [doc["contacts"][c]["overall"]] + list(doc["contacts"][c]["buckets"].values()):
            for f in v2.PRIOR_BOUND_FIELDS:
                v, p = st.get(f), g.get(f)
                if v is None or p is None:
                    continue
                allowed = max(v2.PRIOR_BOUND_REL * abs(p), v2.PRIOR_BOUND_FLOOR.get(f, 0.0))
                assert abs(v - p) <= allowed + 1e-9, (c, f, v, p)


PROVENANCE_GOLDEN = Path(__file__).parent / "fixtures" / "learned_style_v2_provenance_golden.json"


def test_provenance_block_matches_golden(tmp_path):
    e = Env(tmp_path)
    e.build(_golden_fill)
    p = tmp_path / "m3.jsonl"
    _corpus(p, _corpus_rows(12))
    assert e.run("--extra-history", str(p)) == 0
    prov = e.load()["provenance"]
    if os.environ.get("UPDATE_GOLDEN") == "1":
        PROVENANCE_GOLDEN.write_text(json.dumps(prov, indent=2, sort_keys=True) + "\n")
    assert prov == json.loads(PROVENANCE_GOLDEN.read_text())
    assert prov["schema"] == "learned-style/v2"
    assert prov["window"]["end"] == NOW_ISO


# ── review round 2 (PR #603) ───────────────────────────────────────────────

def test_sticker_rows_2007_open_no_units_and_are_not_tapbacks(tmp_path):
    """2007 (sticker) is a reaction ROW like every 2000-3999 type: it never
    opens or answers a burst, never counts as a sent message, and is not a
    learned tapback kind (TAPBACK_CODES is 2000-2006)."""
    fx = Fx(str(tmp_path))
    fx.msg(A, 0, "hey", False)
    g1 = fx.msg(A, 30, "yo", True)                        # unit 1: text
    fx.react(A, HOUR, g1, 2007, from_me=False)            # their sticker: no burst
    fx.react(A, HOUR + 20, g1, 2007)                      # Seth's sticker, nothing to answer
    g2 = fx.msg(A, 3 * HOUR, "lol", False)
    fx.react(A, 3 * HOUR + 15, g2, 2007)                  # unit 2: sticker-only response
    us = _units(fx)
    assert [u["kind"] for u in us] == ["text", "other"]
    assert us[1]["has_msg"] is False and us[1]["att_ok"] is True
    assert us[1]["latency_s"] == 15
    assert all(u["reactions"] == [] and u["self_reactions"] == [] for u in us)
    st = v2.unit_stats(us)
    assert st["reaction_n"] == 0 and st["modality_n"] == 1
    assert len(lsp.load_samples(fx.chat_path, fx.mem_path, [A], NOW, UTC)[0][A]) == 1


def test_written_file_respects_the_per_run_cap_and_logs_bound_overrides(tmp_path):
    """Previous night far outside the global bound: stability wins (the
    per-run cap holds in the written file), the bound override is counted,
    and max_rel_change_written describes the file actually written."""
    e = Env(tmp_path)
    e.build(_behaviour_fill)
    assert e.run() == 0
    prev = e.load()
    for ent in prev["contacts"].values():
        ent["overall"]["tapback_only_rate"] = 1.0
        ent["overall"]["tapback_types"] = dict.fromkeys(v2.TAPBACK_KINDS, 0.0) | {"dislike": 1.0}
    with open(e.out, "w") as f:
        json.dump(prev, f)
    assert e.run() == 0
    out = e.load()
    for c, ent in out["contacts"].items():
        st, pv = ent["overall"], prev["contacts"][c]["overall"]
        assert abs(st["tapback_only_rate"] - 1.0) <= max(0.3 * 1.0, 0.05) + 1e-9
        assert v2.tv_distance(st["tapback_types"], pv["tapback_types"]) <= \
            v2.TAPBACK_MIX_NIGHT_TV_CAP + 1e-9
        assert abs(sum(st["tapback_types"].values()) - 1.0) <= 1e-9
    line = e.log_lines()[-1]
    assert line["prior_bound_overridden_n"] >= 2
    assert line["max_rel_change_written"] == pytest.approx(
        lsp.max_rel_written(out, prev), abs=1e-4)
    assert line["max_rel_change_written"] <= 0.3 + 1e-4 or line["max_rel_change_written"] < \
        line["max_rel_change"]


def test_bound_and_per_run_cap_both_hold_when_prev_is_inside_the_bound(tmp_path):
    e = Env(tmp_path)
    e.build(_behaviour_fill)
    assert e.run() == 0
    prev = e.load()
    g = prev["global"]
    pa = prev["contacts"][A]["overall"]
    pa["tapback_only_rate"] = round(g["tapback_only_rate"] * 1.2, 4)   # inside the bound
    with open(e.out, "w") as f:
        json.dump(prev, f)
    e.build(lambda fx: (_behaviour_fill(fx), [fx.react(A, 400 * HOUR + i, fx.msg(
        A, 400 * HOUR + i - 10, "x", False), 2000) for i in range(0, 20 * 3 * HOUR, 3 * HOUR)]))
    assert e.run() == 0
    out = e.load()
    st, go = out["contacts"][A]["overall"], out["global"]
    assert abs(st["tapback_only_rate"] - pa["tapback_only_rate"]) <= \
        max(0.3 * pa["tapback_only_rate"], 0.05) + 1e-9
    assert abs(st["tapback_only_rate"] - go["tapback_only_rate"]) <= \
        max(0.25 * go["tapback_only_rate"], 0.05) + 1e-9
    assert e.log_lines()[-1]["prior_bound_overridden_n"] == 0


def test_initiation_rate_and_bubbles_p90_are_bounded_too():
    glob = {"initiation_rate_per_week": 2.0, "bubbles_p90": 2.0}
    node = {"initiation_rate_per_week": 8.0, "bubbles_p90": 6.0}
    out, clamped = v2.bound_to_global(dict(node), glob)
    assert out["initiation_rate_per_week"] == pytest.approx(2.5)   # +25%
    assert out["bubbles_p90"] == pytest.approx(2.5)                # +25%
    assert set(clamped) == {"initiation_rate_per_week", "bubbles_p90"}


def test_latency_ordering_wins_over_the_bound_and_is_counted():
    """v1's latency_p50_s is never moved, so a bounded p25/p90 that would
    cross it is re-ordered onto it: ordering wins, and it is counted."""
    glob = {"latency_p25_s": 20, "latency_p50_s": 40, "latency_p75_s": 60,
            "latency_p90_s": 100, "bubbles_p50": 1.0, "bubbles_p90": 2.0}
    lo = {"latency_p25_s": 5, "latency_p50_s": 10, "latency_p75_s": 12, "latency_p90_s": 14,
          "bubbles_p50": 1.0, "bubbles_p90": 2.0}
    hi = {"latency_p25_s": 150, "latency_p50_s": 200, "latency_p75_s": 250,
          "latency_p90_s": 300, "bubbles_p50": 1.0, "bubbles_p90": 2.0}
    doc = {"global": glob, "contacts": {A: {"overall": dict(lo), "buckets": {}},
                                        B: {"overall": dict(hi), "buckets": {}}}}
    counts = v2.enforce_global_bounds(doc)
    a, b = doc["contacts"][A]["overall"], doc["contacts"][B]["overall"]
    assert a["latency_p50_s"] == 10 and b["latency_p50_s"] == 200      # v1 untouched
    assert a["latency_p25_s"] == 10                     # bound says >= 15; ordering says <= 10
    assert b["latency_p90_s"] == 200                    # bound says <= 125; ordering says >= 200
    for st in (a, b):
        assert st["latency_p25_s"] <= st["latency_p50_s"] <= st["latency_p75_s"] \
            <= st["latency_p90_s"]
    assert counts["prior_order_overridden_n"] >= 2


@pytest.mark.parametrize("offset,claimed", [
    (-60, True),          # the bot's row, stamped before its record (delivery first)
    (10, True),           # chat.db date a little after the record: within the skew
    (120, False),         # well after the record: a later tapback, not this send
])
def test_bot_tapback_window_is_one_sided(offset, claimed):
    rec_t = NOW - dt.timedelta(days=1)
    msgs = [{"guid": "t", "rowid": 50, "from_me": True, "atype": 2000,
             "t": rec_t + dt.timedelta(seconds=offset)}]
    assert (v2.claim_bot_tapbacks(msgs, [(rec_t, 10)]) == {"t"}) is claimed


def test_bot_tapback_before_the_rowid_boundary_is_never_claimed():
    rec_t = NOW - dt.timedelta(days=1)
    msgs = [{"guid": "t", "rowid": 9, "from_me": True, "atype": 2000,
             "t": rec_t - dt.timedelta(seconds=5)}]
    assert v2.claim_bot_tapbacks(msgs, [(rec_t, 10)]) == set()
