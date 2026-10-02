#!/usr/bin/env python3
"""learned-style/v2 behaviour fields for scripts/learned_style_profile.py.

v1 learns how LONG Seth's replies are. v2 learns the rest of how he texts,
per contact and situation, so runtime consumers can stop using static
constants (docs/guides/learned-style.md "v2 behaviour fields"):

  latency_p25/p75/p90_s   reply delay quantiles (v1 has p50)
  bubbles_p90, inter_bubble_gap_s_p50
  double_text_rate, double_text_gap_s_p50
  tapback_only_rate, tapback_with_text_rate, tapback_types{kind}, self_reaction_rate
  voice_memo_rate, gif_rate, share_rate
  initiation_rate_per_week, initiation_share

Every value is a number with its own n / n_eff where its sample differs from
the v1 reply samples. Text exists only in memory, inside the functions that
read it; nothing here returns, logs or prints text.

Twin contamination. Text and media sends use v1's attribution labels. The
twin's TAPBACKS write no provenance until PR #611 lands (src/daemon.c
tapback-only path), so a from-me tapback cannot be attributed directly.
The daemon does save every inbound batch it handles as memory.db rows, so any response unit with
daemon activity for that contact within 15 minutes of the burst or the
response is left out of the tapback sample, whether Seth answered with a
tapback or with text: excluding only the tapback units would bias
tapback_only_rate down. If memory.db outbound_sends holds kind='tapback' rows
(the daemon writes them once PR #611 lands), each claims the bot's own tapback
and its unit is excluded exactly (load_tapback_provenance / claim_bot_tapbacks).

The m3 corpus (--extra-history) is older real history. It has no tapbacks,
attachments or chat ids, so the tapback, reaction and modality fields are
chat.db-only (CHATDB_ONLY_GROUPS_N).
"""
import bisect
import datetime as dt
import hashlib
import json
import math
import os
import re
import sqlite3
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import eval_conversation_quality as cq  # noqa: E402

PAIR_WINDOW_S = 6 * 3600
BUBBLE_GAP_S = 90
RAPID_S = 120
WINDOW_DAYS = 180
HALF_LIFE_DAYS = 21
SHRINK_K = 8
# A response unit with daemon activity this close is out of the tapback
# sample: the same window attribution uses for assistant rows.
DAEMON_NEAR_S = cq.MATCH_WINDOW_S
# A Seth turn followed by another Seth turn (more than 90 s later, so not a
# bubble of the same turn) with no reply in between, within this window
# (review ruling, PR #603: <= 2 h). It must stay below THREAD_GAP_S so a
# double text and a thread start are disjoint: an outbound after a long
# silence is an initiation, never a double text.
DOUBLE_TEXT_MAX_S = 2 * 3600
# A thread start: the first message after this much silence either way. The
# pairing window: past it, a message no longer answers anything.
THREAD_GAP_S = 6 * 3600
assert DOUBLE_TEXT_MAX_S < THREAD_GAP_S
# "Is this a reaction ROW" (associated_message_type 2000-3999: tapbacks
# 2000-2006, sticker 2007, removals 3xxx). Such a row never opens or answers
# a burst and is never a sent message. Only TAPBACK_CODES (2000-2006) are
# LEARNED as tapbacks; a sticker or removal is a reaction row learned as
# nothing (review round 2: narrowing this set turned stickers into messages).
REACTION_RANGE = frozenset(range(2000, 4000))
TAPBACK_CODES = {2000: "love", 2001: "like", 2002: "dislike", 2003: "laugh",
                 2004: "emphasize", 2005: "question", 2006: "emoji"}
TAPBACK_KINDS = tuple(TAPBACK_CODES.values())
URL_RE = re.compile(r"(?i)\bhttps?://|\bwww\.")
# Older macOS stored a tapback as a text row ("Loved “…”"); the m3 corpus
# kept those. They are reactions, not replies.
REACTION_TEXT_RE = re.compile(
    r"^(Loved|Liked|Disliked|Laughed at|Emphasized|Questioned) [“\"]|^Reacted ")

# Field groups. Each group is shrunk with its own n_eff; "sample" uses the v1
# reply samples' n / n_eff.
GROUPS = {
    "sample": ("n", "n_eff", ("latency_p25_s", "latency_p75_s", "latency_p90_s", "bubbles_p90")),
    "gap": ("inter_bubble_gap_n", "inter_bubble_gap_n_eff", ("inter_bubble_gap_s_p50",)),
    "double_text": ("double_text_n", "double_text_n_eff", ("double_text_rate",)),
    "double_text_gap": ("double_text_gap_n", "double_text_gap_n_eff", ("double_text_gap_s_p50",)),
    "tapback": ("tapback_n", "tapback_n_eff", ("tapback_only_rate", "tapback_with_text_rate")),
    "reaction": ("reaction_n", "reaction_n_eff",
                 tuple(f"tapback_types.{k}" for k in TAPBACK_KINDS) + ("self_reaction_rate",)),
    "modality": ("modality_n", "modality_n_eff", ("voice_memo_rate", "gif_rate", "share_rate")),
    "initiation": ("initiation_n", "initiation_n_eff",
                   ("initiation_rate_per_week", "initiation_share")),
}
SAMPLE_GROUPS = ("sample", "gap", "double_text", "double_text_gap")
UNIT_GROUPS = ("tapback", "reaction", "modality")
CHATDB_ONLY_GROUPS_N = ("tapback_n", "reaction_n", "modality_n")
INT_FIELDS = ("latency_p25_s", "latency_p75_s", "latency_p90_s", "inter_bubble_gap_s_p50",
              "double_text_gap_s_p50")
