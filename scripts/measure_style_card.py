#!/usr/bin/env python3
"""measure_style_card.py — derive the persona's style card from a window of
the user's own outbound iMessages and write it where the daemon reads it.

Why this exists (2026-09-03): the persona's style numbers contradicted each
other AND the measurement — lowercase-start was 4% in a C comment, 17.3% in
the old style card, 8.6% measured; emoji was "1 in 8" in seth.json, 9% in
the card, 12.6% measured. The July 2026 deliberation leak was traced to
exactly this shape: three prompt layers asserting different numbers for
the same axis, and the model agonizing over the conflict on every turn.

This script makes the card the single source. The C prompt builder
(src/persona/style_card.c) renders the casual register's rule 2 from
~/.human/personas/<persona>.style-card.json; a compiled default is only a
fallback when the card is missing (and it logs that it fell back).

Axes are computed by scripts/eval_persona_evolution.py's per-message
feature functions, so the card and the gap-analysis eval can never
disagree on a definition. Same read-only chat.db contract as that script:
mode=ro + immutable=1, no message text ever leaves this process.

Refusal contract (.claude/rules/no-number-without-a-measurement.md): if the
window has fewer than --min-n messages (default 300) the script exits
non-zero and writes NOTHING. A stale card is recoverable; a card derived
from 40 messages is not distinguishable from a real one downstream.

Usage:
    python3 scripts/measure_style_card.py                  # last 60 days, seth
    python3 scripts/measure_style_card.py --days 90 --persona seth
    python3 scripts/measure_style_card.py --dry-run        # print, don't write

Supersedes the card-writing half of scripts/persona_style_card.py (whose
v1 output shape nothing in C ever read).
"""
import argparse
import datetime
import json
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from eval_persona_evolution import (  # noqa: E402
    DEFAULT_DB,
    aggregate_window,
    fetch_outbound_messages,
)
from reply_pairs import fetch_reply_pairs, is_substantive, reply_stats  # noqa: E402

# Judge-free pair axis (2026-09-13): how the user answers a LONG or
# question-bearing inbound. The multi-turn nightly's substantive scenarios
# (debate, news reaction, advice) end judged "AI" while the casual ones hold;
# the card so far only described single messages. Below SUBSTANTIVE_MIN_N the
# axis is written with its n and nothing renders from it.
SUBSTANTIVE_MIN_N = 20
DEFAULT_SUBSTANTIVE_DAYS = 120

SCHEMA = "style-card/v2"
DEFAULT_DAYS = 60
DEFAULT_MIN_N = 300
DEFAULT_PERSONA = "seth"

# Axes the C renderer consumes (include/human/persona/style_card.h) plus
# length for the record. Names are eval_persona_evolution.AXES labels.
CARD_AXES = (
    "lowercase_start_rate",
    "no_terminal_punct_rate",
    "question_rate",
    "exclamation_rate",
    "emoji_rate",
    "dash_rate",
    "laugh_rate",
    "length_chars",
)


def owner_handles(persona: str):
    """The owner's own handles (persona contacts with relationship "test"):
    texts to them are self-tests and notes, not how the owner texts people
    (178 in 60 days, 57% lowercase starts against 10% to others, 2026-09-30)."""
    path = os.path.expanduser(f"~/.human/personas/{persona}.json")
    try:
        contacts = json.load(open(path)).get("contacts") or {}
    except (OSError, ValueError):
        return set()
    return {h for h, c in contacts.items() if isinstance(c, dict) and c.get("relationship") == "test"}


def drop_daemon_sends(messages, records):
    """[(datetime, text)] minus the daemon's own sends. chat.db marks those
    is_from_me too; until 2026-09-30 the card measured them as Seth (~12% of
    from-me rows), so the twin's habits leaked back into its own style rules."""
    from extract_imessage_pairs import daemon_send_predicate
    is_daemon = daemon_send_predicate(records)

    def epoch(ts):  # fetch_outbound_messages yields NAIVE UTC (Apple epoch)
        return (ts if ts.tzinfo else ts.replace(tzinfo=datetime.timezone.utc)).timestamp()

    return [(ts, t) for ts, t in messages if not is_daemon(t, epoch(ts))]


class InsufficientData(Exception):
    """Raised instead of producing a card from too few messages."""


def default_card_path(persona: str) -> str:
    base = os.environ.get("HU_PERSONA_DIR") or os.path.expanduser("~/.human/personas")
    return os.path.join(base, f"{persona}.style-card.json")


