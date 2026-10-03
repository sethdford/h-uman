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

from . import judge, store

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


def _run_usable(run_dir):
    return (os.path.isfile(os.path.join(run_dir, "answer_key.json"))
            and os.path.isfile(os.path.join(run_dir, "triples.json")))


def weak_items(run_dir, judged_csv=None, judge_model=None):
    """(item_id, weak_source, triple) for the moments h-uman was caught.

    Human detections (confidence >= 4) come from the un-judged sheets in the
    blind-A/B run dir. Synthetic detections come ONLY from `judged_csv` -- the
    lane's own judge output -- and only from rows stamped with `judge_model`
    (the current backend's model id); judged sheets inside the run dir are
    ignored, because their judge may be the prod model family. The lane sheet
    is used only when its source.json records this run dir's basename and
    answer-key hash (judge.lane_sheet_matches); otherwise synthetic detections
    count as zero. weak_source is "human" or "synthetic:<judge_model>". A run dir without answer_key.json or
    triples.json yields [] (the operator hasn't copied triples in yet)."""
    if not _run_usable(run_dir):
        return []
    with open(os.path.join(run_dir, "answer_key.json")) as f:
        key = json.load(f)
    if isinstance(key, dict) and key.get("_mode") not in (None, "detection"):
        # A blind-A/B run made with --mode preference stamps "_mode":
        # "preference" and the key then means the MODEL's side, not the
        # human's answer — reading it as a detection key would silently
        # invert which items count as "weak moments".
        return []
    with open(os.path.join(run_dir, "triples.json")) as f:
        triples = {t["id"]: t for t in json.load(f)}

    def caught(r):
        iid, ch = r.get("id"), (r.get("choice") or "").strip().upper()
        return ch in ("A", "B") and iid in key and iid in triples and ch == key[iid]

    human, synth = [], []
    for path in sorted(glob.glob(os.path.join(run_dir, "*.csv"))):
        with open(path, newline="") as f:
            rows = list(csv.DictReader(f))
        if any(store.row_is_judged(r) for r in rows):
            continue  # never trust a judged sheet of unknown provenance
        for r in rows:
            if not caught(r):
                continue
            try:
                conf = int(float(r.get("confidence") or 0))
            except ValueError:
                conf = 0
            if conf >= 4:
                human.append(r["id"])
    if (judged_csv and judge_model and os.path.isfile(judged_csv)
            and judge.lane_sheet_matches(judged_csv, run_dir)):
        with open(judged_csv, newline="") as f:
            for r in csv.DictReader(f):
                if (r.get("judge_model") or "").strip() == judge_model and caught(r):
                    synth.append(r["id"])
    out, seen = [], set()
    for src, ids in (("human", human), ("synthetic:" + str(judge_model), synth)):
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
              wide_live=False, judged_csv=None):
    """`limit` applies separately to critiques and to reference replies, so a
    night makes at most 2 x limit model calls. `judged_csv` is the lane's own
    newest judged sheet (judge.latest_lane_judged), the only source of
    synthetic weak moments."""
    now = now or _utcnow
    c = {k: 0 for k in ("critiques", "references", "unparseable", "errors", "attempted",
                        "stopped_at_deadline")}

    def out_of_time():
        if deadline is not None and now() >= deadline:
            c["stopped_at_deadline"] = 1
            return True
        return False

    c["critiques_skipped_no_run_dir"] = 0 if run_dirs else 1
    c["critiques_skipped_no_triples"] = 0
    done = store.critiqued_items(store_con, backend.name)
    todo = []
    for d in run_dirs:
        # The reference-reply half below needs no run dir, so neither skip
        # stops it.
        if not os.path.isfile(os.path.join(d, "answer_key.json")):
            c["critiques_skipped_no_run_dir"] += 1   # not a usable blind-A/B run dir
            continue
        if not os.path.isfile(os.path.join(d, "triples.json")):
            c["critiques_skipped_no_triples"] += 1   # operator hasn't copied triples in
            continue
        # Item ids like "c5-001" recur across re-exported run dirs, so the
        # stored id is scoped by the run dir's name.
        prefix = os.path.basename(os.path.normpath(d)) + "/"
        for iid, src, t in weak_items(d, judged_csv, getattr(backend, "model", None)):
            if prefix + iid not in done:
                todo.append((prefix + iid, src, t))
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


def gold_report(store_con, since_ms=0, backend=None):
    """Weekly gold summary. With `backend`, only that backend's rows; the
    report always names the backend(s) it covers."""
    where, args = " WHERE created_at_ms >= ?", [since_ms]
    if backend is not None:
        where += " AND backend = ?"
        args.append(backend)
    gaps = collections.Counter()
    crit = unp = 0
    for g, bad in store_con.execute("SELECT gaps, unparseable FROM critiques" + where, args):
        crit += 1
        unp += bad
        gaps.update(json.loads(g))
    refs = store_con.execute("SELECT COUNT(*) FROM reference_replies" + where, args).fetchone()[0]
    backends = sorted({r[0] for tbl in ("critiques", "reference_replies")
                       for r in store_con.execute(f"SELECT DISTINCT backend FROM {tbl}" + where,
                                                  args)})
    return {"critiques": crit, "references": refs, "unparseable": unp, "gaps": dict(gaps),
            "backends": backends, "backend_filter": backend}