# Per-run cap floors (the relative cap is v1's 30%). Rates move at least
# 0.05 so a rate at 0.0 can still move; times at least a minute (latency),
# 10 s (bubble gap) or 10 minutes (double-text gap).
CAP_FLOOR = {"latency_p25_s": 60, "latency_p75_s": 60, "latency_p90_s": 60,
             "bubbles_p90": 0.5, "inter_bubble_gap_s_p50": 10, "double_text_gap_s_p50": 600,
             "initiation_rate_per_week": 0.25}
RATE_FLOOR = 0.05
VALUE_FIELDS = tuple(f for g in GROUPS.values() for f in g[2])
# Per-contact bound against the global prior (applied after shrinkage, to the
# contact overall and every bucket): probabilities and times move at most 25%
# relative from the global value (the static-rules v2 design's figure;
# probabilities also get its 0.05 absolute floor so a small global rate can
# still differ per contact). Latency quantiles are re-ordered around v1's
# unbounded latency_p50_s afterwards; ordering wins over the bound.
PRIOR_BOUND_REL = 0.25
PRIOR_PROB_FIELDS = ("double_text_rate", "tapback_only_rate", "tapback_with_text_rate",
                     "self_reaction_rate", "voice_memo_rate", "gif_rate", "share_rate",
                     "initiation_share")
PRIOR_TIME_FIELDS = ("latency_p25_s", "latency_p75_s", "latency_p90_s",
                     "inter_bubble_gap_s_p50", "double_text_gap_s_p50")
# Neither probability nor time, bounded the same way (25% relative) with the
# per-run cap's floors so a near-zero global value still lets a contact differ.
# bubbles_p90 is re-ordered around v1's unbounded bubbles_p50 (ordering wins).
PRIOR_OTHER_FIELDS = ("bubbles_p90", "initiation_rate_per_week")
PRIOR_BOUND_FIELDS = PRIOR_PROB_FIELDS + PRIOR_TIME_FIELDS + PRIOR_OTHER_FIELDS
PRIOR_BOUND_FLOOR = dict(dict.fromkeys(PRIOR_PROB_FIELDS, 0.05),
                         bubbles_p90=0.5, initiation_rate_per_week=0.25)
# Fields order_v2 may move after bounding.
ORDERED_FIELDS = ("latency_p25_s", "latency_p75_s", "latency_p90_s", "bubbles_p90")
# A contact's tapback_types mix is held within this total-variation distance
# of the global mix, then renormalised to sum to 1. The spec gives no figure
# for contact-vs-global (its 0.1 TV is the night-to-night cap), so 0.25,
# matching PRIOR_BOUND_REL.
TAPBACK_MIX_TV_CAP = 0.25
# Night-to-night cap on a mix (the v2 design's "histograms by TV <= 0.1"):
# replaces the per-share cap, which broke the mix's sum.
TAPBACK_MIX_NIGHT_TV_CAP = 0.1
# A tapback provenance record is written after delivery is confirmed (the
# send-observer contract), so the bot's chat.db row is at or BEFORE the
# record (bounded below by prior_max_rowid and EXACT_WINDOW_S); a row more
# than this skew AFTER the record is a later tapback, never this send.
TAPBACK_RECORD_SKEW_S = 30
N_FIELDS = tuple(k for g in GROUPS.values() for k in g[:2] if k not in ("n", "n_eff"))
# Top-level keys v2 adds to every stats node (tapback_types is one nested
# object). Initiation keys are absent from shape:* and pace:* buckets.
V2_FIELDS = tuple(dict.fromkeys(
    [f.split(".")[0] for f in VALUE_FIELDS] + list(N_FIELDS)))


def recency_weight(age_days):
    return 0.5 ** (max(0.0, age_days) / HALF_LIFE_DAYS)


def weighted_quantile(values, weights, q):
    pairs = sorted(zip(values, weights))
    target = q * sum(w for _, w in pairs)
    acc = 0.0
    for v, w in pairs:
        acc += w
        if acc >= target - 1e-12:
            return v
    return pairs[-1][0]


def _age(now, t):
    return (now - t).total_seconds() / 86400


def _wsum(ws):
    return round(sum(ws), 4)


# ── chat.db metadata (booleans only) and daemon activity (times only) ──────

def _columns(con, table):
    try:
        return {r[1] for r in con.execute(f"pragma table_info({table})")}
    except sqlite3.Error:
        return set()


