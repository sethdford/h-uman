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
  scripts/insight_stream.py --names [--names-days 2] [--deadline HH:MM] [--write]
"""
import argparse
import contextlib
import datetime as dt
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


def build_wide_prompt(identity, name, relationship, turns, max_notes):
    """The curator's variant (spec §5): names keep their capitals, a structured
    names field, [tN]-only evidence."""
    system = (
        f"You are Seth Ford. {identity}\n\n"
        f"You just reread your recent texts with {name}"
        f"{' (' + relationship + ')' if relationship else ''} and are jotting private notes to "
        "yourself — the things YOU would actually remember and bring up next time: specific names, "
        "places, plans with when, what they're dealing with, running jokes and inside references, "
        "what they like and don't. Never generic traits (\"is friendly\"), never advice, never "
        "anything not in the texts. Casual lowercase like a note to yourself, but keep names of "
        "people, places and organizations capitalized as written; present tense, each under "
        "110 characters.\n\n"
        f"Output ONLY a JSON array of at most {max_notes} objects: "
        "{\"note\": str, \"kind\": \"fact\"|\"thread\"|\"plan\"|\"preference\"|\"inside_ref\", "
        "\"confidence\": number 0-1, \"evidence\": [the [tN] labels of the texts the note "
        "comes from; never a [dN]], \"names\": [{\"name\": str, \"type\": \"person\"|\"place\"|"
        "\"org\"|\"event\"} for every specific name in the note]}. No prose before or after."
    )
    user = "recent texts (oldest first):\n" + "\n".join(turns)
    return system, user


def build_prompt(identity, name, relationship, turns, max_notes, wide=False):
    # wide=False is the LIVE persona pass: its wording below is unchanged from
    # what it was measured with and must stay so (pinned by a byte-equality test).
    if wide:
        return build_wide_prompt(identity, name, relationship, turns, max_notes)
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


def call_model(url, model, system, user, timeout=300, temperature=0.3, max_tokens=700):
    req = {
        "model": model,
        "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
        "max_tokens": max_tokens,
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
        raw_ev = o.get("evidence") if isinstance(o.get("evidence"), list) else []
        names = []
        for nm in (o.get("names") if isinstance(o.get("names"), list) else []):
            if not isinstance(nm, dict):
                continue
            nname = str(nm.get("name") or "").strip()
            ntype = str(nm.get("type") or "").strip().lower()
            if nname and len(nname) <= 60 and ntype in {"person", "place", "org", "event"}:
                names.append({"name": nname, "type": ntype})
        seen.add(note.lower())
        notes.append({"note": note, "kind": kind, "confidence": conf, "evidence": ev,
                      "evidence_tokens": [str(e) for e in raw_ev], "names": names})
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


def admit(items, agree, k, label="", text_key="note", quiet=False):
    """Attach agreement, scale confidence by agree/K, keep the majority; log the
    rest unless quiet (the wide pass must never print note text or handles)."""
    need = (k // 2) + 1 if k > 1 else 1
    kept = []
    for it, ag in zip(items, agree):
        it["agree"] = ag
        it["confidence"] = round(max(0.0, min(1.0, float(it.get("confidence", 0.7)) * ag / max(k, 1))), 3)
        if ag >= need:
            kept.append(it)
        elif not quiet:
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


import curator_evidence as ce  # noqa: E402  (scripts/ is on sys.path when run as a script)
import curator_population as cp  # noqa: E402
import curator_names as cn  # noqa: E402

WIDE_SOURCE_PREFIX = "curator_wide:"
CURATOR_STATE = os.path.join(HOME, ".human/curator_state.json")


def retire_suppressed(db, suppressed, now_ms, write):
    """Opt-out takes effect on what is already known, not only on what is
    learned next: every live row for a suppressed contact is retired."""
    if not suppressed:
        return 0
    qs = ",".join("?" * len(suppressed))
    n = db.execute(f"SELECT COUNT(*) FROM contact_insights WHERE retired_at_ms=0 AND"
                   f" contact_id IN ({qs})", tuple(suppressed)).fetchone()[0]
    if write and n:
        db.execute(f"UPDATE contact_insights SET retired_at_ms=? WHERE retired_at_ms=0 AND"
                   f" contact_id IN ({qs})", (now_ms, *suppressed))
        db.commit()
    return n


def order_by_last_run(handles, state):
    return sorted(handles, key=lambda h: (state.get(h, 0), h))


def write_manifest(manifest_dir, now, man, prefix="curator-manifest"):
    """Counts only. Atomic (tmp + os.replace) so a killed run never leaves a
    truncated manifest; a dry run is written as ...-dryrun.json so it can
    never be read as a night's real numbers."""
    if not man.get("eligible"):
        print("refusing: 0 eligible contacts (no manifest written)", file=sys.stderr)
        return 2
    os.makedirs(manifest_dir, exist_ok=True)
    suffix = "-dryrun" if man.get("dry_run") else ""
    path = os.path.join(manifest_dir, f"{prefix}-{now.strftime('%Y%m%d')}{suffix}.json")
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(man, f, indent=1, sort_keys=True)
    os.replace(tmp, path)
    return 0