def build_card(messages, persona: str, window_start: datetime.datetime,
               window_end: datetime.datetime, min_n: int = DEFAULT_MIN_N,
               n_resamples: int = 2000, seed: int = 42) -> dict:
    """messages: iterable of (datetime, text). Pure — no I/O.

    Keeps only messages with window_start <= ts < window_end, refuses below
    min_n, and returns the card dict. Every axis carries value + 95% CI + n
    so a reader can tell a tight estimate from a noisy one.
    """
    texts = [t for ts, t in messages if window_start <= ts < window_end]
    n = len(texts)
    if n < min_n:
        raise InsufficientData(
            f"window {window_start.date()}..{window_end.date()} has n={n} < min_n={min_n}"
        )
    agg = aggregate_window(texts, n_resamples=n_resamples, seed=seed)
    axes = {}
    for name in CARD_AXES:
        a = agg["axes"][name]
        axes[name] = {
            "value": a["mean"],
            "ci_lo": a["ci_lo"],
            "ci_hi": a["ci_hi"],
            "n": a["n"],
        }
    return {
        "schema": SCHEMA,
        "persona": persona,
        "source": "scripts/measure_style_card.py",
        "generated_at": datetime.datetime.now().replace(microsecond=0).isoformat(),
        "window": {
            "start": window_start.date().isoformat(),
            "end": window_end.date().isoformat(),
            "days": (window_end - window_start).days,
        },
        "n": n,
        "min_n": min_n,
        "confidence": 0.95,
        "axes": axes,
    }


# Entity-casing axis (2026-09-22). Feeds the style governor's action D. The
# entity vocabulary is imported from specificity_score so the governor's
# allowlist and the specificity GATE can never disagree on what an entity is
# — same discipline as the axes above reusing eval_persona_evolution.
ENTITY_MIN_MENTIONS = 3          # mirrors HU_STYLE_CARD_ENTITY_MIN_MENTIONS
ENTITY_MAX_TOKENS = 64           # mirrors HU_STYLE_CARD_MAX_ENTITY_TOKENS
ENTITY_TOKEN_MAX_LEN = 23        # mirrors HU_STYLE_CARD_ENTITY_TOKEN_CAP - 1


def entity_casing_stats(texts, vocab):
    """Share of MID-SENTENCE entity mentions the user writes capitalized.

    Mid-sentence only, on purpose: a phone autocapitalizes sentence starts,
    and action D never touches them, so counting them would teach the
    governor a rate for a position it cannot act on. Returns the card block.
    """
    total = caps = 0
    per_token = {}  # token -> [caps, total]
    for text in texts:
        s = (text or "").strip()
        if not s:
            continue
        sentence_start = True
        for raw in re.finditer(r"\S+", s):
            word = raw.group(0)
            core = word.strip("\"'(),;:.!?…-")
            low = core.lower()
            skip = (word.startswith(("http", "www.")) or "@" in word or "://" in word)
            if core and not skip and not sentence_start and low in vocab:
                is_cap = core[0].isupper() and not core.isupper()
                total += 1
                caps += 1 if is_cap else 0
                slot = per_token.setdefault(low, [0, 0])
                slot[0] += 1 if is_cap else 0
                slot[1] += 1
            if word.endswith((".", "!", "?")):
                sentence_start = True
            elif core:
                sentence_start = False

    tokens = []
    for tok, (c, n) in per_token.items():
        # Only tokens the user ACTUALLY capitalizes earn a table slot: the
        # table is an allowlist, and a 0.0 rate would be inert anyway.
        if n < ENTITY_MIN_MENTIONS or c == 0:
            continue
        if len(tok) > ENTITY_TOKEN_MAX_LEN or not re.fullmatch(r"[a-z']+", tok):
            continue
        tokens.append({"token": tok, "cap_rate": round(c / n, 4), "n": n})
    # Highest-n first: the C side truncates at ENTITY_MAX_TOKENS, so the
    # tokens carrying the most mentions must survive the cut.
    tokens.sort(key=lambda t: (-t["n"], t["token"]))
    return {
        "rate": round(caps / total, 4) if total else 0.0,
        "n_mentions": total,
        "n_capitalized": caps,
        "min_mentions": ENTITY_MIN_MENTIONS,
        "max_tokens": ENTITY_MAX_TOKENS,
        "position": "mid-sentence only (action D never touches sentence starts)",
        "source": "scripts/specificity_score.py insider_vocab (persona contacts + graph entities)",
        "tokens": tokens[:ENTITY_MAX_TOKENS],
    }


def attach_entity_casing(card: dict, texts) -> dict:
    try:
        from specificity_score import insider_vocab
        vocab = {v for v in insider_vocab() if " " not in v}
    except Exception as exc:  # pragma: no cover - vocab sources are optional
        print(f"entity_casing: vocab unavailable ({exc}); axis omitted", file=sys.stderr)
        return card
    if not vocab:
        print("entity_casing: empty vocab; axis omitted", file=sys.stderr)
        return card
    card["entity_casing"] = entity_casing_stats(texts, vocab)
    return card


