"""Evidence rows for the sleep-time curator (spec §4-5).

Only the contact's messages and Seth's own sends are citable ([tN]). Daemon
sends and ambiguous ones are shown for context as [dN] and can never be
evidence: otherwise a detail the model confabulated in a reply becomes a
"memory" overnight and is repeated confidently (HaluMem, arXiv 2511.03506).
"""
import re


def chat_turn_rows(msgs, labels, n, cutoff):
    rows = []
    for msg in msgs:
        if msg["t"] < cutoff:
            continue
        if msg["from_me"]:
            who = "me" if labels.get(msg["guid"]) == "seth" else "daemon"
        else:
            who = "them"
        text = (msg.get("text") or "").strip().replace("\n", " ")[:300] or "[attachment]"
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
        if s.startswith("d"):
            daemon = True
            continue
        mt = re.fullmatch(r"t?(\d+)", s)
        if mt:
            t_idx.append(int(mt.group(1)))
    return t_idx, daemon
