"""Gold (spec §4.3): where Seth replied, a critique of h-uman's reply against
his real one; where h-uman replied and Seth did not, a reference reply stored
UNRATED. Nothing here is trained on or sent."""
import collections
import csv
import datetime as dt
import glob
import json
import os
import re

from . import store

CRITIQUE_VERSION = "critique-v1"
REFERENCE_VERSION = "reference-v1"
GAPS = ("specific_detail", "tone", "length", "question_vs_statement", "other")
CRITIQUE_SYSTEM = (
    "You compare two text-message replies to the same message. One is from the real person, "
    "one from an AI imitating him. Say what the AI reply is missing compared with the real "
    'one. Output only JSON: {"gaps": [...], "missing": "<one sentence>", "severity": 1-3}, '
    "where gaps are chosen from " + json.dumps(list(GAPS)) + ".")
REFERENCE_SYSTEM = (
    "Write the next text message this person would send in the conversation below. Use a "
    "detail from the notes only if it fits naturally. Reply with the message text only: one "
    "or two short lines, no quotes.")


def weak_items(run_dir):
    key = json.load(open(os.path.join(run_dir, "answer_key.json")))
    triples = {t["id"]: t for t in json.load(open(os.path.join(run_dir, "triples.json")))}
    human, synth = [], []
    for path in sorted(glob.glob(os.path.join(run_dir, "*.csv"))):
        with open(path, newline="") as f:
            rows = list(csv.DictReader(f))
        judged = any((r.get("judge_model") or "").strip() for r in rows)
        for r in rows:
            iid, ch = r.get("id"), (r.get("choice") or "").strip().upper()
            if ch not in ("A", "B") or iid not in key or iid not in triples or ch != key[iid]:
                continue
            if judged:
                synth.append(iid)
                continue
            try:
                conf = int(float(r.get("confidence") or 0))
            except ValueError:
                conf = 0
            if conf >= 4:
                human.append(iid)
    out, seen = [], set()
    for src, ids in (("human", human), ("synthetic", synth)):
        for iid in ids:
            if iid not in seen:
                seen.add(iid)
                out.append((iid, src, triples[iid]))
    return out


def parse_critique(text):
    m = re.search(r"\{[\s\S]*\}", text or "")
    try:
        obj = json.loads(m.group(0)) if m else None
    except ValueError:
        obj = None
    if not isinstance(obj, dict):
        return ["other"], "", None, True
    gaps = [g for g in (obj.get("gaps") or []) if g in GAPS] or ["other"]
    try:
        sev = max(1, min(3, int(obj.get("severity"))))
    except (TypeError, ValueError):
        sev = None
    return gaps, str(obj.get("missing") or "")[:300], sev, False


def unanswered_daemon_replies(att, window_h=24):
    out = []
    window = dt.timedelta(hours=window_h)
    for contact, labeled in att["labeled"].items():
        seth_times = [m["t"] for m, lab in labeled if lab == "seth"]
        timeline = att["timelines"].get(contact, [])
        for m, lab in labeled:
            if lab != "huuman":
                continue
            if any(m["t"] < t <= m["t"] + window for t in seth_times):
                continue
            ctx = [x for x in timeline if x["t"] < m["t"]][-6:]
            out.append((contact, m, ctx))
    out.sort(key=lambda x: x[1]["t"], reverse=True)
    return out


def contact_notes(mem, contact_id, wide_live):
    """The same selection hu_contact_insights_render makes: live, confidence >= 0.5,
    newest 8, curator_wide rows only when HU_INSIGHT_WIDE=live."""
    rows = mem.execute(
        "SELECT insight FROM contact_insights WHERE contact_id = ? AND retired_at_ms = 0"
        " AND confidence >= 0.5 AND (? OR source IS NULL OR source NOT LIKE 'curator_wide%')"
        " ORDER BY as_of_ms DESC, id DESC LIMIT 8", (contact_id, 1 if wide_live else 0))
    return [r[0] for r in rows]


def _utcnow():
    return dt.datetime.now(dt.timezone.utc)


def gold_pass(store_con, backend, run_dirs, att, mem, limit, deadline=None, now=None,
              wide_live=False):
    now = now or _utcnow
    c = {k: 0 for k in ("critiques", "references", "unparseable", "errors", "attempted",
                        "stopped_at_deadline")}

    def out_of_time():
        if deadline is not None and now() >= deadline:
            c["stopped_at_deadline"] = 1
            return True
        return False

    done = store.critiqued_items(store_con, backend.name)
    todo = [w for d in run_dirs for w in weak_items(d) if w[0] not in done]
    for iid, src, t in todo[:limit]:
        if out_of_time():
            return c
        c["attempted"] += 1
        user = (f"Message they replied to:\n{t['context']}\n\nReal reply:\n{t['seth_reply']}"
                f"\n\nAI reply:\n{t['huuman_reply']}")
        try:
            raw = backend.generate(CRITIQUE_SYSTEM, user, max_tokens=200)
        except Exception:
            c["errors"] += 1
            continue
        gaps, missing, sev, bad = parse_critique(raw)
        c["critiques"] += store.add_critique(store_con, iid, src, gaps, missing, sev, bad,
                                             backend.name, CRITIQUE_VERSION, store.now_ms())
        c["unparseable"] += int(bad)

    if att is None:
        return c
    have = store.referenced_rowids(store_con, backend.name)
    todo_r = [x for x in unanswered_daemon_replies(att) if x[1]["rowid"] not in have]
    for contact, m, ctx in todo_r[:limit]:
        if out_of_time():
            return c
        c["attempted"] += 1
        convo = "\n".join(f"{'me' if x['from_me'] else 'them'}: {x['text']}" for x in ctx)
        notes = contact_notes(mem, contact, wide_live)
        user = ("Notes about them:\n" + ("\n".join(f"- {n}" for n in notes) or "(none)")
                + f"\n\nConversation:\n{convo}\n\nNext message from me:")
        try:
            reply = backend.generate(REFERENCE_SYSTEM, user, max_tokens=120).strip()
        except Exception:
            c["errors"] += 1
            continue
        if not reply:
            c["unparseable"] += 1
            continue
        c["references"] += store.add_reference(store_con, contact, int(m["rowid"]), convo, reply,
                                               backend.name, REFERENCE_VERSION, store.now_ms())
    return c


def gold_report(store_con, since_ms=0):
    gaps = collections.Counter()
    crit = unp = 0
    for g, bad in store_con.execute("SELECT gaps, unparseable FROM critiques"
                                    " WHERE created_at_ms >= ?", (since_ms,)):
        crit += 1
        unp += bad
        gaps.update(json.loads(g))
    refs = store_con.execute("SELECT COUNT(*) FROM reference_replies WHERE created_at_ms >= ?",
                             (since_ms,)).fetchone()[0]
    return {"critiques": crit, "references": refs, "unparseable": unp, "gaps": dict(gaps)}