def attach_second_beat(card: dict, runs) -> dict:
    """axes.second_beat_rate: share of the user's replies (runs of bubbles to
    one inbound run) that carry a second thought (reply_pairs.has_second_beat).
    Rendered by the C card only when HU_STYLE_SECOND_BEAT=live."""
    from reply_pairs import has_second_beat
    runs = [r for r in runs if r]
    if runs:
        hits = sum(1 for r in runs if has_second_beat(r))
        card["axes"]["second_beat_rate"] = {"value": hits / len(runs), "n": len(runs)}
    return card


def attach_substantive(card: dict, pairs, days: int) -> dict:
    stats = reply_stats(pairs)
    stats["days"] = days
    stats["min_n"] = SUBSTANTIVE_MIN_N
    stats["source"] = ("chat.db inbound >= 150 chars or a real question -> the user's next "
                       "reply in-chat within 30 min")
    card["substantive_reply"] = stats
    return card


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--db", default=DEFAULT_DB)
    p.add_argument("--persona", default=DEFAULT_PERSONA)
    p.add_argument("--days", type=int, default=DEFAULT_DAYS,
                   help="window length ending at --end (default: %(default)s)")
    p.add_argument("--end", default=None, help="window end, YYYY-MM-DD (default: today)")
    p.add_argument("--min-n", type=int, default=DEFAULT_MIN_N,
                   help="refuse to write a card below this many messages (default: %(default)s)")
    p.add_argument("--n-resamples", type=int, default=2000)
    p.add_argument("--seed", type=int, default=42)
    p.add_argument("--out", default=None,
                   help="card path (default: $HU_PERSONA_DIR or ~/.human/personas/<persona>.style-card.json)")
    p.add_argument("--substantive-days", type=int, default=DEFAULT_SUBSTANTIVE_DAYS,
                   help="lookback for the judge-free substantive_reply axis")
    p.add_argument("--dry-run", action="store_true", help="print the card, write nothing")
    return p.parse_args(argv)


def run(args, messages=None, substantive_pairs=None, reply_runs=None) -> int:
    end = (datetime.datetime.strptime(args.end, "%Y-%m-%d")
           if args.end else datetime.datetime.now())
    start = end - datetime.timedelta(days=args.days)
    live = messages is None
    if messages is None:
        messages = fetch_outbound_messages(args.db, start, end,
                                           exclude_handles=owner_handles(args.persona))
        from extract_imessage_pairs import load_daemon_records
        before = len(messages)
        messages = drop_daemon_sends(messages, load_daemon_records())
        sys.stderr.write(f"excluded {before - len(messages)} daemon sends of {before}\n")
    try:
        card = build_card(messages, args.persona, start, end, min_n=args.min_n,
                          n_resamples=args.n_resamples, seed=args.seed)
    except InsufficientData as e:
        sys.stderr.write(f"REFUSED: {e}; wrote nothing.\n")
        return 1
    # Pair axis: hermetic callers pass substantive_pairs (or omit
    # substantive_days) and never touch chat.db.
    sdays = getattr(args, "substantive_days", None)
    if substantive_pairs is None and sdays:
        substantive_pairs = fetch_reply_pairs(args.db, sdays, is_substantive)
    if substantive_pairs is not None:
        attach_substantive(card, substantive_pairs, sdays or DEFAULT_SUBSTANTIVE_DAYS)
    # Second-beat axis: reply runs, the twin's own excluded like the axes above.
    if reply_runs is None and live:
        from extract_imessage_pairs import daemon_send_predicate, load_daemon_records
        from reply_pairs import fetch_reply_runs
        is_daemon = daemon_send_predicate(load_daemon_records())
        runs = fetch_reply_runs(args.db, args.days, skip_handles=owner_handles(args.persona))
        reply_runs = [[t for _, t in r] for r in runs
                      if not any(is_daemon(t, ts) for ts, t in r)]
    if reply_runs is not None:
        attach_second_beat(card, reply_runs)
    # Entity-casing axis over the SAME window the axes above used.
    attach_entity_casing(card, [t for ts, t in messages if start <= ts < end])

    print(json.dumps(card, indent=2))
    if args.dry_run:
        return 0
    out = args.out or default_card_path(args.persona)
    Path(out).parent.mkdir(parents=True, exist_ok=True)
    tmp = out + ".tmp"
    Path(tmp).write_text(json.dumps(card, indent=2) + "\n")
    os.replace(tmp, out)
    sys.stderr.write(f"wrote {out} (n={card['n']}, window={card['window']['days']}d)\n")
    return 0


def main(argv=None) -> int:
    return run(parse_args(argv))


if __name__ == "__main__":
    sys.exit(main())