DEADLINE_ROLL_HOURS = 12


def resolve_deadline(hhmm, now_local):
    """Today's HH:MM (local) while it is still ahead of now_local. If it
    passed less than DEADLINE_ROLL_HOURS ago the window is closed -> None
    (launchd runs a missed 05:10 job on wake; rolling that to tomorrow's
    07:30 would load the live :8741 server for ~22h). If it passed 12h or
    more ago, the same HH:MM tomorrow (an evening manual run). Returns a
    tz-aware UTC datetime or None."""
    hh, mm = map(int, hhmm.split(":"))
    candidate = now_local.replace(hour=hh, minute=mm, second=0, microsecond=0)
    if candidate > now_local:
        return candidate.astimezone(dt.timezone.utc)
    if now_local - candidate < dt.timedelta(hours=DEADLINE_ROLL_HOURS):
        return None
    return (candidate + dt.timedelta(days=1)).astimezone(dt.timezone.utc)


def load_curator_state():
    """Missing file -> {} (first run). Unreadable/corrupt/undecodable file ->
    warn to stderr and proceed with {} rather than crash the curator on a bad
    write (e.g. a truncated file from a killed prior run). Entries whose value
    is not an int (last-run epoch ms) are dropped."""
    if not os.path.exists(CURATOR_STATE):
        return {}
    try:
        with open(CURATOR_STATE) as f:
            data = json.load(f)
    except (OSError, ValueError) as e:  # ValueError covers JSON and Unicode decode errors
        print(f"warning: curator_state.json unreadable ({type(e).__name__}); "
              "starting from empty state", file=sys.stderr)
        return {}
    if not isinstance(data, dict):
        print("warning: curator_state.json is not a JSON object; starting from empty state",
              file=sys.stderr)
        return {}
    return {h: v for h, v in data.items() if type(v) is int}


def save_curator_state(state):
    """Atomic write: json to a sibling .tmp file, then os.replace -- so a
    crash or kill mid-write never leaves CURATOR_STATE truncated."""
    tmp = CURATOR_STATE + ".tmp"
    with open(tmp, "w") as f:
        json.dump(state, f)
    os.replace(tmp, CURATOR_STATE)


HUMAN_CONFIG = os.path.join(HOME, ".human/config.json")
WIDE_MAX_TOKENS = 1500  # names + evidence per note need more room than the persona pass's 700
NAMES_REJECTED_BLOCK = 0.20  # spec §6: > 20% of names failing "was it said?" blocks promotion
WIDE_INT_COUNTERS = (
    "eligible", "excluded_suppressed", "excluded_never", "skipped_min_turns", "curated",
    "model_errors", "unreached_at_deadline", "stopped_at_deadline", "parse_failed",
    "notes_kept", "notes_written", "rejected_no_evidence", "rejected_daemon_evidence",
    "rejected_name_not_said", "rejected_verification", "names_proposed", "names_rejected",
    "names_total", "notes_named")


