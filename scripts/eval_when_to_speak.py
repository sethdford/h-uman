#!/usr/bin/env python3
"""Contract C5, Part B — When2Speak MIR/FIR measurement.

Measures how well h-uman's proactive "should I speak now?" policy tracks
Seth's own behavior, using two rates borrowed from the When2Speak /
turn-taking literature:

  MIR (Missed-Intervention Rate) — of the moments chat.db shows Seth
    himself judged worth engaging with (he replied to an inbound DM within
    REPLY_WINDOW_HOURS), what fraction did the daemon's proactive policy
    DECLINE or never propose reaching out for, in the surrounding window?
    High MIR = the policy is too passive relative to Seth's own instincts.

  FIR (False-Interruption Rate) — of the daemon's actual SEND decisions,
    what fraction were (a) genuinely proactive (Seth had not already
    replied to that contact shortly before) and (b) landed with no
    engagement (the contact did not reply within FIR_WINDOW_HOURS)?
    High FIR = the policy interrupts people who don't want to hear from it
    right now.

Everything runs locally against two SQLite files already on this machine:

  ~/Library/Messages/chat.db   (read-only; NEVER copied or written to —
                                 this is the ground-truth "what actually
                                 happened between Seth and his contacts")
  ~/.human/memory.db            (the daemon's own decision/outcome log)

Decision source, in priority order:
  1. `proactive_decisions` (contract C5 Part A) once it has rows for the
     window under measurement.
  2. Fallback: synthesize "send" decisions from `production_outcomes` +
     `proactive_sends` (both pre-date Part A and only ever recorded
     SENDS, never declines — see FALLBACK NOTE below).

Nothing computed here leaves the machine — this script only reads local
sqlite files and writes one JSON summary to ~/.human/logs/. No message
content is included in the output; only counts, timestamps, and a
truncated one-way hash of each contact identifier.

Refuses (exit 2, writes nothing) when either measurement's denominator is
below --min-n (default 30), per
~/.claude/rules/no-number-without-a-measurement.md — a rate computed from
too few examples is not a rate, it's noise wearing a rate's clothes.

US-4 (sprint-better-than-human-2026-09-05): a single confirmed proactive
send is recorded as TWO rows in `proactive_decisions` -- a proposal row
(trigger='init_proposer_llm') and an outcome row (trigger='proactive_send')
-- sharing the same (contact, ts) because both call sites are handed the
same daemon-tick `now` local. resolve_decision_events() joins/dedupes each
group into one logical event before FIR ever sees it, so a delivered send
is not double-counted and a FIRED-but-never-sent proposal is not wrongly
admitted as a guaranteed false interruption. See resolve_decision_events()'s
docstring and designs/US-4.md Finding 2 / Approach §1 for the full
rationale. Both mir and fir carry a 95% Wilson score interval
(wilson_ci, via scripts/blind_ab/score.py's wilson() -- imported, not
reimplemented) alongside n/rate. --compare-baseline optionally states the
FIR-vs-Seth-initiation-baseline comparison (US-3) explicitly in the output.
"""
import argparse
import hashlib
import json
import os
import sqlite3
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "blind_ab"))
from score import wilson  # noqa: E402 -- reused, not reimplemented, per
                           # .claude/rules/no-number-without-a-measurement.md's sibling
                           # discipline of not hand-rolling a CI formula that already exists.

APPLE_EPOCH = 978307200  # 2001-01-01 00:00:00 UTC, chat.db's `date` epoch

FIR_WINDOW_HOURS = 24.0  # contact-replied-within window for FIR; also the comparison
                          # window for scripts/eval_seth_initiation_baseline.py's rate.

