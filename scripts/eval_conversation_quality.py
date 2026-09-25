#!/usr/bin/env python3
"""Conversation quality from real behavior: do people keep talking after h-uman?

Headline metric: DEAD-END RATE -- the share of turns that get no reply from the
contact within 24 hours. Compared between h-uman's turns and Seth's own turns,
per contact, from chat.db (what was delivered) and memory.db (what h-uman
generated). Secondary: thread depth, reply latency, reply length, positive
tapbacks.

Why a behavioral metric (2026-09-24): every promotion signal so far rewards
sounding like Seth's AVERAGE text (LUAR authorship n=37, blind-A/B proxy n=50,
a human verdict 57 days stale). None of them asks whether the other person
kept talking, which is the "no follow-through, conversation dies" complaint.

Attribution (validated by probe on 2026-09-25: 99/157 logged replies matched a
delivered send once attributedBody is decoded):
  * h-uman send: an is_from_me send whose decoded text matches a memory.db
    `assistant` row for the same contact within +/-15 min.
  * Seth send:   an is_from_me send with NO assistant row for that contact
    within +/-15 min.
  * Otherwise AMBIGUOUS (h-uman was active but the delivered text differs --
    split, restyled, or a guard rewrite). Dropped and counted, never guessed:
    guessing would leak h-uman replies into Seth's arm.
EXACT attribution supersedes that heuristic from the first memory.db
`outbound_sends` row onward (daemon send provenance, one row per delivered
iMessage): a record claims the first is_from_me row for its contact with
ROWID above its pre-send boundary, within 5 min, whose text matches; every
unclaimed send after that point is Seth's. Records that never resolve are
counted (`exact_unmatched_records`) -- a rising count means provenance and
chat.db have drifted. Sends from outside the daemon (e.g. another tool
texting as Seth) would still count as Seth's.
Consecutive sends (gaps <= 60 min, no contact message between) form one TURN;
a turn is h-uman or Seth only if every send in it agrees. Turns younger than
24 h are CENSORED (not yet had the chance to get a reply). Group chats are
excluded. Tapbacks are never replies.

This is comparative, not causal: h-uman answers when Seth is busy, which may
be different conversations. Contacts are paired (both arms required) and the
time-of-day mix of each arm is reported so the confound is visible.

Refuses (exit 2, writes nothing) when either arm has fewer than --min-turns
turns or fewer than --min-contacts paired contacts
(.claude/rules/no-number-without-a-measurement.md). Output never contains
message text.

Usage:
  python3 scripts/eval_conversation_quality.py [--since 2026-08-01] [--split 2026-09-25]
"""
import argparse
import datetime as dt
import json
import os
import random
import re
import sqlite3
import statistics
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "blind_ab"))
from imessage_text import msg_text  # noqa: E402

APPLE_EPOCH = dt.datetime(2001, 1, 1, tzinfo=dt.timezone.utc)
MATCH_WINDOW_S = 15 * 60
REPLY_WINDOW_S = 24 * 3600
THREAD_GAP_S = 60 * 60
POSITIVE_TAPBACKS = {2000, 2001, 2003}  # love, like, laugh
REACTION_RANGE = range(2000, 4000)      # tapbacks (2xxx) and their removals (3xxx)


def _norm(s):
    s = re.sub(r"[^a-z0-9 ]+", "", (s or "").lower())
    return re.sub(r"\s+", " ", s).strip()


def _texts_match(logged_norm, delivered_norm):
    """Containment on normalized text, tolerant of the egress stages
    (punctuation strip, casing) and of burst splitting: each delivered PART
    of a split reply is looked up anywhere in the FULL logged reply, not just
    its opening -- otherwise part 2+ of every split reply lands in
    'ambiguous' and the h-uman arm keeps only unsplit replies."""
    if not logged_norm or not delivered_norm:
        return False
    return logged_norm[:12] in delivered_norm or delivered_norm[:12] in logged_norm


def _to_ns(t):
    return int((t - APPLE_EPOCH).total_seconds() * 1e9)


def _from_ns(ns):
    return APPLE_EPOCH + dt.timedelta(seconds=ns / 1e9)


def _target_guid(assoc):
    # "p:0/GUID" or "bp:GUID"
    return assoc.split("/")[-1].split(":")[-1] if assoc else None