def _utc_now():
    return dt.datetime.now(dt.timezone.utc)


def _local_now():
    return dt.datetime.now().astimezone()


def _is_empty_array(text):
    try:
        return json.loads(text.strip()) == []
    except ValueError:
        return False


def curate_contact(a, identity, h, rows):
    """One contact's model work: generate, deterministic evidence/name
    checks, then K-vote verification of the survivors only. Returns (kept
    notes, per-contact counts). Raises on any model error; the caller merges
    the counts only on success, so an errored contact leaves no partial
    numbers. Prints nothing (no note text or handle may reach a log)."""
    c = dict.fromkeys(WIDE_INT_COUNTERS, 0)
    lines, cite = ce.number_rows(rows)
    system, user = build_prompt(identity, h, "", lines, a.max_notes, wide=True)
    raw = call_model(a.url, a.model, system, user, max_tokens=WIDE_MAX_TOKENS)
    notes = parse_notes(raw, a.max_notes)
    if raw.strip() and not notes and not _is_empty_array(raw):
        c["parse_failed"] = 1
    valid = []
    for n in notes:
        v, reason, checked, unsaid = ce.assess_note(n, cite)
        c["names_proposed"] += len(checked)
        c["names_rejected"] += len(unsaid)
        if v is None:
            c[f"rejected_{reason}"] += 1
        else:
            valid.append(v)
    # The verifier sees only citable human rows: a [dN] daemon line could
    # otherwise "support" a note the daemon itself confabulated.
    context = "recent texts (oldest first):\n" + "\n".join(
        ln for ln in lines if ln.startswith("[t"))
    agree = verify_claims(a, VERIFY_SYSTEM.format(
        identity=identity, name=h, rel="",
        question="A note is supported only if a specific text states it."),
        context, [v["note"] for v in valid], a.consistency_k)
    kept = admit(valid, agree, a.consistency_k, quiet=True)
    c["rejected_verification"] = len(valid) - len(kept)
    c["notes_kept"] = len(kept)
    c["names_total"] = sum(len(v["names"]) for v in kept)
    c["notes_named"] = sum(1 for v in kept if v["names"])
    return kept, c


def write_wide_notes(db, h, kept, a, now_ms):
    before = db.total_changes
    db.executemany(
        "INSERT OR IGNORE INTO contact_insights (contact_id, kind, insight, confidence,"
        " as_of_ms, source, created_at_ms, evidence_ids) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        [(h, v["kind"], v["note"], v["confidence"],
          max(r[1] for r in v["evidence_rows"]),
          WIDE_SOURCE_PREFIX + source_tag(a.consistency_k, v["agree"]), now_ms,
          json.dumps([f"chat:{r[0]}" for r in v["evidence_rows"]])) for v in kept])
    db.commit()
    return db.total_changes - before


def finish_manifest(man, t0):
    man["elapsed_s"] = round(time.monotonic() - t0, 3)
    rate = man["names_rejected"] / man["names_proposed"] if man["names_proposed"] else 0.0
    man["names_rejected_rate"] = round(float(rate), 4)
    man["promotion_blocked"] = rate > NAMES_REJECTED_BLOCK
    return man