# US-4 (designs/US-4.md, "Risk analysis"): fixed, non-computed caveats disclosed
# in every successful evidence JSON so a reader doesn't have to re-derive them
# from the design doc.
KNOWN_LIMITATIONS = [
    "the (contact, ts) join between a FIRED proposal row "
    "(trigger='init_proposer_llm') and its outcome row "
    "(trigger='proactive_send') is a practical key -- both call sites share "
    "the same daemon-tick `now` local (src/daemon.c:1531-1671), not a "
    "schema-enforced foreign key -- so two distinct ticks for the same "
    "contact landing on the same integer unix-second ts would collide "
    "(low-probability, undetected by this script).",
    "an older proactive-send path (src/daemon.c's hu_init_proposer_tick_with_provider, "
    "the non-'_ex' variant) sends directly via the channel vtable and never "
    "writes to proactive_decisions; any sends through that path are invisible "
    "to this measurement -- a coverage gap in what MIR/FIR can see, not a "
    "join/dedup problem.",
]


def apple_ns_to_unix(apple_ns):
    if not apple_ns:
        return 0
    return APPLE_EPOCH + (apple_ns / 1_000_000_000.0)


def contact_ref(contact):
    """One-way, truncated reference for a contact identifier — enough to
    group rows by contact in the output JSON without writing a raw phone
    number/email into a file. Not used for anything security-sensitive."""
    if not contact:
        return None
    return hashlib.sha256(contact.encode("utf-8", "replace")).hexdigest()[:12]


def open_ro(path):
    if not os.path.exists(path):
        return None
    return sqlite3.connect(f"file:{path}?mode=ro", uri=True)


# ── chat.db extraction ──────────────────────────────────────────────────


def load_dm_messages(chat_db, since_unix):
    """Returns list of (chat_id, contact, ts_unix, is_from_me) for every
    message in a 1:1 (DM) chat since `since_unix`. Group chats (chats with
    more than one non-Seth participant) are excluded — When2Speak concerns
    one-on-one proactive outreach, and a group's "reply" isn't attributable
    to a single relationship the way a DM's is."""
    cur = chat_db.cursor()

    # DM chats: exactly one handle joined to the chat.
    cur.execute(
        """
        SELECT chj.chat_id, MIN(h.id) AS contact
        FROM chat_handle_join chj
        JOIN handle h ON h.ROWID = chj.handle_id
        GROUP BY chj.chat_id
        HAVING COUNT(DISTINCT chj.handle_id) = 1
        """
    )
    dm_chat_contact = {row[0]: row[1] for row in cur.fetchall()}
    if not dm_chat_contact:
        return []

    since_apple_ns = (since_unix - APPLE_EPOCH) * 1_000_000_000
    cur.execute(
        """
        SELECT cmj.chat_id, m.date, m.is_from_me
        FROM message m
        JOIN chat_message_join cmj ON cmj.message_id = m.ROWID
        WHERE m.date >= ?
          AND m.is_system_message = 0
          AND m.item_type = 0
        ORDER BY m.date ASC
        """,
        (since_apple_ns,),
    )
    out = []
    for chat_id, date_ns, is_from_me in cur.fetchall():
        contact = dm_chat_contact.get(chat_id)
        if not contact:
            continue  # not a DM chat
        ts = apple_ns_to_unix(date_ns)
        if ts <= 0:
            continue
        out.append((chat_id, contact, ts, bool(is_from_me)))
    return out


def label_positives(messages, reply_window_secs):
    """For every inbound (is_from_me=False) message, label positive=True
    iff there is a later outbound message in the SAME chat within
    reply_window_secs. Returns list of dicts:
      {chat_id, contact, ts, positive}
    """
    by_chat = {}
    for chat_id, contact, ts, is_from_me in messages:
        by_chat.setdefault(chat_id, []).append((ts, is_from_me, contact))

    positives = []
    for chat_id, rows in by_chat.items():
        rows.sort(key=lambda r: r[0])
        outbound_ts = [ts for ts, ifm, _ in rows if ifm]
        for i, (ts, is_from_me, contact) in enumerate(rows):
            if is_from_me:
                continue
            # First outbound strictly after this inbound message, within window.
            replied = any(ts < o_ts <= ts + reply_window_secs for o_ts in outbound_ts)
            positives.append(
                {"chat_id": chat_id, "contact": contact, "ts": ts, "positive": replied}
            )
    return positives


