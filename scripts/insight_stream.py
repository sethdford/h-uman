#!/usr/bin/env python3
"""Insight stream extractor — item 3 of docs/plans/2026-09-06-better-than-human.

For each persona contact with enough stored turns, ask the LOCAL model (GLM on
:8741, never a cloud provider — this reads real conversations) to write the
short private notes Seth would actually keep about that person: specific
names, places, dated plans, what they're dealing with, running jokes and
inside references, preferences. Persona-conditioned (the persona's identity
line is the system frame), so the notes are what SETH would remember, not a
neutral summary. Writes to the `contact_insights` table the daemon's memory
loader renders behind HU_INSIGHT_STREAM (off | shadow | live).

Schema mirrors src/memory/repos/contact_insights_repo_sqlite.c exactly
(CREATE ... IF NOT EXISTS on both sides; the UNIQUE(contact_id, insight)
index makes re-extraction a no-op rather than a duplicate).

Default is a DRY RUN that prints the notes. --write inserts them.

Usage:
  scripts/insight_stream.py [--contact +1555...] [--turns 80] [--write]
                            [--min-turns 20] [--max-notes 8] [--url URL] [--model M]
"""
import argparse
import json
import os
import random
import re
import sqlite3
import sys
import time
import urllib.request

HOME = os.path.expanduser("~")
MEMORY_DB = os.path.join(HOME, ".human/memory.db")
PERSONA = os.path.join(HOME, ".human/personas/seth.json")
DEFAULT_URL = "http://127.0.0.1:8741/v1/chat/completions"
DEFAULT_MODEL = "GLM-4.5-Air-4bit"
SOURCE = "extractor:v1"

SCHEMA = """
CREATE TABLE IF NOT EXISTS contact_insights (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  contact_id TEXT NOT NULL,
  kind TEXT NOT NULL DEFAULT 'fact',
  insight TEXT NOT NULL,
  confidence REAL NOT NULL DEFAULT 0.7,
  as_of_ms INTEGER NOT NULL DEFAULT 0,
  source TEXT,
  created_at_ms INTEGER NOT NULL,
  retired_at_ms INTEGER NOT NULL DEFAULT 0,
  evidence_ids TEXT,
  superseded_by_id INTEGER NOT NULL DEFAULT 0
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_contact_insights_natural
  ON contact_insights(contact_id, insight);
CREATE INDEX IF NOT EXISTS idx_contact_insights_contact
  ON contact_insights(contact_id, retired_at_ms, as_of_ms DESC);
"""

# Columns added after the table first shipped (PGMem-style provenance, 2026-09-19):
# evidence_ids = JSON list of messages.id rows the note was read from;
# superseded_by_id = the newer insight that retired this one (0 = not superseded).
# CREATE TABLE IF NOT EXISTS never adds columns to a live table, so migrate.
MIGRATIONS = {
    "evidence_ids": "ALTER TABLE contact_insights ADD COLUMN evidence_ids TEXT",
    "superseded_by_id": "ALTER TABLE contact_insights ADD COLUMN superseded_by_id "
                        "INTEGER NOT NULL DEFAULT 0",
}


def migrate(db):
    have = {r[1] for r in db.execute("PRAGMA table_info(contact_insights)")}
    for col, ddl in MIGRATIONS.items():
        if col not in have:
            db.execute(ddl)
    db.commit()


KINDS = {"fact", "thread", "plan", "preference", "inside_ref"}
SKIP_RELATIONSHIPS = {"test"}


def load_persona():
    p = json.load(open(PERSONA))
    identity = (p.get("core") or {}).get("identity") or "Seth Ford."
    contacts = {}
    for cid, c in (p.get("contacts") or {}).items():
        rel = (c.get("relationship") or "").lower()
        name = c.get("name") or cid
        if rel in SKIP_RELATIONSHIPS or name.lower().startswith("unknown"):
            continue
        contacts[cid] = {"name": name, "relationship": rel or c.get("relationship_type") or ""}
    return identity, contacts


