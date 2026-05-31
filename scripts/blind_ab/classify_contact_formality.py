#!/usr/bin/env python3
"""B1 data calibration: classify each iMessage contact as "professional" or
"casual" from how THEY write to you (formal contacts write formally — mirror
them). Output merges into the persona's per-contact formality, which the
(now formality-aware) reactive reply path reads to pick register.

Privacy: 100% local — reads ~/Library/Messages/chat.db, writes a JSON file.
Contacts aliased by default. Nothing leaves the machine.

Usage:
  python3 classify_contact_formality.py                 # -> contact_formality.json
  python3 classify_contact_formality.py --keep-handles  # real handles (for merging into persona)
  python3 classify_contact_formality.py --min-msgs 5 --threshold 0.55
"""
import argparse, json, os, re, sqlite3, sys

# Reuse the attributedBody decoder from the sibling exporter.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
try:
    from export_seth_triples import msg_text  # noqa
except Exception:
    def msg_text(text, body):
        return (text or "").strip() or None

DEFAULT_DB = os.path.expanduser("~/Library/Messages/chat.db")
SLANG = re.compile(r"\b(lol|lmao|nah|yeah|ya|u|ur|gonna|wanna|lemme|dunno|kinda|"
                   r"sorta|tbh|idk|omg|btw)\b", re.I)
PRO_MARKERS = re.compile(r"\b(following up|follow up|schedule|onboarding|regards|"
                         r"sincerely|best,|thanks,|per my|reach out|touch base|"
                         r"linkedin|opportunity|invoice|deposit|contract|meeting|"
                         r"please|kindly|appreciate)\b", re.I)


def formal_score(msgs, is_email):
    """0..1 — higher = more formal/professional. Heuristic over a contact's
    inbound messages."""
    if not msgs:
        return 0.5
    cap_starts = sum(1 for m in msgs if m[:1].isupper()) / len(msgs)
    slang_hits = sum(len(SLANG.findall(m)) for m in msgs)
    words = sum(max(1, len(m.split())) for m in msgs)
    slang_rate = min(1.0, slang_hits / max(1, words) * 12)  # scaled
    pro_rate = min(1.0, sum(1 for m in msgs if PRO_MARKERS.search(m)) / len(msgs) * 2)
    score = 0.45 * cap_starts + 0.30 * (1 - slang_rate) + 0.20 * pro_rate
    if is_email:
        score = min(1.0, score + 0.10)  # email handle ~ professional
    return score


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--db", default=DEFAULT_DB)
    ap.add_argument("--min-msgs", type=int, default=4, help="skip contacts with fewer inbound msgs")
    ap.add_argument("--threshold", type=float, default=0.55, help=">= this => professional")
    ap.add_argument("--out", default="contact_formality.json")
    ap.add_argument("--keep-handles", action="store_true")
    a = ap.parse_args()

    if not os.path.exists(a.db):
        sys.exit(f"DB not found: {a.db}")
    con = sqlite3.connect(f"file:{a.db}?mode=ro", uri=True)
    rows = con.execute("""
        SELECT h.id, m.text, m.attributedBody
        FROM message m JOIN handle h ON h.ROWID = m.handle_id
        WHERE m.is_from_me = 0 AND m.associated_message_type = 0 AND m.item_type = 0
    """).fetchall()
    con.close()

    by_handle = {}
    for handle, text, body in rows:
        t = msg_text(text, body)
        if t:
            by_handle.setdefault(handle or "unknown", []).append(t)

    alias, out = {}, {}
    pro = cas = 0
    for handle, msgs in by_handle.items():
        if len(msgs) < a.min_msgs:
            continue
        is_email = "@" in (handle or "")
        s = formal_score(msgs, is_email)
        label = "professional" if s >= a.threshold else "casual"
        if label == "professional":
            pro += 1
        else:
            cas += 1
        key = handle if a.keep_handles else alias.setdefault(handle, f"contact_{len(alias)+1}")
        out[key] = {"formality": label, "score": round(s, 2), "n_msgs": len(msgs)}

    with open(a.out, "w") as f:
        json.dump(out, f, indent=2)
    print(f"classified {len(out)} contacts -> {a.out}  (professional={pro} casual={cas})")
    print("Merge the 'formality' values into your persona's per-contact overlays; the "
          "reactive reply path now renders that register (formality-aware absolute rules).")


if __name__ == "__main__":
    main()