def seth_initiated_sends(messages):
    """Outbound (Seth) messages per chat, sorted — used for the FIR
    "Seth had not already replied" exclusion."""
    by_chat = {}
    for chat_id, contact, ts, is_from_me in messages:
        if is_from_me:
            by_chat.setdefault(chat_id, []).append(ts)
    for v in by_chat.values():
        v.sort()
    return by_chat


def contact_to_chat(messages):
    """contact -> chat_id, for joining decision rows (keyed by contact
    identifier, e.g. a phone number) back to chat.db chat ids."""
    m = {}
    for chat_id, contact, _ts, _ifm in messages:
        m.setdefault(contact, chat_id)
    return m


# ── decision-log extraction (~/.human/memory.db) ────────────────────────


def load_decisions(memory_db, since_unix):
    """Returns (rows, source) where rows is a list of
      {ts, contact, decision, sent, trigger}
    and source is 'proactive_decisions' or 'fallback'.

    `trigger` is the de-facto join key's second half (see
    resolve_decision_events() below) for the 'proactive_decisions' source:
    'init_proposer_llm' for a FIRED proposal row, 'proactive_send' for its
    later outcome row. Fallback-sourced rows (which predate the trigger
    column entirely) are tagged 'fallback' -- a value that never matches
    either real trigger, so resolve_decision_events() passes them through
    unresolved/undeduped, exactly as before this story's join fix."""
    cur = memory_db.cursor()
    cur.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='proactive_decisions'"
    )
    if cur.fetchone():
        cur.execute(
            "SELECT ts, contact, decision, sent, trigger FROM proactive_decisions "
            "WHERE ts >= ? AND contact IS NOT NULL",
            (since_unix,),
        )
        rows = [
            {"ts": ts, "contact": contact, "decision": decision, "sent": bool(sent), "trigger": trigger}
            for ts, contact, decision, sent, trigger in cur.fetchall()
        ]
        if rows:
            return rows, "proactive_decisions"
        # Table exists but is empty for this window — fall through to the
        # fallback so a freshly-deployed C5 Part A doesn't silently
        # produce a "no decisions" (n=0) result before it's had time to
        # accumulate rows.

    # FALLBACK NOTE: production_outcomes and proactive_sends predate
    # contract C5 and only ever recorded rows for ACTUAL sends — there is
    # no decline/defer history in them. Every row here therefore has
    # decision='send'. This makes the fallback's MIR structurally biased
    # toward "missed" for any positive moment with no nearby send (which
    # is expected — it is exactly what MIR is supposed to catch), but it
    # cannot distinguish "the policy considered and declined" from "the
    # policy never ran a tick for this contact at all". Once
    # proactive_decisions has real rows this distinction becomes visible.
    rows = []
    cur.execute("SELECT name FROM sqlite_master WHERE type='table' AND name='proactive_sends'")
    if cur.fetchone():
        cur.execute(
            "SELECT sent_timestamp, contact FROM proactive_sends WHERE sent_timestamp >= ?",
            (since_unix,),
        )
        rows += [
            {"ts": ts, "contact": contact, "decision": "send", "sent": True, "trigger": "fallback"}
            for ts, contact in cur.fetchall()
        ]
    cur.execute(
        "SELECT name FROM sqlite_master WHERE type='table' AND name='production_outcomes'"
    )
    if cur.fetchone():
        cur.execute(
            "SELECT send_timestamp, target FROM production_outcomes WHERE send_timestamp >= ?",
            (since_unix,),
        )
        rows += [
            {"ts": ts, "contact": target, "decision": "send", "sent": True, "trigger": "fallback"}
            for ts, target in cur.fetchall()
        ]
    return rows, "fallback"


# ── join/dedup: resolve raw decision-log rows into one event per real send ──