def load_meta(chat_path, since):
    """{guid: {"audio", "gif", "media"}} for messages with any flag set.
    Tolerates a chat.db without the columns (older macOS, test fixtures)."""
    con = cq._connect_ro(chat_path)
    out = {}
    try:
        cols = _columns(con, "message")
        sel = [c for c in ("is_audio_message", "balloon_bundle_id") if c in cols]
        if sel:
            for row in con.execute(f"select guid, {', '.join(sel)} from message where date >= ?",
                                   (cq._to_ns(since),)):
                vals = dict(zip(sel, row[1:]))
                bundle = (vals.get("balloon_bundle_id") or "").lower()
                flags = {"audio": bool(vals.get("is_audio_message")),
                         "gif": "gif" in bundle,
                         "media": "urlballoon" in bundle}
                if any(flags.values()):
                    out[row[0]] = flags
        if _columns(con, "attachment") and _columns(con, "message_attachment_join"):
            for guid, mime in con.execute(
                    "select m.guid, a.mime_type from message m "
                    "join message_attachment_join j on j.message_id = m.ROWID "
                    "join attachment a on a.ROWID = j.attachment_id where m.date >= ?",
                    (cq._to_ns(since),)):
                mime = (mime or "").lower()
                f = out.setdefault(guid, {"audio": False, "gif": False, "media": False})
                if mime.startswith("audio/"):
                    f["audio"] = True
                elif mime == "image/gif":
                    f["gif"] = True
                else:
                    f["media"] = True
    finally:
        con.close()
    return out


def load_daemon_activity(mem_path, since):
    """{contact: sorted [datetime]} of every memory.db trace the daemon
    leaves for a contact: messages rows of any role (it saves each inbound
    batch it handles), proactive_sends and outbound_sends."""
    con = cq._connect_ro(mem_path)
    out = {}
    queries = (
        ("select session_id, created_at from messages where created_at >= ?",
         since.strftime("%Y-%m-%d %H:%M:%S"), "text"),
        ("select contact, sent_timestamp from proactive_sends where sent_timestamp >= ?",
         int(since.timestamp()), "s"),
        ("select contact, sent_at_ms from outbound_sends where sent_at_ms >= ?",
         int(since.timestamp() * 1000), "ms"),
    )
    try:
        for sql, arg, kind in queries:
            try:
                rows = con.execute(sql, (arg,)).fetchall()
            except sqlite3.OperationalError:
                continue                     # older memory.db without the table
            for contact, v in rows:
                try:
                    if kind == "text":
                        t = dt.datetime.strptime(v, "%Y-%m-%d %H:%M:%S").replace(
                            tzinfo=dt.timezone.utc)
                    elif kind == "s":
                        t = dt.datetime.fromtimestamp(v, dt.timezone.utc)
                    else:
                        t = dt.datetime.fromtimestamp(v / 1000, dt.timezone.utc)
                except (TypeError, ValueError, OverflowError, OSError):
                    continue
                out.setdefault(contact, []).append(t)
    finally:
        con.close()
    for v in out.values():
        v.sort()
    return out


def load_tapback_provenance(mem_path, since):
    """{contact: [(datetime, prior_max_rowid)]} of the tapbacks the daemon
    recorded sending: memory.db outbound_sends rows with kind 'tapback'.
    Empty when the table is absent or holds none. The daemon writes these
    rows once PR #611 lands; until then this returns {} in production and the
    learner relies on the daemon-activity window above. prior is -1 when the
    row has no boundary (NULL or -1 stored)."""
    con = cq._connect_ro(mem_path)
    out = {}
    try:
        rows = con.execute(
            "select contact, sent_at_ms, prior_max_rowid from outbound_sends "
            "where kind = 'tapback' and sent_at_ms >= ? order by id",
            (int(since.timestamp() * 1000),)).fetchall()
    except sqlite3.OperationalError:
        rows = []
    finally:
        con.close()
    for contact, ms, prior in rows:
        try:
            t = dt.datetime.fromtimestamp(ms / 1000, dt.timezone.utc)
        except (TypeError, ValueError, OverflowError, OSError):
            continue
        out.setdefault(contact, []).append((t, prior if prior is not None else -1))
    return out


def claim_bot_tapbacks(msgs, records):
    """Guids of from-me tapbacks the daemon sent. A record may claim a from-me
    tapback above its chat.db ROWID boundary, dated from cq.EXACT_WINDOW_S
    before the record (the send, then the record once delivered) to
    TAPBACK_RECORD_SKEW_S after it. One-sided on purpose: a tapback well after
    the record cannot be the send it reports.

    A record with no boundary (prior_max_rowid -1/None: a group target, or
    chat.db unreadable) claims nothing: by time alone it could take Seth's own
    tapback (see no_boundary_provenance_n). Claiming is one-to-one, nearest in
    time first: each tapback goes to at most one record and each record takes
    at most one tapback, so two quick reacts claim two distinct tapbacks."""
    taps = [m for m in msgs if m["from_me"] and m["atype"] in TAPBACK_CODES]
    pairs = []
    for ri, (t, prior) in enumerate(records):
        if prior is None or prior < 0:
            continue
        for ti, m in enumerate(taps):
            if m.get("rowid", prior + 1) <= prior:
                continue
            d = (m["t"] - t).total_seconds()
            if d < -cq.EXACT_WINDOW_S or d > TAPBACK_RECORD_SKEW_S:
                continue
            pairs.append((abs(d), ri, ti))
    claimed, used = set(), set()
    for _, ri, ti in sorted(pairs):
        guid = taps[ti]["guid"]
        if ri in used or guid in claimed:
            continue
        used.add(ri)
        claimed.add(guid)
    return claimed


