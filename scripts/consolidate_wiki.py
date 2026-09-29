#!/usr/bin/env python3
"""Sleep-time consolidation (better-than-human item 4): compile one budgeted,
linted page per contact (plus self) from the local stores, so the prompt can
read a slice of a page instead of a pile of rows.

  <state>/wiki/<contact_id>.md   sections: now / open threads / what I remember /
  <state>/wiki/self.md                     register with them

Deterministic v1 — no model call. Every bullet ends in a provenance tag
([ins:ID] contact_insights, [pm:ID,...] prospective_memories, [persona:path]
seth.json) so a wrong line can be traced and retired; nothing is written that
is not a row somewhere. The item-4 design allowed a local-GLM compile; that is a
follow-up only if the deterministic dedupe proves too weak, because a rewrite
step is the one place invention could enter.

Lint before publish (refuse, don't fall back): every non-heading line carries a
tag; a page is <= --max-bytes; the self page repeats the persona's core identity
sentence verbatim (employer/city agreement by construction). One failing page
means NOTHING is written and the exit code is non-zero.

The C reader is src/memory/wiki_page.c behind HU_WIKI_HEAD (off|shadow|live),
which takes the first ~1.2 KB of a page, so the sections are ordered by value.

Usage: consolidate_wiki.py [--write] [--out DIR] [--contact ID] [--max-bytes N]
Dry-run (default) prints the pages and the lint verdict, writes nothing.
"""
import argparse
import json
import os
import re
import sqlite3
import sys
import time

HOME = os.path.expanduser("~")
STATE = os.environ.get("HU_STATE_DIR") or os.path.join(HOME, ".human")
MEMORY_DB = os.path.join(STATE, "memory.db")
PERSONA = os.path.join(STATE, "personas", "seth.json")
OUT_DIR = os.path.join(STATE, "wiki")

MAX_BYTES = 2048
MAX_INSIGHTS = 10
MAX_PER_KIND = 4
MAX_THREADS = 6
MIN_CONFIDENCE = 0.5
DUP_JACCARD = 0.6
SKIP_RELATIONSHIPS = {"test"}

SAFE_ID = re.compile(r"^[A-Za-z0-9+@_\-][A-Za-z0-9+@._\-]{0,95}$")
TAG = re.compile(r" \[(ins|pm|persona):[^\]\s]+\]$")
MONTHS = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]


def safe_id(cid):
    return bool(SAFE_ID.match(cid)) and ".." not in cid


def month(ms):
    t = time.gmtime(ms / 1000)
    return f"{MONTHS[t.tm_mon - 1]} {t.tm_year}"


ABBREV = {"st", "mr", "mrs", "ms", "dr", "jr", "sr", "vs", "etc", "e.g", "i.e", "no", "ft", "mt"}


def first_sentence(text, limit=220):
    """First sentence of a persona string; a period after St./Dr./etc. is not an end."""
    text = " ".join((text or "").split())
    s = text
    for m in re.finditer(r"[.!?](?=\s|$)", text):
        before = re.search(r"([A-Za-z.]+)$", text[:m.start()])
        if m.group() == "." and before and before.group(1).lower() in ABBREV:
            continue
        s = text[:m.end()]
        break
    return s if len(s) <= limit else s[:limit - 1].rstrip() + "…"


def tokens(text):
    return set(re.findall(r"[a-z0-9]+", text.lower())) - {"the", "a", "an", "and", "of", "to",
                                                          "in", "on", "at", "for", "is", "it"}


def dedupe(notes):
    """Drop a note whose token set overlaps an earlier (newer) one at >= DUP_JACCARD."""
    kept, seen = [], []
    for n in notes:
        t = tokens(n["insight"])
        if not t:
            continue
        if any(len(t & s) / len(t | s) >= DUP_JACCARD for s in seen):
            continue
        seen.append(t)
        kept.append(n)
    return kept


def contacts_from(persona):
    out = {}
    for cid, c in (persona.get("contacts") or {}).items():
        rel = (c.get("relationship") or "").lower()
        name = c.get("name") or cid
        if rel in SKIP_RELATIONSHIPS or name.lower().startswith("unknown"):
            continue
        out[cid] = c
    return out