def _load_assistant(mem_path, since):
    con = sqlite3.connect(f"file:{mem_path}?mode=ro", uri=True)
    out = {}
    for sid, content, created in con.execute(
            "select session_id, content, created_at from messages where role='assistant' "
            "and created_at >= ?", (since.strftime("%Y-%m-%d %H:%M:%S"),)):
        try:
            t = dt.datetime.strptime(created, "%Y-%m-%d %H:%M:%S").replace(tzinfo=dt.timezone.utc)
        except (TypeError, ValueError):
            continue
        out.setdefault(sid, []).append((t, _norm(content)))
    # Proactive check-ins are h-uman sends that write no `assistant` row.
    # Register them as activity with no text: a nearby send can then never
    # match (never h-uman) and is never counted as Seth's -- it goes ambiguous.
    try:
        for contact, ts in con.execute(
                "select contact, sent_timestamp from proactive_sends where sent_timestamp >= ?",
                (int(since.timestamp()),)):
            out.setdefault(contact, []).append(
                (dt.datetime.fromtimestamp(ts, dt.timezone.utc), ""))
    except sqlite3.OperationalError:
        pass  # older memory.db without the table
    con.close()
    return out


def _load_messages(chat_path, since):
    con = sqlite3.connect(f"file:{chat_path}?mode=ro", uri=True)
    per_contact = {}
    for (rowid, guid, text, body, contact, from_me, date, room, assoc, atype) in con.execute(
            "select m.ROWID, m.guid, m.text, m.attributedBody, h.id, m.is_from_me, m.date, "
            "m.cache_roomnames, m.associated_message_guid, m.associated_message_type "
            "from message m join handle h on m.handle_id = h.ROWID "
            "where m.date >= ? and (m.cache_roomnames is null or m.cache_roomnames = '') "
            "order by m.date", (_to_ns(since),)):
        per_contact.setdefault(contact, []).append({
            "rowid": rowid, "guid": guid, "from_me": bool(from_me), "t": _from_ns(date),
            "text": msg_text(text, body) or "", "assoc": assoc, "atype": atype or 0,
        })
    con.close()
    return per_contact


# Exact provenance: memory.db outbound_sends, written by the daemon for every
# DELIVERED iMessage (src/daemon/daemon_send_provenance.c). A record resolves
# to the first unclaimed is_from_me row for its contact with ROWID above the
# pre-send boundary, within this window of the record time, whose text matches.
EXACT_WINDOW_S = 5 * 60


def _load_outbound(mem_path, since):
    """Per-contact outbound_sends records (sorted by time) and the time of the
    first record ever — provenance is complete only from then on. (None, None)
    for a memory.db that predates the table."""
    con = sqlite3.connect(f"file:{mem_path}?mode=ro", uri=True)
    try:
        first = con.execute("select min(sent_at_ms) from outbound_sends").fetchone()[0]
        rows = con.execute(
            "select contact, sent_at_ms, prior_max_rowid, text from outbound_sends "
            "where sent_at_ms >= ? order by sent_at_ms, id", (int(since.timestamp() * 1000),)
        ).fetchall()
    except sqlite3.OperationalError:
        con.close()
        return None, None
    con.close()
    if first is None:
        return {}, None
    out = {}
    for contact, ms, prior, text in rows:
        out.setdefault(contact, []).append(
            (dt.datetime.fromtimestamp(ms / 1000, dt.timezone.utc), prior, _norm(text)))
    return out, dt.datetime.fromtimestamp(first / 1000, dt.timezone.utc)


def _resolve_exact(timeline, records):
    """Claim chat.db rows for outbound records. Returns (claimed guids,
    number of records that found no delivered row)."""
    claimed, unmatched = set(), 0
    sends = [m for m in timeline if m["from_me"]]
    for t, prior, text in records:
        hit = None
        for m in sends:
            if m["guid"] in claimed or (prior is not None and prior >= 0 and m["rowid"] <= prior):
                continue
            if abs((m["t"] - t).total_seconds()) > EXACT_WINDOW_S:
                continue
            if not text or _texts_match(text, _norm(m["text"])):
                hit = m
                break
        if hit:
            claimed.add(hit["guid"])
        else:
            unmatched += 1
    return claimed, unmatched


def _label_send(msg, assistant_rows, exact_guids=frozenset(), exact_from=None):
    if msg["guid"] in exact_guids:
        return "huuman"
    if exact_from is not None and msg["t"] >= exact_from:
        # Provenance is complete from its first record on: every delivered
        # h-uman send has a row, so an unclaimed send is Seth's.
        return "seth"
    near = [n for (t, n) in assistant_rows
            if abs((t - msg["t"]).total_seconds()) <= MATCH_WINDOW_S]
    if not near:
        return "seth"
    delivered = _norm(msg["text"])
    return "huuman" if any(_texts_match(n, delivered) for n in near) else "ambiguous"