def wide_pass(db, a, identity, persona_ids, att, now, write, deadline=None, state=None,
              exclude=()):
    """Curate every eligible non-persona 1:1 contact from chat.db (spec §3-5).
    Returns a counts-only manifest; never text or handles. Every eligible
    contact lands in exactly one of excluded_suppressed, excluded_never,
    skipped_min_turns, curated, model_errors, unreached_at_deadline. `state`
    is updated only for curated contacts. `exclude`: loopback handle(s)."""
    t0 = time.monotonic()
    man = dict.fromkeys(WIDE_INT_COUNTERS, 0)
    man["dry_run"] = not write
    suppressed = cp.load_suppressed(MEMORY_DB)  # module global so tests can redirect it
    never = cp.load_never(getattr(a, "never_path", cp.NEVER_PATH))
    stale = cp.unmatched_never(never, att["timelines"])
    if stale:
        print(f"warning: {stale} curator_never entries match no chat.db handle",
              file=sys.stderr)
    window_days = getattr(a, "window_days", cp.WINDOW_DAYS)
    cutoff = now - dt.timedelta(days=window_days)
    handles = cp.eligible_handles(att["timelines"], persona_ids, now, window_days=window_days,
                                  exclude=exclude)
    man["eligible"] = len(handles)
    now_ms = int(now.timestamp() * 1000)
    ordered = order_by_last_run(handles, state if state is not None else {})
    for i, h in enumerate(ordered):
        if deadline is not None and _utc_now() >= deadline:
            man["stopped_at_deadline"] = 1
            man["unreached_at_deadline"] = len(ordered) - i
            break
        why = cp.exclusion_reason(h, suppressed, never)
        if why:
            man[f"excluded_{why}"] += 1
            continue
        rows = ce.chat_turn_rows(att["timelines"][h], att["labels"], a.turns, cutoff)
        if len(rows) < a.min_turns:
            man["skipped_min_turns"] += 1
            continue
        try:
            kept, counts = curate_contact(a, identity, h, rows)
        except Exception:  # one contact's model failure must not abort the night
            man["model_errors"] += 1
            continue
        for k, v in counts.items():
            man[k] += v
        man["curated"] += 1
        if write and kept:
            man["notes_written"] += write_wide_notes(db, h, kept, a, now_ms)
        if state is not None:
            state[h] = now_ms
    return finish_manifest(man, t0)


def preflight(a):
    """Refusals shared by the chat.db passes -> (exit code or None, loopback handles).
    chat.db unreadable, model server down, or a malformed never-file/config.json."""
    try:
        con = sqlite3.connect(f"file:{a.chat_db}?mode=ro", uri=True)
        con.execute("SELECT 1 FROM message LIMIT 1").fetchall()
        con.close()
    except sqlite3.Error as e:
        print(f"refusing: chat.db unreadable ({e}); grant Full Disk Access to this "
              "python for the launchd job", file=sys.stderr)
        return 2, set()
    try:
        urllib.request.urlopen(a.url.rsplit("/v1/", 1)[0] + "/health", timeout=5)
    except Exception as e:
        print(f"refusing: model server down ({e})", file=sys.stderr)
        return 2, set()
    try:
        cp.load_never(a.never_path)
        return None, cp.load_loopback_handles(HUMAN_CONFIG)
    except (ValueError, json.JSONDecodeError) as e:
        print(f"refusing: {e}", file=sys.stderr)
        return 2, set()