def no_boundary_provenance_n(records):
    """Tapback provenance records with no chat.db boundary, which claim_bot_tapbacks
    skips: an aggregate count for the run log."""
    return sum(1 for _, prior in records if prior is None or prior < 0)


# ── response units (one per inbound burst Seth answered) ──────────────────

def _target(m):
    return cq._target_guid(m.get("assoc"))


def response_units(msgs, labels, meta, activity, now, tz, tapback_prov=()):
    """Seth's response to each inbound burst: every from-me event (message or
    tapback) after the contact's burst and before their next message, within
    6 h of the burst's last bubble. msgs: one contact's chat.db rows in time
    order, reactions included. tapback_prov: the daemon's own tapback records
    for this contact (load_tapback_provenance); a unit holding a tapback the
    daemon sent is the bot's and leaves the tapback sample. Returns numbers,
    booleans and kind names."""
    import learned_style_profile as lsp       # shape / bands / burst rule

    me_guids = {m["guid"] for m in msgs if m["from_me"] and m["atype"] not in REACTION_RANGE}
    bot_taps = claim_bot_tapbacks(msgs, tapback_prov)
    units, burst, resp = [], [], None
    last_me_t, inbound_gap = None, None

    def finish(r):
        events, b = r["events"], r["burst"]
        first = events[0]
        latency = (first["t"] - b[-1]["t"]).total_seconds()
        kept = lsp.burst_kept([(m["t"], m["text"] or "") for m in b], first["t"])
        lo = (kept[0][0] if kept else b[-1]["t"]) - dt.timedelta(seconds=DAEMON_NEAR_S)
        hi = events[-1]["t"] + dt.timedelta(seconds=DAEMON_NEAR_S)
        i = bisect.bisect_left(activity, lo)
        near = i < len(activity) and activity[i] <= hi
        sent = [e for e in events if e["atype"] not in REACTION_RANGE]
        taps = [e for e in events if e["atype"] in TAPBACK_CODES]
        self_r = [TAPBACK_CODES[e["atype"]] for e in taps if _target(e) in me_guids]
        to_them = [TAPBACK_CODES[e["atype"]] for e in taps if _target(e) not in me_guids]
        att_ok = all(labels.get(e["guid"]) == "seth" for e in sent)
        bot = sum(1 for e in taps if e["guid"] in bot_taps)
        flags = [meta.get(e["guid"], {}) for e in sent]
        if to_them:
            kind = "tapback_text" if sent else "tapback_only"
        else:
            kind = "text" if sent else "other"
        units.append({
            "age_days": _age(now, first["t"]),
            "latency_s": int(latency),
            "shape": lsp.shape("\n".join(t for _, t in kept)),
            "band": lsp.time_band(first["t"], tz),
            "rapid": r["inbound_gap"] is not None and r["inbound_gap"] < RAPID_S
                     and latency <= RAPID_S,
            "kind": kind,
            "reactions": to_them,
            "self_reactions": self_r,
            "att_ok": att_ok,
            "tap_ok": att_ok and not near and not bot,
            "daemon_near": near,
            "daemon_tapback": bool(bot),
            "tapbacks_n": len(taps),
            "bot_tapbacks_n": bot,
            "has_msg": bool(sent),
            "voice": any(f.get("audio") for f in flags),
            "gif": any(f.get("gif") for f in flags),
            "share": any(f.get("media") for f in flags)
                     or any(URL_RE.search(e["text"] or "") for e in sent),
        })

    for m in msgs:
        react = m["atype"] in REACTION_RANGE
        if not m["from_me"]:
            if react:
                continue                      # the contact's tapbacks are not a burst
            if resp:
                finish(resp)
                resp, burst = None, []
            inbound_gap = (m["t"] - last_me_t).total_seconds() if last_me_t else None
            burst.append(m)
            continue
        if burst and (m["t"] - burst[-1]["t"]).total_seconds() <= PAIR_WINDOW_S:
            if resp is None:
                resp = {"burst": burst, "events": [], "inbound_gap": inbound_gap}
            resp["events"].append(m)
        elif not react and resp is None:
            burst = []                        # Seth spoke; that burst went unanswered
        if not react:
            last_me_t = m["t"]
    if resp:
        finish(resp)
    return units


# ── stats ─────────────────────────────────────────────────────────────────

def _rate(items, ws, key):
    tot = sum(ws)
    return round(sum(w for x, w in zip(items, ws) if x[key]) / tot, 4) if tot else None