def insights_for(db, cid):
    rows = db.execute(
        "SELECT id, kind, insight, as_of_ms FROM contact_insights WHERE contact_id=? AND "
        "retired_at_ms=0 AND confidence>=? ORDER BY as_of_ms DESC, id DESC",
        (cid, MIN_CONFIDENCE)).fetchall()
    notes = dedupe([{"id": r[0], "kind": r[1], "insight": " ".join(r[2].split()), "as_of": r[3]}
                    for r in rows if r[2]])
    per_kind, out = {}, []
    for n in notes:
        if per_kind.get(n["kind"], 0) >= MAX_PER_KIND:
            continue
        per_kind[n["kind"]] = per_kind.get(n["kind"], 0) + 1
        out.append(n)
        if len(out) >= MAX_INSIGHTS:
            break
    return out


def threads_for(db, cid, now_s):
    rows = db.execute(
        "SELECT id, trigger_value, action, created_at FROM prospective_memories WHERE "
        "contact_id=? AND fired=0 AND trigger_type='keyword' AND "
        "(expires_at IS NULL OR expires_at=0 OR expires_at>?) ORDER BY created_at DESC, id DESC",
        (cid, now_s)).fetchall()
    by_action, order = {}, []
    for rid, kw, action, _ in rows:
        if action not in by_action:
            by_action[action] = {"action": action, "rows": []}
            order.append(action)
        by_action[action]["rows"].append((rid, kw))
    out = []
    for action in order[:MAX_THREADS]:
        rows_sorted = sorted(by_action[action]["rows"])  # by id: the order they were written
        out.append({"action": action, "cues": [kw for _, kw in rows_sorted],
                    "ids": [rid for rid, _ in rows_sorted]})
    return out


def register_line(cid, c):
    parts = []
    if c.get("greeting_style"):
        parts.append(c["greeting_style"])
    if c.get("prefers_short_texts"):
        parts.append("short texts")
    if c.get("uses_emoji"):
        parts.append("emoji ok")
    if c.get("texts_in_bursts"):
        parts.append("texts in bursts")
    if c.get("warmth_level"):
        parts.append(f"warmth {c['warmth_level']}")
    if not parts:
        return None
    return f"- {'; '.join(parts)} [persona:contacts.{cid}]"


def build_contact_page(cid, c, insights, threads, compiled):
    name = c.get("name") or cid
    rel = c.get("relationship") or c.get("relationship_type") or ""
    head = f"# {name}" + (f" ({rel})" if rel else "") + f" — compiled {compiled}"
    now = []
    if c.get("identity"):
        now.append(f"- {first_sentence(c['identity'])} [persona:contacts.{cid}.identity]")
    open_threads = [f"- {t['action']} (cue: {', '.join(t['cues'])}) "
                    f"[pm:{','.join(str(i) for i in t['ids'])}]" for t in threads]
    remember = [f"- {n['insight']} ({month(n['as_of'])}) [ins:{n['id']}]" for n in insights]
    reg = register_line(cid, c)
    return assemble(head, now, open_threads, remember, [reg] if reg else [])


def build_self_page(persona, insights, threads, compiled):
    core = persona.get("core") or {}
    head = f"# {persona.get('name') or 'self'} (self) — compiled {compiled}"
    now = []
    if core.get("identity"):
        now.append(f"- {first_sentence(core['identity'])} [persona:core.identity]")
    for i, ev in enumerate(persona.get("life_events") or []):
        if ev.get("state") == "in_progress" and ev.get("description"):
            now.append(f"- {ev['description']} (as of {ev.get('as_of', '?')}) "
                       f"[persona:life_events.{i}]")
    open_threads = [f"- {t['action']} (cue: {', '.join(t['cues'])}) "
                    f"[pm:{','.join(str(i) for i in t['ids'])}]" for t in threads]
    remember = [f"- {n['insight']} ({month(n['as_of'])}) [ins:{n['id']}]" for n in insights]
    reg = [f"- {r} [persona:style_rules.{i}]"
           for i, r in enumerate((persona.get("style_rules") or [])[:3])]
    return assemble(head, now, open_threads, remember, reg)