def _created_at_ms(text):
    """messages.created_at is TEXT datetime('now') (UTC); never compare it to
    epoch integers in SQL — that is silently vacuous. Parse it here."""
    try:
        return int(time.mktime(time.strptime(text, "%Y-%m-%d %H:%M:%S")) * 1000
                   - time.timezone * 1000)
    except Exception:
        return 0


def recent_turn_rows(db, contact_id, n):
    """Oldest-first [(message_id, created_at_ms, 'me'|'them', text)] with the
    ids kept, so a note can cite the rows it was read from."""
    rows = db.execute(
        "SELECT id, role, content, created_at FROM messages WHERE session_id = ? "
        "ORDER BY id DESC LIMIT ?", (contact_id, n)).fetchall()
    rows.reverse()
    out = []
    for mid, role, content, created in rows:
        content = (content or "").strip().replace("\n", " ")
        if not content:
            continue
        who = "me" if role == "assistant" else "them"
        out.append((int(mid), _created_at_ms(created or ""), who, content[:300]))
    return out


def recent_turns(db, contact_id, n):
    return [f"{who}: {text}" for _, _, who, text in recent_turn_rows(db, contact_id, n)]


def numbered_turns(rows):
    """Prompt lines '[t3] them: ...' so the model can cite evidence by index."""
    return [f"[t{i}] {who}: {text}" for i, (_, _, who, text) in enumerate(rows)]


def evidence_for(indices, rows):
    """Turn indices cited by the model -> (message ids, newest evidence ms)."""
    ids, newest = [], 0
    for i in indices:
        if 0 <= i < len(rows):
            mid, ms, _, _ = rows[i]
            ids.append(mid)
            newest = max(newest, ms)
    return sorted(set(ids)), newest


def build_prompt(identity, name, relationship, turns, max_notes):
    system = (
        f"You are Seth Ford. {identity}\n\n"
        f"You just reread your recent texts with {name}"
        f"{' (' + relationship + ')' if relationship else ''} and are jotting private notes to "
        "yourself — the things YOU would actually remember and bring up next time: specific names, "
        "places, plans with when, what they're dealing with, running jokes and inside references, "
        "what they like and don't. Never generic traits (\"is friendly\"), never advice, never "
        "anything not in the texts. Lowercase, like a note to yourself, present tense, each under "
        "110 characters.\n\n"
        f"Output ONLY a JSON array of at most {max_notes} objects: "
        "{\"note\": str, \"kind\": \"fact\"|\"thread\"|\"plan\"|\"preference\"|\"inside_ref\", "
        "\"confidence\": number 0-1, \"evidence\": [the [tN] numbers of the texts the note "
        "comes from]}. No prose before or after."
    )
    user = "recent texts (oldest first):\n" + "\n".join(turns)
    return system, user


def call_model(url, model, system, user, timeout=300, temperature=0.3):
    req = {
        "model": model,
        "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
        "max_tokens": 700,
        "temperature": temperature,
        "chat_template_kwargs": {"enable_thinking": False},
    }
    r = urllib.request.urlopen(
        urllib.request.Request(url, data=json.dumps(req).encode(),
                               headers={"Content-Type": "application/json"}), timeout=timeout)
    d = json.load(r)
    return (d["choices"][0]["message"].get("content") or "").strip()


def parse_notes(text, max_notes):
    m = re.search(r"\[[\s\S]*\]", text)
    if not m:
        return []
    try:
        arr = json.loads(m.group(0))
    except Exception:
        return []
    notes, seen = [], set()
    for o in arr if isinstance(arr, list) else []:
        if not isinstance(o, dict):
            continue
        note = (o.get("note") or "").strip().rstrip(".")
        if not note or len(note) > 140 or note.lower() in seen:
            continue
        kind = (o.get("kind") or "fact").strip().lower()
        if kind not in KINDS:
            kind = "fact"
        try:
            conf = float(o.get("confidence", 0.7))
        except Exception:
            conf = 0.7
        conf = max(0.0, min(1.0, conf))
        ev = []
        for e in (o.get("evidence") or []) if isinstance(o.get("evidence"), list) else []:
            m2 = re.search(r"\d+", str(e))
            if m2:
                ev.append(int(m2.group(0)))
        seen.add(note.lower())
        notes.append({"note": note, "kind": kind, "confidence": conf, "evidence": ev})
        if len(notes) >= max_notes:
            break
    return notes