def unit_stats(units):
    """Tapback, reaction and modality fields from response units."""
    out = {}
    tap = [u for u in units if u["tap_ok"]]
    w = [recency_weight(u["age_days"]) for u in tap]
    out["tapback_n"], out["tapback_n_eff"] = len(tap), _wsum(w)
    tot = sum(w)
    for f, kind in (("tapback_only_rate", "tapback_only"), ("tapback_with_text_rate", "tapback_text")):
        out[f] = round(sum(wi for u, wi in zip(tap, w) if u["kind"] == kind) / tot, 4) if tot else None
    rx = [(k, wi, False) for u, wi in zip(tap, w) for k in u["reactions"]]
    rx += [(k, wi, True) for u, wi in zip(tap, w) for k in u["self_reactions"]]
    rtot = sum(wi for _, wi, _ in rx)
    out["reaction_n"], out["reaction_n_eff"] = len(rx), round(rtot, 4)
    for k in TAPBACK_KINDS:
        out[f"tapback_types.{k}"] = (round(sum(wi for kk, wi, _ in rx if kk == k) / rtot, 4)
                                     if rtot else None)
    out["self_reaction_rate"] = round(sum(wi for _, wi, s in rx if s) / rtot, 4) if rtot else None
    mod = [u for u in units if u["att_ok"] and u["has_msg"]]
    mw = [recency_weight(u["age_days"]) for u in mod]
    out["modality_n"], out["modality_n_eff"] = len(mod), _wsum(mw)
    out["voice_memo_rate"] = _rate(mod, mw, "voice")
    out["gif_rate"] = _rate(mod, mw, "gif")
    out["share_rate"] = _rate(mod, mw, "share")
    return out


def sample_stats(samples):
    """Latency / bubble / double-text fields from v1 reply samples."""
    out = {}
    w = [recency_weight(s["age_days"]) for s in samples]
    if samples:
        lat = [s["latency_s"] for s in samples]
        for f, q in (("latency_p25_s", 0.25), ("latency_p75_s", 0.75), ("latency_p90_s", 0.90)):
            out[f] = int(weighted_quantile(lat, w, q))
        out["bubbles_p90"] = float(weighted_quantile([s["bubbles"] for s in samples], w, 0.9))
    else:
        out.update(dict.fromkeys(("latency_p25_s", "latency_p75_s", "latency_p90_s",
                                  "bubbles_p90")))
    gaps = [(g, wi) for s, wi in zip(samples, w) for g in s.get("gaps", ())]
    out["inter_bubble_gap_n"] = len(gaps)
    out["inter_bubble_gap_n_eff"] = _wsum(wi for _, wi in gaps)
    out["inter_bubble_gap_s_p50"] = (int(weighted_quantile([g for g, _ in gaps],
                                                           [wi for _, wi in gaps], 0.5))
                                     if gaps else None)
    dts = [(s, wi) for s, wi in zip(samples, w) if s.get("double_text") is not None]
    out["double_text_n"] = len(dts)
    out["double_text_n_eff"] = _wsum(wi for _, wi in dts)
    out["double_text_rate"] = _rate([s for s, _ in dts], [wi for _, wi in dts], "double_text")
    gp = [(s["double_text_gap_s"], wi) for s, wi in dts if s["double_text"]]
    out["double_text_gap_n"] = len(gp)
    out["double_text_gap_n_eff"] = _wsum(wi for _, wi in gp)
    out["double_text_gap_s_p50"] = (int(weighted_quantile([g for g, _ in gp],
                                                          [wi for _, wi in gp], 0.5))
                                    if gp else None)
    return out


def initiation_starts(timeline, labels, now, tz, end_age=0.0):
    """Thread starts in one contact's timeline (reactions removed): the first
    message after >= 6 h of silence either way. Returns (starts, segment)
    where segment is (oldest age, newest age) in days of the observed span,
    for the exposure. The first message has no known predecessor and is not
    counted. A from-me start the attribution does not give to Seth is
    'unknown' and counts on neither side."""
    import learned_style_profile as lsp

    if not timeline:
        return [], None
    starts = []
    for prev, m in zip(timeline, timeline[1:]):
        if (m["t"] - prev["t"]).total_seconds() < THREAD_GAP_S:
            continue
        age = _age(now, m["t"])
        if age > WINDOW_DAYS:
            continue
        if not m["from_me"]:
            who = "contact"
        else:
            who = "seth" if labels.get(m["guid"]) == "seth" else "unknown"
        starts.append({"who": who, "age_days": age, "band": lsp.time_band(m["t"], tz)})
    oldest = min(_age(now, timeline[0]["t"]), WINDOW_DAYS)
    return starts, (oldest, min(end_age, oldest))


def _exposure_days(segments):
    h = HALF_LIFE_DAYS
    return sum(h / math.log(2) * (0.5 ** (b / h) - 0.5 ** (a / h))
               for seg in segments if seg for a, b in [seg])


def initiation_stats(starts, segments):
    known = [s for s in starts if s["who"] != "unknown"]
    w = [recency_weight(s["age_days"]) for s in known]
    seth = sum(wi for s, wi in zip(known, w) if s["who"] == "seth")
    weeks = _exposure_days(segments) / 7
    return {"initiation_n": len(known), "initiation_n_eff": _wsum(w),
            "initiation_share": round(seth / sum(w), 4) if sum(w) else None,
            "initiation_rate_per_week": round(seth / weeks, 4) if weeks > 0 else None}