def run_wide(a, db, identity, contacts, now_ms):
    """The --population wide dispatch. Order of effects:
    1. deadline window closed (--deadline passed < 12h ago) -> exit 0, no writes;
    2. refusals, exit 2, before any write: chat.db unreadable, model server
       down, malformed never-file or config.json, 0 eligible contacts;
    3. load curator_state.json; read suppressions and retire their live
       insights (--write); run the curator pass (rows written per contact,
       --write) -- a sqlite3.Error from memory.db here refuses with exit 2
       before the state file or manifest is written (R5, fail closed), though
       rows already committed stay;
    4. save curator_state.json (--write only), then write the counts-only
       manifest (dry runs as ...-dryrun.json);
    5. exit 3 if every contact the pass attempted hit a model error."""
    import eval_conversation_quality as cq
    deadline = None
    if a.deadline:
        deadline = resolve_deadline(a.deadline, _local_now())
        if deadline is None:
            print(f"window closed: deadline {a.deadline} passed less than "
                  f"{DEADLINE_ROLL_HOURS}h ago; nothing to do", file=sys.stderr)
            return 0
    rc, loopback = preflight(a)
    if rc is not None:
        return rc
    now = dt.datetime.now(dt.timezone.utc)
    att = cq.attribute(a.chat_db, MEMORY_DB, now - dt.timedelta(days=a.window_days))
    persona_ids = set(contacts)
    # Refuse BEFORE any write when there is nothing to curate -- otherwise
    # a --write run with 0 eligible contacts still retires suppressed rows
    # and writes curator_state.json before write_manifest's own refusal,
    # leaving side effects behind a run that reports "refused".
    if not cp.eligible_handles(att["timelines"], persona_ids, now, window_days=a.window_days,
                               exclude=loopback):
        print("refusing: 0 eligible contacts (no manifest written)", file=sys.stderr)
        return 2
    state = load_curator_state()
    try:
        suppressed = cp.load_suppressed(MEMORY_DB)
        retired = retire_suppressed(db, suppressed, now_ms, a.write)
        man = wide_pass(db, a, identity, persona_ids, att, now, a.write, deadline, state,
                        exclude=loopback)
    except sqlite3.Error as e:
        print(f"refusing: memory.db unreadable ({e})", file=sys.stderr)
        return 2
    man["retired_suppressed"] = retired
    if a.write:
        save_curator_state(state)
    rc = write_manifest(a.manifest_dir, now, man)
    if man["promotion_blocked"]:
        print(f"promotion blocked: {man['names_rejected_rate']:.0%} of proposed names were "
              "not said in their evidence (> 20%)", file=sys.stderr)
    attempted = man["curated"] + man["model_errors"]
    if attempted and man["model_errors"] == attempted:
        print(f"every attempted contact ({attempted}) hit a model error; see the manifest",
              file=sys.stderr)
        return 3
    return rc


# ---- nightly typed-name pass (spec 2026-09-29 named-entity-extraction §4.4) ----
NAMES_COUNTERS = (
    "eligible", "contacts", "excluded_suppressed", "excluded_never", "skipped_no_text",
    "model_errors", "names_proposed", "names_kept", "names_rejected", "import_entities",
    "import_failed", "unreached_at_deadline", "stopped_at_deadline")
NAMES_DIR = os.path.join(HOME, ".human/names")
GRAPH_DB = os.path.join(HOME, ".human/graph.db")
HUMAN_BIN = os.path.join(HOME, ".local/bin/human-daemon")
NAMES_MAX_TOKENS = 1200


def names_eligible(att, now, names_days, window_days, exclude):
    """The wide pass's enumeration WITHOUT its persona skip (persona contacts are the
    ones the daemon replies to), narrowed to handles with any message, either
    direction, in the last names_days."""
    handles = cp.eligible_handles(att["timelines"], set(), now, window_days=window_days,
                                  exclude=exclude)
    cutoff = now - dt.timedelta(days=names_days)
    return [h for h in handles if any(m["t"] >= cutoff for m in att["timelines"][h])]


def names_contact(a, rows, display_name):
    """One contact: one model call (thinking suppressed by call_model), deterministic
    verification. -> (kept, proposed count, rejected count). Raises on a model error."""
    lines, cite = ce.number_rows(rows)
    system, user = cn.build_prompt(lines)
    proposed = cn.parse_names(call_model(a.url, a.model, system, user,
                                         max_tokens=NAMES_MAX_TOKENS))
    kept, rejected = cn.verify_names(proposed, cite, cn.drop_names(display_name))
    return kept, len(proposed), rejected