# ---- admission control (ConsistencyGate, arXiv 2607.22962; MemGuard 2608.21867) ----
#
# One sample at temperature 0.3 wrote whatever the model said. Regenerating K
# times and intersecting does NOT work here (measured 2026-09-19: an 80-turn
# context holds far more than max_notes facts, so three samples pick disjoint
# subsets and 0 of 8 notes reached a majority). ConsistencyGate's real shape is
# generate ONCE, then ask K independent verification questions per candidate:
# "do the texts actually support this?" Agreement scales confidence (agree/K)
# instead of hard-dropping, so the reader's HU_INSIGHT_MIN_CONFIDENCE (0.5) is
# the gate: a 1-of-3 note (x0.33) never renders, a 3-of-3 keeps its confidence.
# The agreement count is kept in `source` ("extractor:v2:k3:a2") as persistent
# verifier metadata.

VERIFY_TEMPERATURE = 0.7  # decorrelates the K verification passes

VERIFY_SYSTEM = (
    "You are Seth Ford. {identity}\n\n"
    "Below are your recent texts with {name}{rel}, then numbered candidate notes. For each "
    "note decide whether the texts ACTUALLY say it — not inferred, not generic, not from "
    "somewhere else. {question}\n\n"
    "Output ONLY a JSON array of objects {{\"i\": int, \"supported\": true|false}}, one per "
    "note. No prose."
)


def parse_verdicts(text, n):
    """-> list[bool] of length n; a missing index counts as unsupported."""
    out = [False] * n
    m = re.search(r"\[[\s\S]*\]", text)
    if not m:
        return out
    try:
        arr = json.loads(m.group(0))
    except Exception:
        return out
    for o in arr if isinstance(arr, list) else []:
        if not isinstance(o, dict):
            continue
        try:
            i = int(o.get("i"))
        except Exception:
            continue
        if 0 <= i < n:
            out[i] = o.get("supported") is True  # JSON true only; "yes"/1 do not count
    return out


def verify_claims(a, system, context, claims, k):
    """K independent verification passes; returns agree counts per claim.

    Claims are presented in a different order each pass so a position bias
    cannot vote K times for the same note. K<=1 returns k (=1) for every claim:
    the historical no-verification path."""
    n = len(claims)
    if k <= 1 or n == 0:
        return [max(k, 1)] * n
    agree = [0] * n
    rng = random.Random(len(context) * 7919 + n)
    for _ in range(k):
        order = list(range(n))
        rng.shuffle(order)
        lines = [f"[{pos}] {claims[idx]}" for pos, idx in enumerate(order)]
        user = context + "\n\ncandidate notes:\n" + "\n".join(lines)
        raw = call_model(a.url, a.model, system, user, temperature=VERIFY_TEMPERATURE)
        verdicts = parse_verdicts(raw, n)
        for pos, idx in enumerate(order):
            if verdicts[pos]:
                agree[idx] += 1
    return agree


