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


WORD_RE = re.compile(r"[^\W_]+(?:['’][^\W_]+)*")


def implicit_names(text):
    """Capitalized words after the first are names the model may not have
    declared: the wide prompt asks for lowercase except proper nouns. A
    trailing possessive ('s / ’s) is stripped; "I" and its contractions
    (I'm, I'll) are not names. Unicode-aware (str.isupper, [^\\W_])."""
    out = []
    for tok in WORD_RE.findall(text or "")[1:]:
        tok = re.sub(r"['’]s$", "", tok, flags=re.I)
        if not tok[:1].isupper() or re.split(r"['’]", tok)[0] == "I":
            continue
        out.append(tok)
    return out


def names_to_check(note):
    """Declared names plus implicit capitalized tokens, deduplicated
    case-insensitively, declared spellings first."""
    seen, out = set(), []
    declared = [str(n.get("name") or "") for n in note.get("names") or []]
    for name in declared + implicit_names(note.get("note")):
        if name.strip() and name.lower() not in seen:
            seen.add(name.lower())
            out.append(name)
    return out


def assess_note(note, cite_map):
    """-> (validated note | None, reason, names checked, names not said).
    Names are only checked once the evidence itself is valid."""
    t_idx, daemon = parse_evidence(note.get("evidence_tokens"))
    if daemon:
        return None, "daemon_evidence", [], []
    rows = [cite_map[i] for i in t_idx if i in cite_map]
    if not rows:
        return None, "no_evidence", [], []
    texts = [r[3] for r in rows]
    checked = names_to_check(note)
    unsaid = [n for n in checked if not name_said(n, texts)]
    if unsaid:
        return None, "name_not_said", checked, unsaid
    return {**note, "evidence_rows": rows}, "ok", checked, []


def validate_note(note, cite_map):
    out, reason, _, _ = assess_note(note, cite_map)
    return out, reason