def resolve_decision_events(rows):
    """Groups decision-log rows (each carrying ts, contact, decision, sent,
    trigger) by the de-facto join key (contact, ts) -- see designs/US-4.md
    Finding 2: a single daemon tick passes the SAME `now` local to both the
    proposal call site (src/agent/init_proposer.c:1278,
    trigger='init_proposer_llm') and the outcome call site
    (src/daemon/daemon_proactive.c:923, trigger='proactive_send'), so a real
    send's proposal row and outcome row share (contact, ts) exactly. Without
    this resolution step, compute_fir() would double-count every confirmed
    send (one eligible-send hit from each of the two rows) and would
    wrongly admit a FIRED-but-never-delivered send as an eligible,
    guaranteed-false interruption (only the proposal row passes a naive
    `decision == 'send'` filter).

    Returns a list of resolved-event dicts:
      {"contact", "ts", "resolved_decision", "resolved_sent",
       "dropped_pre_send": bool, "send_failed": bool}

    Resolution per (contact, ts) group:
      - An outcome row (trigger == 'proactive_send') is authoritative when
        present: resolved_decision / resolved_sent come from it, never from
        the proposal row.
        * send_failed = True iff a proposal row in the SAME group had
          decision == 'send' but the authoritative outcome's decision is
          NOT 'send' (the proposal FIRED but died before delivery --
          e.g. a channel-send error at daemon_proactive.c:970).
      - No outcome row present, but a proposal row (trigger ==
        'init_proposer_llm') is: resolved_sent = False; resolved_decision =
        the proposal's own decision.
        * dropped_pre_send = True iff that resolved_decision == 'send' --
          FIRED but the daemon never even attempted to send (skipped by a
          validator/gate/dedup/rate-limit check between daemon.c:1573 and
          :1666).
      - Neither trigger matches (fallback-sourced rows, tagged
        trigger='fallback' by load_decisions(), or any future trigger this
        script doesn't yet know about): there is no join ambiguity to
        resolve -- each such row already represents one complete,
        independent event (this is exactly the fallback source's shape:
        every row is an already-confirmed send). Each is passed through as
        its own resolved event with dropped_pre_send = send_failed = False,
        preserving pre-fix behavior for the fallback source untouched.
    """
    groups = {}
    for r in rows:
        groups.setdefault((r["contact"], r["ts"]), []).append(r)

    events = []
    for (contact, ts), group_rows in groups.items():
        outcome_rows = [r for r in group_rows if r.get("trigger") == "proactive_send"]
        proposal_rows = [r for r in group_rows if r.get("trigger") == "init_proposer_llm"]
        other_rows = [
            r for r in group_rows if r.get("trigger") not in ("proactive_send", "init_proposer_llm")
        ]

        if outcome_rows:
            outcome = outcome_rows[-1]
            resolved_decision = outcome["decision"]
            resolved_sent = bool(outcome["sent"])
            proposal_fired = any(r["decision"] == "send" for r in proposal_rows)
            events.append({
                "contact": contact,
                "ts": ts,
                "resolved_decision": resolved_decision,
                "resolved_sent": resolved_sent,
                "dropped_pre_send": False,
                "send_failed": proposal_fired and resolved_decision != "send",
            })
        elif proposal_rows:
            resolved_decision = proposal_rows[-1]["decision"]
            events.append({
                "contact": contact,
                "ts": ts,
                "resolved_decision": resolved_decision,
                "resolved_sent": False,
                "dropped_pre_send": resolved_decision == "send",
                "send_failed": False,
            })
        else:
            for r in other_rows:
                events.append({
                    "contact": contact,
                    "ts": ts,
                    "resolved_decision": r["decision"],
                    "resolved_sent": bool(r["sent"]),
                    "dropped_pre_send": False,
                    "send_failed": False,
                })
    return events


# ── MIR / FIR computation ───────────────────────────────────────────────


