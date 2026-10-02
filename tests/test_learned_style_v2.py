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
    assert out["tapback_types"]["love"] == pytest.approx(0.35)   # -30%
    assert set(clamped) == {"latency_p90_s", "tapback_only_rate", "tapback_types.love"}


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
    meta = {("schema",): "learned-style/v2", ("persona",): "seth", ("generated_at",): NOW_ISO}
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