def names_pass(a, contacts, att, now, deadline=None, exclude=()):
    """-> (counts-only manifest, entity lines). Every eligible handle lands in exactly
    one of excluded_suppressed, excluded_never, skipped_no_text, contacts,
    model_errors, unreached_at_deadline. A model error is recorded by exception TYPE
    name only (model_error_types): its message could quote a text or a handle."""
    man = dict.fromkeys(NAMES_COUNTERS, 0)
    man["by_type"] = dict.fromkeys(cn.TYPES, 0)
    man["model_error_types"] = {}
    man["dry_run"] = not a.write
    suppressed = cp.load_suppressed(MEMORY_DB)
    never = cp.load_never(getattr(a, "never_path", cp.NEVER_PATH))
    handles = names_eligible(att, now, a.names_days, a.window_days, exclude)
    man["eligible"] = len(handles)
    cutoff = now - dt.timedelta(days=a.names_days)
    lines = []
    for i, h in enumerate(handles):
        if deadline is not None and _utc_now() >= deadline:
            man["stopped_at_deadline"] = 1
            man["unreached_at_deadline"] = len(handles) - i
            break
        why = cp.exclusion_reason(h, suppressed, never)
        if why:
            man[f"excluded_{why}"] += 1
            continue
        rows = ce.chat_turn_rows(att["timelines"][h], att["labels"], a.turns, cutoff)
        if not any(r[2] != "daemon" for r in rows):
            man["skipped_no_text"] += 1
            continue
        try:
            kept, proposed, rejected = names_contact(a, rows, (contacts.get(h) or {}).get("name"))
        except Exception as e:  # one contact's model failure must not abort the night
            man["model_errors"] += 1
            kind = type(e).__name__
            man["model_error_types"][kind] = man["model_error_types"].get(kind, 0) + 1
            continue
        man["contacts"] += 1
        man["names_proposed"] += proposed
        man["names_rejected"] += rejected
        man["names_kept"] += len(kept)
        for k in kept:
            man["by_type"][k["type"]] += 1
        lines.extend(cn.entity_lines(h, kept))
    return man, lines


def import_names(a, man, lines, now):
    """Kept names -> a 0600 JSONL in --names-dir -> (--write only) the C importer.
    A --write run deletes the JSONL once the importer returns (it holds handles and
    names); a dry run keeps it as ...-dryrun.jsonl, its only inspectable output.
    No kept names -> no file and no importer call (it would exit 1 on N+E == 0).
    -> 2 if the import failed (non-zero exit, timeout, or no JSON counts), else 0."""
    if not lines:
        return 0
    suffix = "" if a.write else "-dryrun"
    path = cn.write_jsonl_private(
        os.path.join(a.names_dir, f"names-{now.strftime('%Y%m%d')}{suffix}.jsonl"), lines)
    if not a.write:
        return 0
    try:
        entities, code = cn.run_import(a.human_bin, a.graph_db, path)
    finally:
        with contextlib.suppress(OSError):
            os.unlink(path)
    if code != 0 or entities is None:
        man["import_failed"] = 1
        print(f"import failed: `human memory import-facts` exited {code}"
              f"{'' if entities is not None else ' without JSON counts'}; nothing written",
              file=sys.stderr)
        return 2
    man["import_entities"] = entities
    return 0