def compute_mir(positives, decisions_by_contact, before_secs, after_secs):
    """Returns {"n", "missed", "rate", "wilson_ci"} -- wilson_ci is the 95%
    Wilson score interval on the missed/total proportion, via the shared
    scripts/blind_ab/score.py:wilson() (imported, not reimplemented -- same
    discipline as scripts/eval_seth_initiation_baseline.py's
    compute_baseline()). [0.0, 0.0] when n==0, matching that sibling's
    degenerate-input convention so this stays directly unit-testable."""
    total = 0
    missed = 0
    for p in positives:
        if not p["positive"]:
            continue
        total += 1
        window = decisions_by_contact.get(p["contact"], [])
        nearby = [d for d in window if p["ts"] - before_secs <= d["ts"] <= p["ts"] + after_secs]
        has_send = any(d["decision"] == "send" for d in nearby)
        if not has_send:
            missed += 1
    if total == 0:
        return {"n": 0, "missed": 0, "rate": None, "wilson_ci": [0.0, 0.0]}
    _p, lo, hi = wilson(missed, total)
    return {"n": total, "missed": missed, "rate": missed / total, "wilson_ci": [lo, hi]}


def compute_fir(events, seth_sends_by_chat, contact_chat_map, contact_replies_by_contact,
                seth_before_secs, contact_after_secs):
    """`events` is the resolve_decision_events() output — one logical event
    per (contact, ts), not raw (possibly duplicated) decision-log rows. The
    FIR-eligible population is confirmed delivery only:
    resolved_decision == 'send' AND resolved_sent is True, matching the
    metric's own docstring ("actual SEND decisions ... landed"). Events that
    never reached the eligible population are still counted, separately, so
    the evidence JSON states where they went instead of silently discarding
    them (dropped_pre_send / send_failed are diagnostic counters, not part
    of the rate — see designs/US-4.md, Approach §1)."""
    eligible = 0
    false_interruptions = 0
    dropped_pre_send = 0
    send_failed = 0
    for e in events:
        if e["dropped_pre_send"]:
            dropped_pre_send += 1
            continue
        if e["send_failed"]:
            send_failed += 1
            continue
        if e["resolved_decision"] != "send" or not e["resolved_sent"]:
            continue
        chat_id = contact_chat_map.get(e["contact"])
        seth_ts_list = seth_sends_by_chat.get(chat_id, []) if chat_id is not None else []
        # Exclusion: Seth already sent something to this contact shortly
        # before the daemon's send — this wasn't a case of Seth-silence.
        seth_already_engaged = any(
            e["ts"] - seth_before_secs <= s_ts < e["ts"] for s_ts in seth_ts_list
        )
        if seth_already_engaged:
            continue
        eligible += 1
        reply_ts_list = contact_replies_by_contact.get(e["contact"], [])
        got_reply = any(e["ts"] < r_ts <= e["ts"] + contact_after_secs for r_ts in reply_ts_list)
        if not got_reply:
            false_interruptions += 1
    if eligible == 0:
        return {
            "n": 0,
            "false_interruptions": 0,
            "rate": None,
            "wilson_ci": [0.0, 0.0],
            "dropped_pre_send": dropped_pre_send,
            "send_failed": send_failed,
        }
    _p, lo, hi = wilson(false_interruptions, eligible)
    return {
        "n": eligible,
        "false_interruptions": false_interruptions,
        "rate": false_interruptions / eligible,
        "wilson_ci": [lo, hi],
        "dropped_pre_send": dropped_pre_send,
        "send_failed": send_failed,
    }


def contact_inbound_ts(messages):
    """contact -> sorted list of inbound (is_from_me=False) timestamps —
    used by compute_fir to check whether the contact replied after a
    daemon send."""
    m = {}
    for chat_id, contact, ts, is_from_me in messages:
        if not is_from_me:
            m.setdefault(contact, []).append(ts)
    for v in m.values():
        v.sort()
    return m