def analyze(chat_path, mem_path, since, now):
    """Per-turn outcomes for every 1:1 contact. Returns counts + per_turn rows
    (no message text)."""
    assistant = _load_assistant(mem_path, since)
    outbound, exact_from = _load_outbound(mem_path, since)
    outbound = outbound or {}
    counts = {"seth": 0, "huuman": 0, "ambiguous": 0, "censored": 0}
    per_turn = []
    messages = _load_messages(chat_path, since)
    exact_matched = 0
    # Records for contacts with no chat.db rows in the window never resolve.
    exact_unmatched = sum(len(v) for c, v in outbound.items() if c not in messages)
    for contact, msgs in messages.items():
        reactions = [m for m in msgs if m["atype"] in REACTION_RANGE]
        timeline = [m for m in msgs if m["atype"] not in REACTION_RANGE]
        exact_guids, unmatched = _resolve_exact(timeline, outbound.get(contact, []))
        exact_matched += len(exact_guids)
        exact_unmatched += unmatched
        positive_targets = {_target_guid(r["assoc"]) for r in reactions
                            if not r["from_me"] and r["atype"] in POSITIVE_TAPBACKS}
        i = 0
        while i < len(timeline):
            if not timeline[i]["from_me"]:
                i += 1
                continue
            turn = [timeline[i]]
            j = i + 1
            while (j < len(timeline) and timeline[j]["from_me"]
                   and (timeline[j]["t"] - turn[-1]["t"]).total_seconds() <= THREAD_GAP_S):
                turn.append(timeline[j])
                j += 1
            end = turn[-1]["t"]
            i = j
            if (now - end).total_seconds() < REPLY_WINDOW_S:
                counts["censored"] += 1
                continue
            labels = {_label_send(m, assistant.get(contact, []), exact_guids, exact_from)
                      for m in turn}
            arm = labels.pop() if len(labels) == 1 else "ambiguous"
            counts[arm] += 1

            after = timeline[j:]
            reply = next((m for m in after if not m["from_me"]
                          and (m["t"] - end).total_seconds() <= REPLY_WINDOW_S), None)
            depth, prev_me, prev_t = 0, True, end
            for m in after:
                if (m["t"] - prev_t).total_seconds() > THREAD_GAP_S:
                    break
                if m["from_me"] != prev_me:
                    depth += 1
                    prev_me = m["from_me"]
                prev_t = m["t"]
            per_turn.append({
                "arm": arm, "contact": contact, "end": end.isoformat(),
                "hour_local": end.astimezone().hour,
                "dead_end": reply is None,
                "latency_s": None if reply is None else round((reply["t"] - end).total_seconds()),
                "reply_len": None if reply is None else len(reply["text"]),
                "depth": depth,
                "positive_tapback": any(m["guid"] in positive_targets for m in turn),
            })
    per_turn.sort(key=lambda r: r["end"])
    return {"turns": counts, "per_turn": per_turn,
            "attribution": {"exact_from": exact_from.isoformat() if exact_from else None,
                            "exact_matched": exact_matched,
                            "exact_unmatched_records": exact_unmatched}}


def _rate(rows):
    return sum(r["dead_end"] for r in rows) / len(rows) if rows else None


def _tod(rows):
    buckets = {"night": 0, "morning": 0, "afternoon": 0, "evening": 0}
    for r in rows:
        h = r.get("hour_local", 12)
        buckets["night" if h < 6 else "morning" if h < 12 else "afternoon" if h < 18 else "evening"] += 1
    n = max(len(rows), 1)
    return {k: round(v / n, 3) for k, v in buckets.items()}


def _secondary(rows):
    lat = [r["latency_s"] for r in rows if r.get("latency_s") is not None]
    return {
        "median_depth": statistics.median([r.get("depth", 0) for r in rows]) if rows else None,
        "median_reply_latency_s": statistics.median(lat) if lat else None,
        "positive_tapback_rate": (round(sum(bool(r.get("positive_tapback")) for r in rows) / len(rows), 3)
                                  if rows else None),
    }


