"""Evidence rows for the sleep-time curator (spec §4-5).

Only the contact's messages and Seth's own sends are citable ([tN]). Daemon
sends and ambiguous ones are shown for context as [dN] and can never be
evidence: otherwise a detail the model confabulated in a reply becomes a
"memory" overnight and is repeated confidently (HaluMem, arXiv 2511.03506).
"""
import re


def chat_turn_rows(msgs, labels, n, cutoff):
    if n <= 0:
        return []
    rows = []
    for msg in sorted(msgs, key=lambda m: m["t"]):
        if msg["t"] < cutoff:
            continue
        if msg["from_me"]:
            who = "me" if labels.get(msg["guid"]) == "seth" else "daemon"
        else:
            who = "them"
        text = (msg.get("text") or "").strip().replace("\n", " ")[:300]
        if not text or not re.search(r"\w", text):
            text = "[attachment]"
        rows.append((int(msg["rowid"]), int(msg["t"].timestamp() * 1000), who, text))
    return rows[-n:]


def number_rows(rows):
    lines, cite, t, d = [], {}, 0, 0
    for row in rows:
        _, _, who, text = row
        if who == "daemon":
            lines.append(f"[d{d}] me: {text}")
            d += 1
        else:
            lines.append(f"[t{t}] {who}: {text}")
            cite[t] = row
            t += 1
    return lines, cite


def parse_evidence(tokens):
    t_idx, daemon = [], False
    for tok in tokens or []:
        s = str(tok).strip().strip("[]").lower()
        # Any token starting with "d" is deliberately treated as a daemon
        # citation: fails closed by dropping the note (see validate_note),
        # rather than risking a daemon-confabulated detail read as evidence.
        if s.startswith("d"):
            daemon = True
            continue
        mt = re.fullmatch(r"t?(\d+)", s)
        if mt:
            t_idx.append(int(mt.group(1)))
    return t_idx, daemon


def name_said(name, texts):
    """Verbatim, case-insensitive, word-bounded: 'Al' is not in 'Also',
    'Priya' is in "priya's". Boundaries are Unicode-aware (\\w, not
    [A-Za-z0-9]) so accented names aren't falsely split out of (or into)
    adjacent text, e.g. 'Al' must not match inside 'caféAl'."""
    name = (name or "").strip()
    if not name:
        return False
    pat = re.compile(r"(?<!\w)" + re.escape(name) + r"(?!\w)", re.I)
    return any(pat.search(t or "") for t in texts)


def validate_note(note, cite_map):
    t_idx, daemon = parse_evidence(note.get("evidence_tokens"))
    if daemon:
        return None, "daemon_evidence"
    rows = [cite_map[i] for i in t_idx if i in cite_map]
    if not rows:
        return None, "no_evidence"
    texts = [r[3] for r in rows]
    for n in note.get("names") or []:
        if not name_said(n.get("name"), texts):
            return None, "name_not_said"
    return {**note, "evidence_rows": rows}, "ok"