def decisions_by_contact_index(events):
    """events: resolve_decision_events() output (or, for callers still
    passing raw un-resolved decision-log rows, anything carrying
    "contact"/"ts" plus either "decision" or "resolved_decision" — accepted
    for robustness, not exercised by main() post-fix). Returns
    contact -> [{"ts": ..., "decision": ...}, ...], the shape compute_mir's
    presence check expects. Because `events` is already deduped to one
    entry per (contact, ts), this can no longer double-count a confirmed
    send the way indexing raw decision-log rows could — though compute_mir's
    own any() presence check was already insensitive to that duplication
    (a duplicate can't make any() more true)."""
    m = {}
    for e in events:
        decision = e["resolved_decision"] if "resolved_decision" in e else e["decision"]
        m.setdefault(e["contact"], []).append({"ts": e["ts"], "decision": decision})
    return m


# ── US-3 baseline comparison (AC-4.4) ───────────────────────────────────


def load_baseline_rate(path):
    """Returns (rate, error_reason). `rate` is None iff error_reason is
    not None. Accepts either the raw eval_seth_initiation_baseline.py
    output shape (top-level "rate" key) or the sprint evidence-JSON shape
    that wraps the run under "primary_run" (see
    sprints/*/evidence/us3-seth-initiation-baseline.json, this sprint's own
    committed US-3 artifact) — tries top-level first, falls back to
    primary_run.rate, so either file this story is handed works unchanged."""
    if not os.path.exists(path):
        return None, f"US-3 baseline path not found: {path}"
    try:
        with open(path) as f:
            data = json.load(f)
    except (OSError, ValueError) as exc:
        return None, f"US-3 baseline unreadable at {path}: {exc}"
    rate = data.get("rate")
    if rate is None:
        rate = (data.get("primary_run") or {}).get("rate")
    if rate is None:
        return None, f"US-3 baseline at {path} has no usable 'rate' field (US-3 may have refused)"
    return rate, None