def assemble(head, now, threads, remember, register):
    def render(rem):
        parts = [head]
        for title, lines in (("now", now), ("open threads", threads), ("what I remember", rem),
                             ("register", register)):
            if lines:
                parts.append(f"## {title}")
                parts.extend(lines)
        return "\n".join(parts) + "\n"
    # trim the oldest memories first until the page fits its budget
    rem = list(remember)
    text = render(rem)
    while len(text.encode("utf-8")) > MAX_BYTES and rem:
        rem.pop()
        text = render(rem)
    return text


def lint_page(text, max_bytes=MAX_BYTES, must_contain=None):
    problems = []
    size = len(text.encode("utf-8"))
    if size > max_bytes:
        problems.append(f"{size} bytes > {max_bytes}")
    for ln in text.splitlines():
        if not ln.strip() or ln.startswith("#"):
            continue
        if not ln.startswith("- ") or not TAG.search(ln):
            problems.append(f"line without provenance: {ln[:60]!r}")
    if must_contain and must_contain not in text:
        problems.append(f"missing required text: {must_contain[:60]!r}")
    return problems


def build_all(db, persona, targets, now_s=None):
    now_s = int(now_s if now_s is not None else time.time())
    compiled = time.strftime("%Y-%m-%d", time.gmtime(now_s))
    contacts = contacts_from(persona)
    pages, problems = {}, {}
    for cid in targets:
        if cid == "self":
            text = build_self_page(persona, insights_for(db, "self"), threads_for(db, "self", now_s),
                                   compiled)
            core = (persona.get("core") or {}).get("identity")
            probs = lint_page(text, must_contain=first_sentence(core) if core else None)
        elif cid in contacts:
            if not safe_id(cid):
                print(f"{cid}: not a safe file name, skipped", file=sys.stderr)
                continue
            text = build_contact_page(cid, contacts[cid], insights_for(db, cid),
                                      threads_for(db, cid, now_s), compiled)
            probs = lint_page(text)
        else:
            continue
        pages[f"{cid}.md"] = text
        if probs:
            problems[f"{cid}.md"] = probs
    return pages, problems


def write_pages(pages, out_dir):
    os.makedirs(out_dir, mode=0o700, exist_ok=True)
    for fname, text in pages.items():
        tmp = os.path.join(out_dir, f".{fname}.tmp")
        with open(tmp, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(tmp, os.path.join(out_dir, fname))


def run(db, persona, out_dir, write, contact=None, now_s=None):
    targets = [contact] if contact else ["self"] + list(contacts_from(persona))
    pages, problems = build_all(db, persona, targets, now_s)
    for fname, text in pages.items():
        size = len(text.encode("utf-8"))
        verdict = "FAIL " + "; ".join(problems[fname]) if fname in problems else "ok"
        print(f"{fname}: {size} bytes, {text.count(chr(10)) - 1} lines, lint {verdict}")
        if not write:
            print(text)
    if problems:
        print(f"lint failed on {len(problems)} page(s); nothing written", file=sys.stderr)
        return 1
    if not pages:
        print("no pages to write", file=sys.stderr)
        return 1
    if write:
        write_pages(pages, out_dir)
        print(f"wrote {len(pages)} pages to {out_dir}")
    return 0


def main():
    global MAX_BYTES
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--write", action="store_true", help="publish pages (default: dry-run)")
    ap.add_argument("--out", default=OUT_DIR)
    ap.add_argument("--contact", help="one contact id (or 'self')")
    ap.add_argument("--max-bytes", type=int, default=MAX_BYTES)
    a = ap.parse_args()
    MAX_BYTES = a.max_bytes
    persona = json.load(open(PERSONA))
    db = sqlite3.connect(f"file:{MEMORY_DB}?mode=ro", uri=True)
    return run(db, persona, a.out, a.write, a.contact)


if __name__ == "__main__":
    sys.exit(main())
