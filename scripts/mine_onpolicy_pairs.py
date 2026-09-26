#!/usr/bin/env python3
"""Mine on-policy preference pairs from real iMessage threads: moments where
h-uman replied as Seth AND Seth replied himself to the same inbound message.
chosen = Seth's real reply, rejected = h-uman's reply, prompt = the thread so
far rendered exactly like the existing preference corpus.

Why (2026-09-26): the depth-upweighted SFT candidate scored 0.565 on the
authorship twin vs 0.597 for the serving adapter (floor 0.627, strangers 0.64):
imitating Seth's average text keeps flattening him. The strongest signal left
is where h-uman and Seth answered the SAME moment — e.g. one contact got
"Nah" / "Aight" from h-uman and "Love the curls" from Seth on the same day.
Send provenance (outbound_sends) now tells us exactly which reply was whose.

A pair is only minted when both replies answer the same inbound: the run of
from-me messages between one inbound and the next contains at least one
"seth" and at least one "huuman" send and no "ambiguous" one. Anything looser
recreates the sliding-window flaw that got the old auto_correction /
outbound_edit pairs rejected (build_v6_preference_corpus.py).

Also counts "gold" moments: runs answered only by Seth. Those can become pairs
later by asking the production path what h-uman would have said there
(rejected = a fresh policy sample), which is the larger source; this script
only counts them.

Dry run by default: prints counts per contact, never message text. --write
PATH writes {"prompt","chosen","rejected"} JSONL (the shape mlx_tune_train.py
and mlx_lm_lora ORPO read) plus PATH.manifest.json (counts only).
"""
import argparse
import datetime as dt
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import eval_conversation_quality as cq  # noqa: E402
from merge_seth_preference_sources import render_prompt  # noqa: E402

CONTEXT_TURNS = 6            # extract_imessage_pairs.py uses up to 6 context turns
CONTEXT_GAP_S = 3600         # ... and 1h window gaps
OBJ = "￼"               # attachment placeholder in chat.db text


def _clean(text):
    return (text or "").replace(OBJ, "").strip()


def _context(timeline, start):
    """Up to CONTEXT_TURNS messages before timeline[start], stopping at a gap
    of more than CONTEXT_GAP_S, as render_prompt() turns."""
    turns, prev_t = [], timeline[start]["t"]
    i = start - 1
    while i >= 0 and len(turns) < CONTEXT_TURNS:
        m = timeline[i]
        if (prev_t - m["t"]).total_seconds() > CONTEXT_GAP_S:
            break
        text = _clean(m["text"])
        if text:
            turns.append({"role": "assistant" if m["from_me"] else "user", "content": text})
        prev_t = m["t"]
        i -= 1
    turns.reverse()
    return turns


def iter_moments(timeline, labels):
    """One record per run of from-me messages that answers an inbound:
    {"kind": "pair"|"gold"|"huuman"|"ambiguous", "turns": context for
    render_prompt, "seth": [texts], "huuman": [texts], "t": first send time,
    "seth_after_huuman": bool}. `labels` maps guid -> "seth" | "huuman" |
    "ambiguous" for from-me sends. "pair" means both answered the same
    inbound; "gold" only Seth; "huuman" only h-uman."""
    i, n = 0, len(timeline)
    while i < n:
        if timeline[i]["from_me"] or i + 1 >= n or not timeline[i + 1]["from_me"]:
            i += 1
            continue
        # timeline[i] is the last inbound before a run of from-me sends.
        start = i + 1
        end = start
        while end < n and timeline[end]["from_me"]:
            end += 1
        run = timeline[start:end]
        i = end
        kinds = [labels.get(m["guid"], "ambiguous") for m in run]
        if "ambiguous" in kinds:
            yield {"kind": "ambiguous"}
            continue
        seth = [t for t in (_clean(m["text"]) for m, k in zip(run, kinds) if k == "seth") if t]
        huuman = [t for t in (_clean(m["text"]) for m, k in zip(run, kinds) if k == "huuman") if t]
        if not seth and not huuman:
            continue
        kind = "pair" if seth and huuman else ("gold" if seth else "huuman")
        after = False
        if kind == "pair":
            first_h = kinds.index("huuman")
            after = "seth" in kinds[first_h:]
        yield {"kind": kind, "turns": _context(timeline, start), "seth": seth,
               "huuman": huuman, "t": run[0]["t"], "seth_after_huuman": after}


