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
  retired_at_ms INTEGER NOT NULL DEFAULT 0
);
CREATE UNIQUE INDEX IF NOT EXISTS idx_contact_insights_natural
  ON contact_insights(contact_id, insight);
CREATE INDEX IF NOT EXISTS idx_contact_insights_contact
  ON contact_insights(contact_id, retired_at_ms, as_of_ms DESC);
"""

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


def recent_turns(db, contact_id, n):
    rows = db.execute(
        "SELECT role, content FROM messages WHERE session_id = ? ORDER BY id DESC LIMIT ?",
        (contact_id, n)).fetchall()
    rows.reverse()
    out = []
    for role, content in rows:
        content = (content or "").strip().replace("\n", " ")
        if not content:
            continue
        who = "me" if role == "assistant" else "them"
        out.append(f"{who}: {content[:300]}")
    return out


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
        "\"confidence\": number 0-1}. No prose before or after."
    )
    user = "recent texts (oldest first):\n" + "\n".join(turns)
    return system, user


def call_model(url, model, system, user, timeout=300):
    req = {
        "model": model,
        "messages": [{"role": "system", "content": system}, {"role": "user", "content": user}],
        "max_tokens": 700,
        "temperature": 0.3,
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
        seen.add(note.lower())
        notes.append({"note": note, "kind": kind, "confidence": conf})
        if len(notes) >= max_notes:
            break
    return notes


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


def prospective_pass(db, a, identity, contacts, targets, now_ms):
    """Item 5: deferred intentions -> prospective_memories keyword triggers, which
    the reactive prompt already checks (daemon_reactive_prompt.c) and renders as
    "[PROSPECTIVE MEMORY: Remember to: ...]" when the contact's next text
    contains the keyword. Keywords are stored lowercase; the trigger match is
    case-folded on the C side."""
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
        except Exception as e:
            print(f"{cid} ({meta['name']}): model error {e}")
            continue
        items = parse_prospective(raw, a.max_notes)
        print(f"{cid} ({meta['name']}): {len(items)} open intentions")
        for it in items:
            print(f"    [{it['days']:2d}d] {it['action']}  <- {', '.join(it['keywords'])}")
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
        live = db.execute("SELECT COUNT(*) FROM prospective_memories WHERE fired=0 AND "
                          "expires_at > strftime('%s','now')").fetchone()[0]
        print(f"prospective done: {total} new triggers, {live} live")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--prospective", action="store_true",
                    help="extract open intentions into prospective_memories instead of insights")
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
    targets = [a.contact] if a.contact else list(contacts)
    now_ms = int(time.time() * 1000)
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
        system, user = build_prompt(identity, meta["name"], meta["relationship"], turns,
                                    a.max_notes)
        t0 = time.time()
        try:
            raw = call_model(a.url, a.model, system, user)
        except Exception as e:
            print(f"{cid} ({meta['name']}): model error {e}")
            continue
        notes = parse_notes(raw, a.max_notes)
        print(f"{cid} ({meta['name']}): {len(turns)} turns -> {len(notes)} notes "
              f"in {time.time() - t0:.1f}s")
        for n in notes:
            print(f"    [{n['kind']:10s} {n['confidence']:.2f}] {n['note']}")
        if not notes:
            print("    raw:", raw[:200].replace("\n", " "))
        if a.write and notes:
            before = db.total_changes
            db.executemany(
                "INSERT OR IGNORE INTO contact_insights"
                " (contact_id, kind, insight, confidence, as_of_ms, source, created_at_ms)"
                " VALUES (?, ?, ?, ?, ?, ?, ?)",
                [(cid, n["kind"], n["note"], n["confidence"], now_ms, SOURCE, now_ms)
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