def compare_to_baseline(fir_rate, baseline_path):
    """AC-4.4: state the FIR-vs-Seth-baseline comparison explicitly, in one
    of exactly three shapes — never leave it to a reader to diff two JSON
    files by hand:
      - flag omitted, or path missing/unreadable/refused: {"available":
        False, "reason": <specific, names the path>}
      - flag valid: {"available": True, "fir_rate", "seth_baseline_rate",
        "verdict": "fir_le_baseline"|"fir_gt_baseline"}"""
    if not baseline_path:
        return {"available": False, "reason": "US-3 baseline not provided or US-3 refused"}
    rate, err = load_baseline_rate(baseline_path)
    if err is not None:
        return {"available": False, "reason": err}
    verdict = "fir_le_baseline" if fir_rate <= rate else "fir_gt_baseline"
    return {
        "available": True,
        "fir_rate": fir_rate,
        "seth_baseline_rate": rate,
        "verdict": verdict,
    }


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--days", type=int, default=90, help="lookback window (default 90)")
    ap.add_argument("--reply-window-hours", type=float, default=6.0,
                    help="Seth-replied-within window for the 'positive' label (default 6h)")
    ap.add_argument("--fir-window-hours", type=float, default=FIR_WINDOW_HOURS,
                    help="contact-replied-within window for FIR (default 24h)")
    ap.add_argument("--decision-window-before-hours", type=float, default=1.0,
                    help="how far BEFORE a positive moment to look for a decision (default 1h)")
    ap.add_argument("--decision-window-after-hours", type=float, default=24.0,
                    help="how far AFTER a positive moment to look for a decision (default 24h)")
    ap.add_argument("--seth-already-engaged-hours", type=float, default=6.0,
                    help="FIR exclusion window: Seth sent something in this window before "
                         "the daemon's send (default 6h)")
    ap.add_argument("--min-n", type=int, default=30, help="refuse below this denominator (default 30)")
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--memory-db", default=os.path.expanduser("~/.human/memory.db"))
    ap.add_argument("--out-dir", default=os.path.expanduser("~/.human/logs"))
    ap.add_argument("--compare-baseline", default=None,
                    help="path to a US-3 seth-initiation-baseline JSON (raw script output "
                         "or the sprint evidence-JSON wrapper) to compare FIR against (AC-4.4)")
    args = ap.parse_args(argv)

    now = time.time()
    since = now - args.days * 86400.0

    chat_db = open_ro(args.chat_db)
    if chat_db is None:
        print(f"REFUSE: chat.db not found at {args.chat_db}", file=sys.stderr)
        return 2
    memory_db = open_ro(args.memory_db)
    if memory_db is None:
        print(f"REFUSE: memory.db not found at {args.memory_db}", file=sys.stderr)
        return 2

    messages = load_dm_messages(chat_db, since)
    if not messages:
        print("REFUSE: no DM messages found in the lookback window", file=sys.stderr)
        return 2

    positives = label_positives(messages, args.reply_window_hours * 3600.0)
    seth_sends_by_chat = seth_initiated_sends(messages)
    contact_chat_map = contact_to_chat(messages)
    contact_replies = contact_inbound_ts(messages)

    decisions, decisions_source = load_decisions(memory_db, since)
    events = resolve_decision_events(decisions)
    decisions_by_contact = decisions_by_contact_index(events)

    mir = compute_mir(
        positives,
        decisions_by_contact,
        before_secs=args.decision_window_before_hours * 3600.0,
        after_secs=args.decision_window_after_hours * 3600.0,
    )
    fir = compute_fir(
        events,
        seth_sends_by_chat,
        contact_chat_map,
        contact_replies,
        seth_before_secs=args.seth_already_engaged_hours * 3600.0,
        contact_after_secs=args.fir_window_hours * 3600.0,
    )

    if mir["n"] < args.min_n or fir["n"] < args.min_n:
        print(
            f"REFUSE: insufficient n (MIR n={mir['n']}, FIR n={fir['n']}, min_n={args.min_n}) "
            "— not writing a result.",
            file=sys.stderr,
        )
        return 2

    total_positive_examples = sum(1 for p in positives if p["positive"])
    comparison = compare_to_baseline(fir["rate"], args.compare_baseline)
    result = {
        "generated_at": int(now),
        "days": args.days,
        "decisions_source": decisions_source,
        "windows": {
            "reply_window_hours": args.reply_window_hours,
            "fir_window_hours": args.fir_window_hours,
            "decision_window_before_hours": args.decision_window_before_hours,
            "decision_window_after_hours": args.decision_window_after_hours,
            "seth_already_engaged_hours": args.seth_already_engaged_hours,
        },
        "inputs": {
            "dm_messages": len(messages),
            "inbound_messages": len(positives),
            "positive_moments": total_positive_examples,
            "decisions_loaded": len(decisions),
            "resolved_events": len(events),
        },
        "mir": mir,
        "fir": fir,
        "comparison": comparison,
        "known_limitations": KNOWN_LIMITATIONS,
    }

    os.makedirs(args.out_dir, exist_ok=True)
    date_str = time.strftime("%Y-%m-%d", time.localtime(now))
    out_path = os.path.join(args.out_dir, f"when-to-speak-{date_str}.json")
    with open(out_path, "w") as f:
        json.dump(result, f, indent=2)

    print(f"wrote {out_path}")
    print(
        f"MIR: {mir['rate']:.3f} (missed {mir['missed']}/{mir['n']}, "
        f"95% CI [{mir['wilson_ci'][0]:.3f}, {mir['wilson_ci'][1]:.3f}])"
        if mir["rate"] is not None
        else "MIR: n/a"
    )
    print(
        f"FIR: {fir['rate']:.3f} (false_interruptions {fir['false_interruptions']}/{fir['n']}, "
        f"95% CI [{fir['wilson_ci'][0]:.3f}, {fir['wilson_ci'][1]:.3f}], "
        f"dropped_pre_send={fir['dropped_pre_send']}, send_failed={fir['send_failed']})"
        if fir["rate"] is not None
        else "FIR: n/a"
    )
    print(f"decisions_source={decisions_source}")
    if comparison["available"]:
        print(
            f"comparison: FIR {comparison['fir_rate']:.3f} vs Seth baseline "
            f"{comparison['seth_baseline_rate']:.3f} -> {comparison['verdict']}"
        )
    else:
        print(f"comparison: unavailable ({comparison['reason']})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