def mine_thread(timeline, labels):
    """Pairs and counts for one contact's 1:1 timeline (reactions removed)."""
    pairs, counts = [], {"natural_pairs": 0, "seth_after_huuman": 0,
                         "gold_moments": 0, "huuman_only": 0, "skipped_ambiguous": 0}
    for m in iter_moments(timeline, labels):
        if m["kind"] == "ambiguous":
            counts["skipped_ambiguous"] += 1
        elif m["kind"] == "gold":
            counts["gold_moments"] += 1
        elif m["kind"] == "huuman":
            counts["huuman_only"] += 1
        else:
            chosen, rejected = "\n".join(m["seth"]), "\n".join(m["huuman"])
            prompt = render_prompt(m["turns"])
            if prompt and chosen != rejected:
                pairs.append({"prompt": prompt, "chosen": chosen, "rejected": rejected})
                counts["natural_pairs"] += 1
                if m["seth_after_huuman"]:
                    counts["seth_after_huuman"] += 1
    return pairs, counts


def mine(chat_path, mem_path, since, names):
    att = cq.attribute(chat_path, mem_path, since)
    all_pairs, per_contact = [], {}
    for contact, timeline in att["timelines"].items():
        pairs, counts = mine_thread(timeline, att["labels"])
        if any(counts.values()):
            per_contact[names.get(contact, "(not in persona)")] = counts
        all_pairs.extend(pairs)
    seen, unique = set(), []
    for p in all_pairs:
        key = (p["prompt"], p["chosen"], p["rejected"])
        if key not in seen:
            seen.add(key)
            unique.append(p)
    return unique, per_contact, att["exact_from"]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--persona", default=os.path.expanduser("~/.human/personas/seth.json"))
    ap.add_argument("--chat-db", default=os.path.expanduser("~/Library/Messages/chat.db"))
    ap.add_argument("--memory-db", default=os.path.expanduser("~/.human/memory.db"))
    ap.add_argument("--days", type=int, default=60)
    ap.add_argument("--write", metavar="PATH", help="write pairs JSONL here (plus .manifest.json)")
    a = ap.parse_args(argv)

    names = {}
    try:
        with open(a.persona) as f:
            names = {k: c.get("name", "?") for k, c in json.load(f).get("contacts", {}).items()}
    except (OSError, ValueError):
        pass
    since = dt.datetime.now(dt.timezone.utc) - dt.timedelta(days=a.days)
    pairs, per_contact, exact_from = mine(a.chat_db, a.memory_db, since, names)

    totals = {k: sum(c[k] for c in per_contact.values())
              for k in ("natural_pairs", "seth_after_huuman", "gold_moments",
                        "huuman_only", "skipped_ambiguous")}
    for name, c in sorted(per_contact.items(), key=lambda kv: -kv[1]["gold_moments"]):
        print(f"{name:28} pairs={c['natural_pairs']:3}  gold={c['gold_moments']:4}  "
              f"huuman_only={c['huuman_only']:3}  ambiguous={c['skipped_ambiguous']:3}")
    print(f"TOTAL natural pairs={len(pairs)} (Seth after h-uman: {totals['seth_after_huuman']}), "
          f"gold moments={totals['gold_moments']}, h-uman-only={totals['huuman_only']}, "
          f"skipped ambiguous={totals['skipped_ambiguous']}; window {a.days}d; "
          f"exact provenance from {exact_from.isoformat() if exact_from else 'n/a'}")

    if a.write:
        if not pairs:
            print("REFUSE — no natural pairs found; nothing written")
            return 2
        os.makedirs(os.path.dirname(os.path.abspath(a.write)), exist_ok=True)
        tmp = a.write + ".tmp"
        with open(tmp, "w") as f:
            for p in pairs:
                f.write(json.dumps(p, ensure_ascii=False) + "\n")
        os.replace(tmp, a.write)
        with open(a.write + ".manifest.json", "w") as f:
            json.dump({"generated_at": dt.datetime.now(dt.timezone.utc).isoformat(),
                       "window_days": a.days, "pairs": len(pairs), "totals": totals,
                       "per_contact": per_contact,
                       "exact_from": exact_from.isoformat() if exact_from else None}, f, indent=2)
        print(f"wrote {len(pairs)} pairs -> {a.write}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