# ── shrinkage, ordering, rounding, cap ────────────────────────────────────

def shrink_v2(child, parent, groups=tuple(GROUPS)):
    """(n_eff * v + K * parent) / (n_eff + K) per group, with the group's own
    n_eff. A group with no data takes the parent's value; its n stays 0."""
    out = dict(child)
    for g in groups:
        _, ne_key, fields = GROUPS[g]
        ne = child.get(ne_key) or 0.0
        for f in fields:
            if f not in child and f not in parent:
                continue
            v, p = child.get(f), parent.get(f)
            if v is None or p is None:
                out[f] = p if v is None else v
            else:
                out[f] = (ne * v + SHRINK_K * p) / (ne + SHRINK_K)
    return finish_v2(out)


def order_v2(st):
    """Keep the new quantiles ordered around v1's, never moving a v1 field."""
    p50 = st.get("latency_p50_s")
    if p50 is not None:
        if st.get("latency_p25_s") is not None:
            st["latency_p25_s"] = min(st["latency_p25_s"], p50)
        if st.get("latency_p90_s") is not None:
            st["latency_p90_s"] = max(st["latency_p90_s"], p50)
        if st.get("latency_p75_s") is not None:
            hi = st["latency_p90_s"] if st.get("latency_p90_s") is not None else st["latency_p75_s"]
            st["latency_p75_s"] = min(max(st["latency_p75_s"], p50), max(hi, p50))
    if st.get("bubbles_p90") is not None and st.get("bubbles_p50") is not None:
        st["bubbles_p90"] = max(st["bubbles_p90"], st["bubbles_p50"])
    return st


def round_v2(st):
    for f in VALUE_FIELDS:
        v = st.get(f)
        if v is None:
            continue
        st[f] = int(round(v)) if f in INT_FIELDS else round(float(v), 4)
    return st


def finish_v2(st):
    return order_v2(round_v2(st))


def flatten(st):
    out = {k: v for k, v in st.items() if k != "tapback_types"}
    for k, v in (st.get("tapback_types") or {}).items():
        out[f"tapback_types.{k}"] = v
    return out


def nest(st):
    out = {k: v for k, v in st.items() if not k.startswith("tapback_types.")}
    types = {k.split(".", 1)[1]: v for k, v in st.items() if k.startswith("tapback_types.")}
    if types:
        out["tapback_types"] = types
    return out


def cap_v2(new, prev):
    """Clamp each v2 value of `new` (a nested stats node) to within
    max(30% of the previous value, its floor). Returns (capped nested copy,
    [clamped field names])."""
    import learned_style_profile as lsp

    fn, fp = flatten(new), flatten(prev) if isinstance(prev, dict) else {}
    clamped = []
    for f in VALUE_FIELDS:
        if f.startswith("tapback_types."):
            continue                 # a distribution: TV-capped in enforce_global_bounds
        v, p = fn.get(f), fp.get(f)
        if v is None or not _num(p):
            continue
        lo, hi = run_interval(f, p)
        if v < lo or v > hi:
            fn[f] = hi if v > p else lo
            clamped.append(f)
    return nest(finish_v2(fn)), clamped


def _num(x):
    return isinstance(x, (int, float)) and not isinstance(x, bool)


def _grid(lo, hi, f):
    """Round an interval INWARD to what the file stores (integers or 4 dp),
    so rounding a clamped value never moves it outside the interval."""
    if f in INT_FIELDS:
        return math.ceil(lo - 1e-9), math.floor(hi + 1e-9)
    return math.ceil(lo * 1e4 - 1e-6) / 1e4, math.floor(hi * 1e4 + 1e-6) / 1e4


def run_interval(f, p):
    """The per-run (night-to-night) interval for field f around the previous
    value p: max(30% of p, the field's floor)."""
    import learned_style_profile as lsp

    allowed = max(lsp.CAP_REL * abs(p), CAP_FLOOR.get(f, RATE_FLOOR))
    return _grid(p - allowed, p + allowed, f)


def bound_interval(f, g):
    """The global-prior interval for field f around the global value g."""
    allowed = max(PRIOR_BOUND_REL * abs(g), PRIOR_BOUND_FLOOR.get(f, 0.0))
    lo, hi = g - allowed, g + allowed
    if f in PRIOR_PROB_FIELDS:
        lo, hi = max(lo, 0.0), min(hi, 1.0)
    return _grid(lo, hi, f)


def max_rel_v2(new, prev):
    fn, fp = flatten(new), flatten(prev) if isinstance(prev, dict) else {}
    rels = [abs(fn[f] - fp[f]) / abs(fp[f]) for f in VALUE_FIELDS
            if isinstance(fn.get(f), (int, float)) and isinstance(fp.get(f), (int, float))
            and not isinstance(fp.get(f), bool) and fp[f] != 0]
    return max(rels) if rels else None


# ── per-contact bounds against the global prior ───────────────────────────

