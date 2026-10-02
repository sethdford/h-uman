#!/usr/bin/env python3
"""Build the SFT corpus whose targets are Seth's WHOLE replies.

The 2026-09-19 corpus (seth-sft-20260919) took one bubble per target, so a
reply like "Excellent!" + "We gonna hang out soon?" became two examples and
the model never saw a reply that carries a second thought. Half of Seth's
replies do; 35% of the twin's (census 2026-10-02), and a prompt rule moved
that only 28% -> 34%. This builder reads chat.db, merges consecutive bubbles
of one speaker into a turn (extract_imessage_pairs.collapse_turns), and
writes {prompt, completion} rows in the same format as the old corpus.

Kept out: group chats, the owner's own handles, exclude_from contacts, and
the daemon's sends as targets (they stay in context, labeled "Seth:", which
is how the contact saw them).

    python3 scripts/build_sft_turn_corpus.py --out ~/.human/training-data/seth-sft-turns-20261002
"""
import argparse
import hashlib
import json
import os
import statistics
import sys
import time

sys.path.insert(0, os.path.dirname(__file__))

import extract_imessage_pairs as e  # noqa: E402

CONTEXT_TURNS = 5
MAX_COMPLETION_CHARS = 800
BASE_CORPUS = os.path.expanduser("~/.human/training-data/seth-sft-20260919")
TEMPLATE_CONFIG = os.path.join(BASE_CORPUS, "config.yaml")


def format_prompt(context):
    """Same shape as seth-sft-20260919: one incoming turn is bare text,
    longer histories are "Seth:"/"Them:" lines."""
    if len(context) == 1 and context[0]["speaker"] == "them":
        return context[0]["text"]
    return "\n".join(("Them: " if c["speaker"] == "them" else "Seth: ") + c["text"] for c in context)


def turn_rows(window):
    """{prompt, completion} rows for one conversation window. A row ends on a
    turn Seth wrote that answers an incoming turn."""
    turns = [{"speaker": e._speaker(t), "text": t["text"]} for t in e.collapse_turns(window)]
    rows = []
    for i, t in enumerate(turns):
        if t["speaker"] != "seth" or i == 0 or turns[i - 1]["speaker"] != "them":
            continue
        if not (e.MIN_REPLY_LENGTH <= len(t["text"]) <= MAX_COMPLETION_CHARS):
            continue
        rows.append({"prompt": format_prompt(turns[max(0, i - CONTEXT_TURNS):i]),
                     "completion": t["text"]})
    return rows


def _line_text(line):
    for tag in ("Them: ", "Seth: "):
        if line.startswith(tag):
            return line[len(tag):]
    return line


def _split_followup(prompt):
    """(incoming text, [Seth's earlier bubbles]) when a prompt ends with
    Seth's own bubbles after an incoming line, else (incoming text, [])."""
    lines = prompt.split("\n")
    bubbles = []
    while lines and lines[-1].startswith("Seth: "):
        bubbles.insert(0, lines.pop()[len("Seth: "):])
    return (_line_text(lines[-1]) if lines else None), bubbles


def merge_followups(rows):
    """Fold the old corpus's follow-up rows (prompt ends with Seth's earlier
    bubbles) into the row holding the first bubble, so the target is the whole
    reply. Returns (rows, number of follow-ups with no first-bubble row)."""
    heads, out, followups = {}, [], []
    for r in rows:
        incoming, bubbles = _split_followup(r["prompt"])
        if bubbles:
            followups.append((len(bubbles), incoming, bubbles, r["completion"]))
            continue
        row = dict(r)
        heads.setdefault((incoming, r["completion"]), row)
        out.append(row)
    dropped = 0
    for _, incoming, bubbles, completion in sorted(followups, key=lambda f: f[0]):
        head = heads.get((incoming, bubbles[0]))
        if head is None or head["completion"] != "\n".join(bubbles):
            dropped += 1
            continue
        head["completion"] += "\n" + completion
    return out, dropped


def _reply_key(row):
    return _split_followup(row["prompt"])[0], row["completion"].split("\n")[0]


def union_rows(fresh, old):
    """Fresh chat.db rows plus the old rows they do not already cover (same
    incoming line, same first bubble); the fresh copy has real timestamps."""
    covered = {_reply_key(r) for r in fresh}
    return fresh + [r for r in old if _reply_key(r) not in covered]