def run_names(a, contacts):
    """The --names dispatch. Order of effects:
    1. deadline window closed -> exit 0, nothing written;
    2. refusals, exit 2, nothing written: chat.db unreadable, model server down,
       malformed never-file/config.json, --write without an executable
       --human-bin, 0 eligible contacts, memory.db unreadable;
    3. the pass; kept names -> a 0600 JSONL in --names-dir;
    4. --write only: `human memory import-facts` (HU_GRAPH_DB=--graph-db), then the
       JSONL is deleted;
    5. counts-only manifest names-manifest-YYYYMMDD[-dryrun].json;
    6. exit 3 if every attempted contact hit a model error; exit 2 if the import
       failed (nothing written)."""
    import eval_conversation_quality as cq
    deadline = None
    if a.deadline:
        deadline = resolve_deadline(a.deadline, _local_now())
        if deadline is None:
            print(f"window closed: deadline {a.deadline} passed less than "
                  f"{DEADLINE_ROLL_HOURS}h ago; nothing to do", file=sys.stderr)
            return 0
    rc, loopback = preflight(a)
    if rc is not None:
        return rc
    if a.write and not (os.path.isfile(a.human_bin) and os.access(a.human_bin, os.X_OK)):
        print(f"refusing: human binary not executable ({a.human_bin})", file=sys.stderr)
        return 2
    now = _utc_now()
    att = cq.attribute(a.chat_db, MEMORY_DB, now - dt.timedelta(days=a.window_days))
    if not names_eligible(att, now, a.names_days, a.window_days, loopback):
        print("refusing: 0 eligible contacts (no manifest written)", file=sys.stderr)
        return 2
    t0 = time.monotonic()
    try:
        man, lines = names_pass(a, contacts, att, now, deadline, exclude=loopback)
    except sqlite3.Error as e:
        print(f"refusing: memory.db unreadable ({e})", file=sys.stderr)
        return 2
    rc = import_names(a, man, lines, now)
    man["elapsed_s"] = round(time.monotonic() - t0, 3)
    wrc = write_manifest(a.manifest_dir, now, man, prefix="names-manifest")
    attempted = man["contacts"] + man["model_errors"]
    if attempted and man["model_errors"] == attempted:
        print(f"every attempted contact ({attempted}) hit a model error; see the manifest",
              file=sys.stderr)
        return 3
    return rc or wrc


WIDE_REFUSED_FLAGS = (("contact", "--contact"), ("prospective", "--prospective"),
                      ("retire_superseded", "--retire-superseded"),
                      ("prune_triggers", "--prune-triggers"))


def main(argv=None):
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
    ap.add_argument("--population", choices=["persona", "wide"], default="persona")
    ap.add_argument("--chat-db", default=os.path.join(HOME, "Library/Messages/chat.db"))
    ap.add_argument("--window-days", type=int, default=30)
    ap.add_argument("--deadline", help="HH:MM local; stop before the next contact after this")
    ap.add_argument("--manifest-dir", default=os.path.join(HOME, ".human/logs"))
    ap.add_argument("--never-path", default=os.path.join(HOME, ".human/curator_never.json"))
    ap.add_argument("--names", action="store_true",
                    help="nightly typed-name pass: the local model lists the names in each "
                         "eligible contact's last --names-days of texts, each verified against "
                         "a cited text, imported into graph.db via `human memory import-facts` "
                         "(--write); dry-run otherwise")
    ap.add_argument("--names-days", type=int, default=2)
    ap.add_argument("--names-dir", default=NAMES_DIR)
    ap.add_argument("--graph-db", default=GRAPH_DB)
    ap.add_argument("--human-bin", default=HUMAN_BIN)
    a = ap.parse_args(argv)
    if a.names:
        bad = [flag for attr, flag in WIDE_REFUSED_FLAGS if getattr(a, attr)]
        if a.population == "wide":
            bad.append("--population wide")
        if bad:
            print(f"refusing: {', '.join(bad)} cannot be combined with --names",
                  file=sys.stderr)
            return 2
    if a.population == "wide":
        bad = [flag for attr, flag in WIDE_REFUSED_FLAGS if getattr(a, attr)]
        if bad:
            print(f"refusing: {', '.join(bad)} cannot be combined with --population wide "
                  "(persona-pass options)", file=sys.stderr)
            return 2
    if not a.url.startswith("http://127.0.0.1") and not a.url.startswith("http://localhost"):
        print("refusing: the extractor reads real conversations and only talks to a local model",
              file=sys.stderr)
        return 2

    identity, contacts = load_persona()
    if a.names:
        return run_names(a, contacts)
    db = sqlite3.connect(MEMORY_DB)
    db.executescript(SCHEMA)
    migrate(db)
    targets = [a.contact] if a.contact else list(contacts)
    now_ms = int(time.time() * 1000)
    if a.population == "wide":
        return run_wide(a, db, identity, contacts, now_ms)
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