def bound_to_global(node, glob):
    """Clamp node's PRIOR_BOUND_FIELDS (in place) to within
    max(PRIOR_BOUND_REL * |global|, floor) of the global value; probabilities
    stay in [0, 1], times stay integers rounded toward the global value.
    Returns (node, [clamped field names])."""
    return _bound(node, glob, None)[:2]


def _bound(node, glob, prev):
    """bound_to_global, restricted to the per-run interval around prev's value
    when prev (the previous night's node) has one: the result then satisfies
    both whenever the two intervals meet. When they do not (prev itself lies
    outside the bound), stability wins and the field is reported overridden.
    Returns (node, [clamped], [overridden])."""
    clamped, overridden = [], []
    for f in PRIOR_BOUND_FIELDS:
        v, p = node.get(f), (glob or {}).get(f)
        if v is None or p is None:
            continue
        lo, hi = bound_interval(f, p)
        q = (prev or {}).get(f)
        if _num(q):
            a, b = run_interval(f, q)
            if max(lo, a) <= min(hi, b):
                lo, hi = max(lo, a), min(hi, b)
            else:
                lo, hi = a, b
                overridden.append(f)
        if v < lo - 1e-12:
            node[f] = lo
        elif v > hi + 1e-12:
            node[f] = hi
        else:
            continue
        clamped.append(f)
    return node, clamped, overridden


def _normalise(mix):
    """The mix with negatives clipped and summing to 1, or None if it has a
    missing kind or no mass."""
    if not isinstance(mix, dict) or any(mix.get(k) is None for k in TAPBACK_KINDS):
        return None
    vals = {k: max(0.0, float(mix[k])) for k in TAPBACK_KINDS}
    tot = sum(vals.values())
    return {k: v / tot for k, v in vals.items()} if tot > 0 else None


def tv_distance(p, q):
    return 0.5 * sum(abs(p[k] - q[k]) for k in TAPBACK_KINDS)


def cap_mix(mix, gmix, cap=TAPBACK_MIX_TV_CAP):
    """Pull mix toward gmix along the straight line until their
    total-variation distance is at most cap, then renormalise. Returns
    (mix or None, capped)."""
    p, g = _normalise(mix), _normalise(gmix)
    if p is None or g is None:
        return p, False
    tv = tv_distance(p, g)
    if tv <= cap:
        return p, False
    s = cap / tv
    return _normalise({k: g[k] + s * (p[k] - g[k]) for k in TAPBACK_KINDS}), True


def night_cap_mix(mix, prev, cap=TAPBACK_MIX_NIGHT_TV_CAP):
    """Move from the previous night's mix toward `mix` until their TV is at
    most cap (a point on the segment, so it stays a distribution)."""
    p = _normalise(prev)
    if mix is None or p is None:
        return mix
    tv = tv_distance(mix, p)
    if tv <= cap:
        return mix
    t = cap / tv
    return _normalise({k: p[k] + t * (mix[k] - p[k]) for k in TAPBACK_KINDS})


def _contact_nodes(doc):
    for c, entry in (doc.get("contacts") or {}).items():
        if not isinstance(entry, dict):
            continue
        if isinstance(entry.get("overall"), dict):
            yield (c, "overall"), entry["overall"]
        for b, st in (entry.get("buckets") or {}).items():
            if isinstance(st, dict):
                yield (c, b), st


def enforce_global_bounds(doc, prev_doc=None):
    """Bound every contact node to the global prior and cap its tapback mix
    (TV <= TAPBACK_MIX_TV_CAP from the global mix, sum 1). Run after
    shrinkage, and again after the per-run cap with prev_doc: then every
    value is also kept inside its night-to-night interval (mixes: TV <=
    TAPBACK_MIX_NIGHT_TV_CAP from the previous mix), so the written file
    satisfies both caps whenever that is possible; where it is not, the
    per-run cap wins and prior_bound_overridden_n counts it. order_v2
    re-orders latency/bubble quantiles last (prior_order_overridden_n counts
    a quantile ordering pushed outside its bound). Returns counts only."""
    out = {"prior_clamped_n": 0, "mix_capped_n": 0, "prior_bound_overridden_n": 0,
           "prior_order_overridden_n": 0}
    glob = doc.get("global")
    if not isinstance(glob, dict):
        return out
    prev_doc = prev_doc if isinstance(prev_doc, dict) else {}
    pglob = prev_doc.get("global") if isinstance(prev_doc.get("global"), dict) else {}
    gmix = night_cap_mix(_normalise(glob.get("tapback_types")), pglob.get("tapback_types"))
    if gmix is not None:
        glob["tapback_types"] = gmix
    prev_nodes = dict(_contact_nodes(prev_doc))
    for path, st in _contact_nodes(doc):
        pv = prev_nodes.get(path) or {}
        _, clamped, overridden = _bound(st, glob, pv)
        before = {f: st.get(f) for f in ORDERED_FIELDS}
        order_v2(st)
        for f in ORDERED_FIELDS:
            if st.get(f) != before[f] and glob.get(f) is not None:
                lo, hi = bound_interval(f, glob[f])
                out["prior_order_overridden_n"] += not (lo <= st[f] <= hi)
        out["prior_clamped_n"] += len(clamped)
        out["prior_bound_overridden_n"] += len(overridden)
        mix, capped = cap_mix(st.get("tapback_types"), gmix)
        mix = night_cap_mix(mix, pv.get("tapback_types"))
        if mix is not None:
            st["tapback_types"] = mix
            if gmix is not None and tv_distance(mix, gmix) > TAPBACK_MIX_TV_CAP + 1e-9:
                out["prior_bound_overridden_n"] += 1
        out["mix_capped_n"] += capped
    return out