def keep_chat(chat_id, skip):
    """1:1 chats only (group identifiers start with "chat"), never the owner's
    own handles or an excluded contact."""
    return bool(chat_id) and not chat_id.startswith("chat") and chat_id not in skip


def is_valid_row(row):
    """Frozen by content, as in seth-sft-20260919: ~5% held out."""
    return int(hashlib.sha256((row["prompt"] + row["completion"]).encode()).hexdigest(), 16) % 20 == 0


def build(days):
    from ab_agent_turns import excluded_handles, owner_handles

    skip = owner_handles() | excluded_handles()
    messages = e.extract_messages(e.DB_PATH)
    cutoff = time.time() - days * 86400
    messages = [m for m in messages if m["timestamp"] >= cutoff]
    daemon = e.mark_daemon_sends(messages, e.load_daemon_records())
    rows, seen = [], set()
    for chat_id, msgs in e.group_by_chat(messages).items():
        if not keep_chat(chat_id, skip):
            continue
        for window in e.build_conversation_windows(msgs):
            for r in turn_rows(window):
                key = (r["prompt"], r["completion"])
                if key not in seen:
                    seen.add(key)
                    rows.append(r)
    return rows, daemon


def load_base(base):
    rows = []
    for name in ("train.jsonl", "valid.jsonl"):
        with open(os.path.join(base, name)) as f:
            rows += [json.loads(line) for line in f if line.strip()]
    return rows


def write_corpus(out, rows, daemon, days, provenance):
    os.makedirs(out, exist_ok=True)
    train = [r for r in rows if not is_valid_row(r)]
    valid = [r for r in rows if is_valid_row(r)]
    for name, part in (("train.jsonl", train), ("valid.jsonl", valid)):
        with open(os.path.join(out, name), "w") as f:
            for r in part:
                f.write(json.dumps(r, ensure_ascii=False) + "\n")
    multi = sum("\n" in r["completion"] for r in rows)
    manifest = {
        "source": e.DB_PATH,
        "rule": ("1:1 chats, last %d days; Seth's whole turn (bubbles <= %d s apart) answering an "
                 "incoming turn, up to %d turns of context; daemon sends never targets; "
                 "dedup on (prompt,completion); valid = sha256(prompt+completion)%%20==0"
                 % (days, e.TURN_BUBBLE_GAP_S, CONTEXT_TURNS)),
        "n_rows": len(rows), "n_train": len(train), "n_valid": len(valid),
        "multi_bubble_completion_rate": round(multi / len(rows), 3) if rows else 0.0,
        "median_completion_chars": statistics.median(len(r["completion"]) for r in rows) if rows else 0,
        "daemon_sends_excluded": daemon,
        **provenance,
        "built": time.strftime("%Y-%m-%d"),
        "purpose": "SFT candidate: whole-reply targets (nightly candidate stage, trainer=mlx_lm)",
    }
    with open(os.path.join(out, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    cfg = open(TEMPLATE_CONFIG).read().splitlines()
    cfg = [("data: " + out) if line.startswith("data:") else line for line in cfg]
    cfg.insert(0, "# Whole-reply SFT corpus (build_sft_turn_corpus.py); otherwise the 2026-09-19 recipe.")
    with open(os.path.join(out, "config.yaml"), "w") as f:
        f.write("\n".join(cfg) + "\n")
    return manifest


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--days", type=int, default=730)
    ap.add_argument("--base", default=BASE_CORPUS,
                    help="older single-bubble corpus to repair and merge in ('' for chat.db only)")
    args = ap.parse_args()
    out = os.path.expanduser(args.out)
    fresh, daemon = build(args.days)
    provenance = {"n_chatdb_rows": len(fresh)}
    rows = fresh
    if args.base:
        old = load_base(os.path.expanduser(args.base))
        repaired, dropped = merge_followups(old)
        rows = union_rows(fresh, repaired)
        provenance.update({"base": args.base, "n_base_rows": len(old),
                           "n_base_followups_folded": len(old) - len(repaired) - dropped,
                           "n_base_followups_unmatched_dropped": dropped,
                           "n_base_rows_kept": len(rows) - len(fresh)})
    if not fresh or not rows:
        raise SystemExit("no chat.db rows: refusing to write a corpus that did not read chat.db")
    print(json.dumps(write_corpus(out, rows, daemon, args.days, provenance), indent=1))


if __name__ == "__main__":
    main()