def admit(items, agree, k, label="", text_key="note"):
    """Attach agreement, scale confidence by agree/K, keep the majority; log the rest."""
    need = (k // 2) + 1 if k > 1 else 1
    kept = []
    for it, ag in zip(items, agree):
        it["agree"] = ag
        it["confidence"] = round(max(0.0, min(1.0, float(it.get("confidence", 0.7)) * ag / max(k, 1))), 3)
        if ag >= need:
            kept.append(it)
        else:
            print(f"    {label}: rejected ({ag}/{k} verifications) {it.get(text_key)}")
    return kept


def source_tag(k, agree):
    return "extractor:v1" if k <= 1 else f"extractor:v2:k{k}:a{agree}"


PROSPECTIVE_SYSTEM = (
    "You are Seth Ford. {identity}\n\n"
    "You just reread your recent texts with {name}{rel}. List the things you still OWE or intend to "
    "follow up on, or that you should bring up when a topic comes back up — only real, open items "
    "from the texts (something you said you'd send/do/ask/check, something they're waiting on, an "
    "event of theirs to ask about later). Skip anything already done. Lowercase, each under 100 "
    "characters.\n\n"
    "Output ONLY a JSON array of at most {max_notes} objects: "
    "{{\"remember_to\": str, \"when_they_mention\": [1-3 short lowercase keywords likely to appear "
    "in their next text about it], \"days_valid\": integer 3-60}}. No prose."
)


def parse_prospective(text, max_notes):
    m = re.search(r"\[[\s\S]*\]", text)
    if not m:
        return []
    try:
        arr = json.loads(m.group(0))
    except Exception:
        return []
    out = []
    for o in arr if isinstance(arr, list) else []:
        if not isinstance(o, dict):
            continue
        action = (o.get("remember_to") or "").strip().rstrip(".")
        kws = [str(k).strip().lower() for k in (o.get("when_they_mention") or []) if str(k).strip()]
        # >= 4 chars: the live match is a substring test, and "mac" fires on "stomach".
        kws = [k for k in kws if 4 <= len(k) <= 40][:3]
        try:
            days = int(o.get("days_valid", 14))
        except Exception:
            days = 14
        days = max(3, min(60, days))
        if action and kws and len(action) <= 140:
            out.append({"action": action, "keywords": kws, "days": days})
        if len(out) >= max_notes:
            break
    return out


# Keyword quality (2026-09-13). The model was free to pick any 4+ letter word, and
# picked "work" on 18 open rows (7 intentions for ONE contact), so a single inbound
# "take off work" cued seven reminders. A cue must be (1) not a stop word, (2) rare
# in THIS contact's own inbound texts — a word in more than KEYWORD_MAX_SHARE of
# their last KEYWORD_SAMPLE texts is a topic, not a cue — and (3) back exactly one
# open intention per contact. The C matcher is whole-word too (prospective.c).
KEYWORD_STOP = {
    "work", "home", "house", "today", "tomorrow", "tonight", "yesterday", "week", "weekend",
    "morning", "night", "time", "thing", "things", "stuff", "good", "great", "nice", "okay",
    "yeah", "sure", "love", "like", "want", "need", "know", "think", "feel", "feeling",
    "going", "coming", "back", "later", "soon", "still", "just", "really", "maybe", "sorry",
    "thanks", "thank", "please", "hello", "there", "here", "what", "when", "where", "which",
    "this", "that", "them", "they", "your", "with", "have", "been", "will", "would", "could",
    "should", "about", "after", "before", "call", "text", "talk", "chat", "meet", "plan",
    "plans", "dinner", "lunch", "food", "money", "people", "family", "friend", "friends",
    "school", "class", "phone", "photo", "photos", "picture", "pictures", "video", "haha",
}
KEYWORD_MAX_SHARE = 0.05
KEYWORD_SAMPLE = 200


def keyword_pattern(kw):
    return re.compile(r"(?<![a-z0-9])" + re.escape(kw) + r"(?![a-z0-9])")


def inbound_texts(db, contact_id, n=KEYWORD_SAMPLE):
    rows = db.execute(
        "SELECT content FROM messages WHERE session_id = ? AND role = 'user' "
        "ORDER BY id DESC LIMIT ?", (contact_id, n)).fetchall()
    return [(r[0] or "").lower() for r in rows if r[0]]


def keyword_share(kw, texts):
    """Fraction of the contact's inbound texts containing kw as a whole word/phrase."""
    if not texts:
        return 0.0
    pat = keyword_pattern(kw)
    return sum(1 for t in texts if pat.search(t)) / len(texts)


def keyword_reject_reason(kw, texts, max_share=KEYWORD_MAX_SHARE):
    if kw in KEYWORD_STOP:
        return "stop word"
    if not re.search(r"[a-z]", kw):
        return "no letters"
    share = keyword_share(kw, texts)
    if share > max_share:
        return f"generic ({share:.0%} of their texts)"
    return None


def filter_keywords(kws, texts, open_cues, action):
    """open_cues: {keyword: action} of this contact's live triggers. Returns
    (kept, dropped[(kw, reason)]). A keyword already cueing a DIFFERENT open
    intention is dropped so one cue maps to one reminder; the same intention
    keeps its own cue (idempotent rerun)."""
    kept, dropped = [], []
    for kw in kws:
        reason = keyword_reject_reason(kw, texts)
        if reason is None and kw in open_cues and open_cues[kw] != action:
            reason = f"already cues '{open_cues[kw]}'"
        if reason:
            dropped.append((kw, reason))
        else:
            kept.append(kw)
    return kept, dropped


def open_cues_for(db, contact_id):
    rows = db.execute(
        "SELECT trigger_value, action FROM prospective_memories WHERE trigger_type='keyword' "
        "AND fired=0 AND contact_id=? ORDER BY id", (contact_id,)).fetchall()
    cues = {}
    for kw, action in rows:
        cues.setdefault(kw, action)
    return cues


def prune_pass(db, targets, write):
    """Retire already-written open triggers that fail the keyword rules: fired=2
    (a real fire is 1) so the rows stay auditable. Reports intentions left with
    no live cue. Read-only unless write=True."""
    retired = orphaned = 0
    for cid in targets:
        texts = inbound_texts(db, cid)
        rows = db.execute(
            "SELECT id, trigger_value, action FROM prospective_memories WHERE fired=0 AND "
            "trigger_type='keyword' AND contact_id=? ORDER BY id", (cid,)).fetchall()
        if not rows:
            continue
        owner = {}
        drop, live_actions, all_actions = [], set(), set()
        for rid, kw, action in rows:
            all_actions.add(action)
            reason = keyword_reject_reason(kw, texts)
            if reason is None:
                if kw in owner and owner[kw] != action:
                    reason = f"already cues '{owner[kw]}'"
                else:
                    owner.setdefault(kw, action)
            if reason:
                drop.append((rid, kw, action, reason))
            else:
                live_actions.add(action)
        for rid, kw, action, reason in drop:
            print(f"    {cid}: retire '{kw}' -> {action}: {reason}")
        lost = all_actions - live_actions
        print(f"{cid}: {len(rows)} open rows, retire {len(drop)}, "
              f"{len(lost)} intentions left without a cue")
        retired += len(drop)
        orphaned += len(lost)
        if write and drop:
            db.executemany("UPDATE prospective_memories SET fired=2 WHERE id=? AND fired=0",
                           [(d[0],) for d in drop])
            db.commit()
    print(f"prune {'applied' if write else 'dry-run'}: {retired} retired, "
          f"{orphaned} intentions orphaned")
    return {"retired": retired, "orphaned": orphaned}


def prospective_pass(db, a, identity, contacts, targets, now_ms):
    """Item 5: deferred intentions -> prospective_memories keyword triggers, which
    the reactive prompt already checks (daemon_reactive_prompt.c) and renders as
    "[PROSPECTIVE MEMORY: Remember to: ...]" when the contact's next text
    contains the keyword as a whole word. Keywords are stored lowercase; the
    trigger match is case-folded on the C side and each surfaced intention is
    retired (fired=1) after one render."""
    total = 0
    for cid in targets:
        meta = contacts.get(cid, {"name": cid, "relationship": ""})
        turns = recent_turns(db, cid, a.turns)
        if len(turns) < a.min_turns:
            continue
        system = PROSPECTIVE_SYSTEM.format(
            identity=identity, name=meta["name"],
            rel=(" (" + meta["relationship"] + ")") if meta["relationship"] else "",
            max_notes=a.max_notes)
        user = "recent texts (oldest first):\n" + "\n".join(turns)
        try:
            raw = call_model(a.url, a.model, system, user)
            items = parse_prospective(raw, a.max_notes)
            agree = verify_claims(
                a, VERIFY_SYSTEM.format(identity=identity, name=meta["name"],
                                        rel=(" (" + meta["relationship"] + ")") if meta["relationship"] else "",
                                        question="Here a note is supported only if the texts show it is "
                                                 "still an OPEN item you owe or should bring up later."),
                user, [it["action"] for it in items], a.consistency_k)
        except Exception as e:
            print(f"{cid} ({meta['name']}): model error {e}")
            continue
        items = admit(items, agree, a.consistency_k, label=f"{cid} ({meta['name']})",
                      text_key="action")
        texts = inbound_texts(db, cid)
        open_cues = open_cues_for(db, cid)
        print(f"{cid} ({meta['name']}): {len(items)} open intentions")
        for it in items:
            kept, dropped = filter_keywords(it["keywords"], texts, open_cues, it["action"])
            for kw, reason in dropped:
                print(f"         drop '{kw}': {reason}")
            it["keywords"] = kept
            for kw in kept:
                open_cues.setdefault(kw, it["action"])
            print(f"    [{it['days']:2d}d] {it['action']}  <- "
                  f"{', '.join(kept) if kept else '(no usable cue, skipped)'}")
        items = [it for it in items if it["keywords"]]
        if a.write and items:
            before = db.total_changes
            rows = []
            for it in items:
                for kw in it["keywords"]:
                    exists = db.execute(
                        "SELECT 1 FROM prospective_memories WHERE trigger_type='keyword' AND "
                        "trigger_value=? AND action=? AND contact_id=? AND fired=0",
                        (kw, it["action"], cid)).fetchone()
                    if exists:
                        continue
                    rows.append(("keyword", kw, it["action"], cid,
                                 now_ms // 1000 + it["days"] * 86400, now_ms // 1000))
            db.executemany(
                "INSERT INTO prospective_memories(trigger_type,trigger_value,action,contact_id,"
                "expires_at,created_at) VALUES(?,?,?,?,?,?)", rows)
            db.commit()
            new = db.total_changes - before
            total += new
            print(f"    wrote {new} new triggers")
    if a.write:
        # Retire past-due rows (fired=3) so "live" counts what can still fire, and
        # report fired=1 — the only number that proves the read side runs. Until
        # 2026-09-20 this line counted writes while the daemon never read them.
        expired = db.execute(
            "UPDATE prospective_memories SET fired=3 WHERE fired=0 AND expires_at > 0 AND "
            "expires_at <= strftime('%s','now')").rowcount
        db.commit()
        live = db.execute("SELECT COUNT(*) FROM prospective_memories WHERE fired=0 AND "
                          "expires_at > strftime('%s','now')").fetchone()[0]
        fired = db.execute("SELECT COUNT(*) FROM prospective_memories WHERE fired=1").fetchone()[0]
        print(f"prospective done: {total} new triggers, {live} live, {expired} expired, "
              f"{fired} fired all-time")


# ---- supersession (PGMem 2608.01708 / A-TMA 2607.01935 "ghost memory") ----
#
# An insight from January rendered next to one from September that contradicts
# it is the evolving-state tell. Nothing retired insights before this pass
# (hu_contact_insights_retire had no production caller). The model proposes
# (stale_id -> superseded_by_id) pairs per contact; K-sample majority admits a
# pair; a deterministic guard then refuses any pair where the "newer" note is
# not actually newer by evidence date. Dry-run (shadow) unless --write.

SUPERSEDE_SYSTEM = (
    "You are Seth Ford. {identity}\n\n"
    "Below are your private notes about {name}{rel}, each with an id and the month it was "
    "true as of. Some OLDER notes have been made stale by NEWER ones: a job/place/plan/"
    "relationship that changed, a plan that already happened, a thread that resolved. List "
    "only pairs where the newer note clearly replaces the older one. Do not pair notes that "
    "can both be true.\n\n"
    "Output ONLY a JSON array of objects: {{\"stale_id\": int, \"superseded_by_id\": int}}. "
    "No prose."
)


def parse_supersessions(text):
    m = re.search(r"\[[\s\S]*\]", text)
    if not m:
        return []
    try:
        arr = json.loads(m.group(0))
    except Exception:
        return []
    out = []
    for o in arr if isinstance(arr, list) else []:
        if not isinstance(o, dict):
            continue
        try:
            a_, b_ = int(o.get("stale_id")), int(o.get("superseded_by_id"))
        except Exception:
            continue
        if a_ != b_:
            out.append({"action": f"{a_}->{b_}", "stale": a_, "by": b_, "confidence": 1.0})
    return out


def supersession_candidates(pairs, agree, k, live):
    """Pairs verified by a majority of K passes that also pass the
    newer-by-evidence guard.

    pairs: [{"stale", "by"}] proposed once; agree: verification counts.
    live: {id: (as_of_ms, kind, insight)} for the contact's live rows.
    Returns (accepted, refused) where refused carries the reason."""
    need = (k // 2) + 1 if k > 1 else 1
    accepted, refused = [], []
    for c, ag in zip(pairs, agree):
        stale, by = c["stale"], c["by"]
        if ag < need:
            refused.append((stale, by, f"{ag}/{k} verifications"))
        elif stale not in live or by not in live:
            refused.append((stale, by, "unknown or already retired id"))
        elif live[by][0] <= live[stale][0]:
            refused.append((stale, by, "superseding note is not newer by evidence date"))
        else:
            accepted.append((stale, by))
    return accepted, refused


def supersede_pass(db, a, identity, contacts, targets, now_ms):
    total_acc = total_ref = 0
    for cid in targets:
        meta = contacts.get(cid, {"name": cid, "relationship": ""})
        rows = db.execute(
            "SELECT id, as_of_ms, kind, insight FROM contact_insights WHERE contact_id=? "
            "AND retired_at_ms=0 ORDER BY as_of_ms, id", (cid,)).fetchall()
        if len(rows) < 2:
            continue
        live = {r[0]: (r[1], r[2], r[3]) for r in rows}
        lines = [f"id={r[0]} (as of {time.strftime('%b %Y', time.gmtime(r[1] / 1000)) if r[1] else '?'}, "
                 f"{r[2]}): {r[3]}" for r in rows]
        system = SUPERSEDE_SYSTEM.format(
            identity=identity, name=meta["name"],
            rel=(" (" + meta["relationship"] + ")") if meta["relationship"] else "")
        user = "notes (oldest first):\n" + "\n".join(lines)
        try:
            pairs = parse_supersessions(call_model(a.url, a.model, system, user))
            pairs = [p_ for p_ in pairs if p_["stale"] in live and p_["by"] in live]
            claims = [f"note id={p_['stale']} ('{live[p_['stale']][2]}') is made stale by "
                      f"note id={p_['by']} ('{live[p_['by']][2]}')" for p_ in pairs]
            agree = verify_claims(
                a, VERIFY_SYSTEM.format(identity=identity, name=meta["name"],
                                        rel=(" (" + meta["relationship"] + ")") if meta["relationship"] else "",
                                        question="Here each 'note' is a claim that a NEWER note replaces "
                                                 "an OLDER one; support it only if both cannot be true "
                                                 "at once and the newer one is what holds now."),
                user, claims, a.consistency_k)
        except Exception as e:
            print(f"{cid} ({meta['name']}): model error {e}")
            continue
        accepted, refused = supersession_candidates(pairs, agree, a.consistency_k, live)
        for stale, by, why in refused:
            print(f"    {cid}: refuse {stale}->{by}: {why}")
        for stale, by in accepted:
            print(f"    {cid}: {'retire' if a.write else 'would retire'} {stale} "
                  f"'{live[stale][2]}' <- superseded by {by} '{live[by][2]}'")
        print(f"{cid} ({meta['name']}): {len(rows)} live, {len(accepted)} superseded, "
              f"{len(refused)} refused")
        total_acc += len(accepted)
        total_ref += len(refused)
        if a.write and accepted:
            db.executemany(
                "UPDATE contact_insights SET retired_at_ms=?, superseded_by_id=? "
                "WHERE id=? AND retired_at_ms=0",
                [(now_ms, by, stale) for stale, by in accepted])
            db.commit()
    print(f"supersede {'applied' if a.write else 'dry-run'}: {total_acc} retired, "
          f"{total_ref} refused")
    return {"retired": total_acc, "refused": total_ref}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prospective", action="store_true",
                    help="extract open intentions into prospective_memories instead of insights")
    ap.add_argument("--prune-triggers", action="store_true",
                    help="retire open prospective triggers whose keyword is generic, a stop "
                         "word, or already cues another intention (fired=2); no model call; "
                         "dry-run unless --write")
    ap.add_argument("--retire-superseded", action="store_true",
                    help="ask the model which older live insights newer ones replace; "
                         "K-sample majority + newer-by-evidence guard; dry-run unless --write")
    ap.add_argument("--consistency-k", type=int, default=1,
                    help="verification passes per candidate; a note must be supported by a "
                         "majority to be written and its confidence is scaled by agreement "
                         "(1 = off)")
    ap.add_argument("--contact")
    ap.add_argument("--turns", type=int, default=80)
    ap.add_argument("--min-turns", type=int, default=20)
    ap.add_argument("--max-notes", type=int, default=8)
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--url", default=DEFAULT_URL)
    ap.add_argument("--model", default=DEFAULT_MODEL)
    a = ap.parse_args()
    if not a.url.startswith("http://127.0.0.1") and not a.url.startswith("http://localhost"):
        print("refusing: the extractor reads real conversations and only talks to a local model",
              file=sys.stderr)
        return 2

    identity, contacts = load_persona()
    db = sqlite3.connect(MEMORY_DB)
    db.executescript(SCHEMA)
    migrate(db)
    targets = [a.contact] if a.contact else list(contacts)
    now_ms = int(time.time() * 1000)
    if a.prune_triggers:
        prune_pass(db, targets, a.write)
        return 0
    if a.retire_superseded:
        supersede_pass(db, a, identity, contacts, targets, now_ms)
        return 0
    if a.prospective:
        prospective_pass(db, a, identity, contacts, targets, now_ms)
        return 0
    total_new = 0
    for cid in targets:
        meta = contacts.get(cid, {"name": cid, "relationship": ""})
        turns = recent_turns(db, cid, a.turns)
        if len(turns) < a.min_turns:
            print(f"{cid} ({meta['name']}): {len(turns)} turns < {a.min_turns}, skipped")
            continue
        rows = recent_turn_rows(db, cid, a.turns)
        system, user = build_prompt(identity, meta["name"], meta["relationship"],
                                    numbered_turns(rows), a.max_notes)
        t0 = time.time()
        try:
            raw = call_model(a.url, a.model, system, user)
            notes = parse_notes(raw, a.max_notes)
            agree = verify_claims(
                a, VERIFY_SYSTEM.format(identity=identity, name=meta["name"],
                                        rel=(" (" + meta["relationship"] + ")") if meta["relationship"] else "",
                                        question="A note is supported only if a specific text states it."),
                user, [n["note"] for n in notes], a.consistency_k)
        except Exception as e:
            print(f"{cid} ({meta['name']}): model error {e}")
            continue
        notes = admit(notes, agree, a.consistency_k, label=f"{cid} ({meta['name']})")
        print(f"{cid} ({meta['name']}): {len(turns)} turns -> {len(notes)} notes "
              f"in {time.time() - t0:.1f}s (k={a.consistency_k})")
        for n in notes:
            ids, newest = evidence_for(n["evidence"], rows)
            n["evidence_ids"], n["as_of_ms"] = ids, (newest or now_ms)
            print(f"    [{n['kind']:10s} {n['confidence']:.2f} {n['agree']}/{a.consistency_k}"
                  f" ev={len(ids)}] {n['note']}")
        if not notes:
            print("    raw:", raw[:200].replace("\n", " "))
        if a.write and notes:
            before = db.total_changes
            db.executemany(
                "INSERT OR IGNORE INTO contact_insights"
                " (contact_id, kind, insight, confidence, as_of_ms, source, created_at_ms,"
                "  evidence_ids)"
                " VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                [(cid, n["kind"], n["note"], n["confidence"], n["as_of_ms"],
                  source_tag(a.consistency_k, n["agree"]), now_ms,
                  json.dumps(n["evidence_ids"]) if n["evidence_ids"] else None)
                 for n in notes])
            db.commit()
            new = db.total_changes - before
            total_new += new
            print(f"    wrote {new} new rows ({len(notes) - new} already present)")
    if a.write:
        live = db.execute("SELECT COUNT(*) FROM contact_insights WHERE retired_at_ms=0").fetchone()[0]
        print(f"done: {total_new} new rows, {live} live insights total")
    return 0


if __name__ == "__main__":
    sys.exit(main())