# ── node assembly ─────────────────────────────────────────────────────────

def raw_node(samples, units, starts, segments, with_initiation=True):
    st = dict(sample_stats(samples))
    st.update(unit_stats(units))
    if with_initiation:
        st.update(initiation_stats(starts, segments))
    return round_v2(st)        # ordered after shrinkage (shrink_v2 / order_v2)


# ── --extra-history: the m3 corpus ─────────────────────────────────────────

def hash8(handle):
    """scripts/m3_extract_corpus.py hash_handle(): sha256, 8 hex, no salt."""
    return hashlib.sha256(handle.encode("utf-8")).hexdigest()[:8]


def _memdb_true_t(ts_ms, tz):
    """The generator parsed memory.db created_at (UTC) with time.mktime,
    i.e. as LOCAL time. Undo it: true = parsed + local UTC offset."""
    e = dt.datetime.fromtimestamp(ts_ms / 1000, dt.timezone.utc)
    off = (e.astimezone(tz) if tz else e.astimezone()).utcoffset() or dt.timedelta(0)
    return e + off


def load_extra_history(path, contacts, assistant_rows, now, tz, chat_min_t):
    """Older real history from scripts/m3_extract_corpus.py output (JSONL,
    keys channel, content, handle, role, ts_ms).

    Roles (m3_extract_corpus.py): imessage 'user' = the contact,
    imessage 'assistant' = ANY is_from_me row, which includes the twin's own
    iMessage sends; memory_db 'assistant'/'daemon' = the twin's replies
    (relabelled 'daemon' since 2026-09-03). So corpus sends are attributed
    exactly like chat.db sends (cq._label_send), against the live memory.db
    assistant rows plus the corpus's own memory_db twin rows, and only
    'seth' sends are learned. memory_db rows are never learned from.

    Returns ({contact: timeline}, counts). counts['_labels'] maps synthetic
    guids to labels and counts['_end_t'] is the newest corpus timestamp;
    both are private (keys starting '_') and never logged."""
    hmap = {hash8(c): c for c in contacts}
    counts = dict.fromkeys(("extra_rows_n", "extra_reaction_rows_dropped",
                            "extra_unmapped_dropped", "extra_twin_rows",
                            "extra_overlap_dropped", "extra_out_of_window",
                            "extra_sent_n", "extra_ambiguous_n", "extra_huuman_n",
                            "extra_used_n"), 0)
    tls, twin, end_t = {}, {}, None
    with open(path, encoding="utf-8", errors="replace") as f:
        for i, line in enumerate(f):
            try:
                r = json.loads(line)
                ts = int(r.get("ts_ms") or 0)
            except (ValueError, TypeError):
                continue
            counts["extra_rows_n"] += 1
            if ts <= 0:
                continue
            ch, role = r.get("channel"), r.get("role")
            c = hmap.get(r.get("handle") or "")
            content = r.get("content") or ""
            t = dt.datetime.fromtimestamp(ts / 1000, dt.timezone.utc)
            if ch == "memory_db":
                if role in ("assistant", "daemon") and c:
                    twin.setdefault(c, []).append((_memdb_true_t(ts, tz), cq._norm(content)))
                    counts["extra_twin_rows"] += 1
                continue
            if ch != "imessage" or role not in ("user", "assistant", "seth"):
                continue
            end_t = t if end_t is None or t > end_t else end_t
            if not c:
                counts["extra_unmapped_dropped"] += 1
                continue
            if REACTION_TEXT_RE.match(content):
                counts["extra_reaction_rows_dropped"] += 1
                continue
            if chat_min_t is not None and t >= chat_min_t:
                counts["extra_overlap_dropped"] += 1
                continue
            if _age(now, t) > WINDOW_DAYS:
                counts["extra_out_of_window"] += 1
                continue
            tls.setdefault(c, []).append({"guid": f"xh{i}", "from_me": role != "user", "t": t,
                                          "text": content, "atype": 0, "assoc": None})
    labels = {}
    for c, tl in tls.items():
        tl.sort(key=lambda m: m["t"])
        rows = list(assistant_rows.get(c, [])) + twin.get(c, [])
        for m in tl:
            if not m["from_me"]:
                continue
            label = cq._label_send(m, rows)
            labels[m["guid"]] = label
            counts["extra_sent_n"] += 1
            counts["extra_ambiguous_n"] += label == "ambiguous"
            counts["extra_huuman_n"] += label == "huuman"
        counts["extra_used_n"] += len(tl)
    counts["_labels"] = labels
    counts["_end_t"] = end_t
    return tls, counts