def summarize(rows, min_turns=30, min_contacts=3, seed=1234, n_boot=2000):
    """Dead-end rate per arm over PAIRED contacts, with a contact-cluster
    bootstrap CI on (huuman - seth). INSUFFICIENT means no rates are reported."""
    arms = {"seth": [r for r in rows if r["arm"] == "seth"],
            "huuman": [r for r in rows if r["arm"] == "huuman"]}
    paired = sorted({r["contact"] for r in arms["seth"]} & {r["contact"] for r in arms["huuman"]})
    p = {a: [r for r in arms[a] if r["contact"] in paired] for a in arms}
    out = {"contacts_paired": len(paired), "turns_paired": {a: len(p[a]) for a in p}}
    if len(paired) < min_contacts or min(len(p["seth"]), len(p["huuman"])) < min_turns:
        out.update(verdict="INSUFFICIENT",
                   reason=(f"need >= {min_contacts} paired contacts and >= {min_turns} turns per arm; "
                           f"have {len(paired)} contacts, {out['turns_paired']}"),
                   dead_end_rate={"seth": None, "huuman": None}, delta_ci95=None)
        return out
    rate = {a: _rate(p[a]) for a in p}
    by_contact = {c: {a: [r for r in p[a] if r["contact"] == c] for a in p} for c in paired}
    rng = random.Random(seed)
    deltas = []
    for _ in range(n_boot):
        sample = [rng.choice(paired) for _ in paired]
        s = [r for c in sample for r in by_contact[c]["seth"]]
        h = [r for c in sample for r in by_contact[c]["huuman"]]
        if s and h:
            deltas.append(_rate(h) - _rate(s))
    deltas.sort()
    lo, hi = deltas[int(0.025 * (len(deltas) - 1))], deltas[int(0.975 * (len(deltas) - 1))]
    direction = ("huuman_more_dead_ends" if lo > 0 else
                 "huuman_fewer_dead_ends" if hi < 0 else "no_detectable_difference")
    out.update(verdict="MEASURED", direction=direction,
               dead_end_rate={a: round(rate[a], 4) for a in rate},
               delta_huuman_minus_seth=round(rate["huuman"] - rate["seth"], 4),
               delta_ci95=[round(lo, 4), round(hi, 4)],
               secondary={a: _secondary(p[a]) for a in p},
               time_of_day_mix={a: _tod(p[a]) for a in p})
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--memory-db", default=os.path.expanduser("~/.human/memory.db"))
    ap.add_argument("--since", default=None, help="YYYY-MM-DD (default: 60 days ago)")
    ap.add_argument("--split", default=None,
                    help="YYYY-MM-DD: also report h-uman turns before vs after this date "
                         "(each against the same Seth arm)")
    ap.add_argument("--min-turns", type=int, default=30)
    ap.add_argument("--min-contacts", type=int, default=3)
    ap.add_argument("--out", default=None)
    a = ap.parse_args(argv)

    now = dt.datetime.now(dt.timezone.utc)
    since = (dt.datetime.strptime(a.since, "%Y-%m-%d").replace(tzinfo=dt.timezone.utc)
             if a.since else now - dt.timedelta(days=60))
    res = analyze(a.chat_db, a.memory_db, since, now)
    rows = res["per_turn"]
    summary = {"all": summarize(rows, a.min_turns, a.min_contacts)}
    if a.split:
        cut = dt.datetime.strptime(a.split, "%Y-%m-%d").replace(tzinfo=dt.timezone.utc).isoformat()
        seth = [r for r in rows if r["arm"] == "seth"]
        for name, keep in (("before", lambda r: r["end"] < cut), ("after", lambda r: r["end"] >= cut)):
            summary[name] = summarize(seth + [r for r in rows if r["arm"] == "huuman" and keep(r)],
                                      a.min_turns, a.min_contacts)

    head = summary["all"]
    print(f"turns: {res['turns']}  paired contacts: {head['contacts_paired']}")
    att = res["attribution"]
    print(f"attribution: exact from {att['exact_from'] or '(no outbound_sends yet)'}; "
          f"{att['exact_matched']} sends resolved, {att['exact_unmatched_records']} records unresolved")
    if head["verdict"] != "MEASURED":
        print(f"INSUFFICIENT: {head['reason']} -- no verdict written", file=sys.stderr)
        return 2
    print(f"dead-end rate  seth={head['dead_end_rate']['seth']:.3f}  "
          f"h-uman={head['dead_end_rate']['huuman']:.3f}  "
          f"delta={head['delta_huuman_minus_seth']:+.3f}  CI95={head['delta_ci95']}  ({head['direction']})")
    out = a.out or os.path.expanduser(
        f"~/.human/logs/conversation-quality-{now.strftime('%Y-%m-%d')}.json")
    with open(out, "w") as f:
        json.dump({
            "generated_at": now.isoformat(),
            "provenance": {"source": "chat.db (delivered) + memory.db (assistant rows)",
                           "rater": "behavioral: contacts' replies, not a judge",
                           "attribution": ("exact via memory.db outbound_sends from exact_from "
                                           "on; before that, text match within +/-15 min with "
                                           "ambiguous dropped"),
                           "exact": res["attribution"],
                           "since": since.isoformat(), "split": a.split},
            "turn_counts": res["turns"], "summary": summary,
        }, f, indent=2)
    print(f"wrote {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
