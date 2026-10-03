"""Verifier audit (spec §4.1): a different model family re-checks kept curator
notes against ONLY the messages they cite. Its verdict is a second opinion,
not truth; audit_sheet/audit_score turn it into a real error rate."""
import datetime as dt
import json
import os
import re
import sqlite3
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                "blind_ab"))
from imessage_text import msg_text  # noqa: E402

from . import stats, store  # noqa: E402

PROMPT_VERSION = "audit-v1"
VERDICTS = ("supported", "unsupported", "unclear")
SOURCES = ("persona", "wide", "all")
SYSTEM = ("You check whether a short memory note about a person is supported by the text "
          "messages it cites. On the first line answer with exactly one word: supported, "
          "unsupported, or unclear. supported = the messages state it. unsupported = the "
          "messages do not state it, or contradict it. unclear = the messages are ambiguous. "
          "On the second line give one short reason.")


def source_kind(source):
    return "wide" if (source or "").startswith("curator_wide") else "persona"


def resolve_evidence(evidence_ids, mem, chat):
    """Cited message texts, or [] if ANY citation can't be resolved: a note is
    never judged on partial evidence."""
    try:
        ids = json.loads(evidence_ids) if evidence_ids else []
    except ValueError:
        return []
    if not isinstance(ids, list) or not ids:
        return []
    texts = []
    try:
        for e in ids:
            s = str(e)
            if s.startswith("chat:"):
                if chat is None:
                    return []
                row = chat.execute("SELECT text, attributedBody FROM message WHERE ROWID = ?",
                                   (int(s[5:]),)).fetchone()
                t = msg_text(row[0], row[1]) if row else None
            else:
                row = mem.execute("SELECT content FROM messages WHERE id = ?",
                                  (int(s),)).fetchone()
                t = row[0] if row else None
            if not t or not t.strip():
                return []
            texts.append(t.strip().replace("\n", " ")[:300])
    except (ValueError, TypeError, sqlite3.Error):
        # A bad read counts as unresolvable evidence: one note must never
        # abort the whole pass.
        return []
    return texts


def parse_verdict(text):
    lines = [ln.strip() for ln in (text or "").strip().splitlines() if ln.strip()]
    if not lines:
        return "unclear", "", True
    first = re.sub(r"[^a-z]", "", lines[0].lower())
    if first in VERDICTS:
        return first, (lines[1] if len(lines) > 1 else "")[:200], False
    return "unclear", "", True


def _utcnow():
    return dt.datetime.now(dt.timezone.utc)


def audit_pass(store_con, backend, mem, chat, limit, deadline=None, now=None):
    now = now or _utcnow
    c = {k: 0 for k in ("sampled", "attempted", "audited", "supported", "unsupported", "unclear",
                        "unparseable", "skipped_no_evidence", "errors", "stopped_at_deadline")}
    done = store.audited_ids(store_con, backend.name)
    rows = mem.execute("SELECT id, insight, source, evidence_ids FROM contact_insights"
                       " WHERE retired_at_ms = 0 ORDER BY created_at_ms DESC, id DESC").fetchall()
    for iid, note, source, ev in rows:
        if c["sampled"] >= limit:
            break
        if iid in done:
            continue
        if deadline is not None and now() >= deadline:
            c["stopped_at_deadline"] = 1
            break
        c["sampled"] += 1
        texts = resolve_evidence(ev, mem, chat)
        if not texts:
            # Recorded once, so an unresolvable note never re-consumes the nightly
            # limit; 'no_evidence' is outside every rate (audit_report reads only
            # supported/unsupported/unclear).
            store.add_audit(store_con, iid, source_kind(source), "no_evidence", "", False,
                            backend.name, PROMPT_VERSION, store.now_ms())
            c["skipped_no_evidence"] += 1
            continue
        c["attempted"] += 1
        user = "Note: " + note + "\n\nCited messages:\n" + "\n".join(f"- {t}" for t in texts)
        try:
            raw = backend.generate(SYSTEM, user, max_tokens=120)
        except Exception:
            c["errors"] += 1
            continue
        verdict, reason, bad = parse_verdict(raw)
        store.add_audit(store_con, iid, source_kind(source), verdict, reason, bad, backend.name,
                        PROMPT_VERSION, store.now_ms())
        c["audited"] += 1
        c[verdict] += 1
        c["unparseable"] += int(bad)
    return c


def audit_filter(backend=None, source="all", since_ms=0):
    """(WHERE clause, args) selecting audits by provenance. backend=None means
    every backend; source is "wide", "persona" or "all"."""
    if source not in SOURCES:
        raise ValueError(f"unknown source: {source!r}")
    where, args = " WHERE created_at_ms >= ?", [since_ms]
    if backend is not None:
        where += " AND backend = ?"
        args.append(backend)
    if source != "all":
        where += " AND source = ?"
        args.append(source)
    return where, args


def audit_report(store_con, since_ms=0, backend=None):
    """Verdict counts and disagreement rate per source. With `backend`, only
    that backend's verdicts (the runner passes its own); the report always
    names the backend(s) and prompt version(s) it covers, so a Gemini night
    can never silently mix into Gemma's number."""
    out = {}
    for src in SOURCES:
        where, args = audit_filter(backend, src, since_ms)
        n = dict(store_con.execute("SELECT verdict, COUNT(*) FROM audits" + where
                                   + " GROUP BY verdict", args).fetchall())
        sup, uns, unc = n.get("supported", 0), n.get("unsupported", 0), n.get("unclear", 0)
        out[src] = {"supported": sup, "unsupported": uns, "unclear": unc,
                    "disagreement": stats.rate(uns, sup + uns)}
    where, args = audit_filter(backend, "all", since_ms)
    out["backends"] = sorted(r[0] for r in store_con.execute(
        "SELECT DISTINCT backend FROM audits" + where, args))
    out["prompt_versions"] = sorted(r[0] for r in store_con.execute(
        "SELECT DISTINCT prompt_version FROM audits" + where, args))
    out["backend_filter"] = backend  # names the filter even when it matched no rows
    return out
