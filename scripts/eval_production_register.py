#!/usr/bin/env python3
"""eval_production_register.py — the register the product actually sends, measured.

Everything the multi-turn harness said about "substantive register" in
2026-09 (yes-man agreement, "[affirmation] — [comment]") turned out to be
the dynamics of a 30-turn scripted contact: 101 real production turns
opened on agreement 0.09 of the time and carried no dash, against the
harness's 0.46–0.62. The evidence about the product is in
`production_outcomes` — the inbound (`prompt`) and the text the send funnel
delivered (`chosen`, overwritten by hu_daemon_record_delivered_reply). This
stage measures that, judge-free, against the persona's own measured cards:

  substantive inbounds  (reply_pairs.is_substantive)  vs  style card `substantive_reply`
  distress inbounds     (reply_pairs.is_distress)     vs  emotion card `distress_reply`
  everything            (dash rate, agreement openers, assistant scaffolds)

Same definitions as the cards (reply_pairs.reply_stats, eval_persona_evolution
.has_dash, emotion_register.SUPPORT_SCAFFOLDS), so a delta is a delta and not
two hand-rolled counters disagreeing.

Contract (no-number-without-a-measurement): refuses with exit 3 and writes
NOTHING below --min-n delivered turns in the window; a register with fewer
than --min-register-n turns reports its n and no deltas; a card that lacks
an axis yields no delta for it. `gaps` is advisory and provisional — the
thresholds are named in the verdict, and nothing reads this file as a gate.
Read-only on memory.db; no text leaves the process (the verdict carries
counts, never replies).
"""
import argparse
import datetime
import json
import os
import sqlite3
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from emotion_register import SUPPORT_SCAFFOLDS  # noqa: E402
from eval_persona_evolution import has_dash  # noqa: E402
from reply_pairs import is_distress, is_substantive, reply_stats  # noqa: E402

SCHEMA = "production-register/v1"
DEFAULT_DB = os.path.expanduser("~/.human/memory.db")
DEFAULT_OUTPUT = os.path.expanduser("~/.human/logs/eval-production-register-latest.json")
# Provisional: named here and in the verdict; nothing gates on them yet.
GAP_AGREEMENT_DELTA = 0.20
GAP_DASH_RATE = 0.05
GAP_LENGTH_RATIO = 2.0
GAP_SCAFFOLD_RATE = 0.10


def persona_dir():
    return os.environ.get("HU_PERSONA_DIR") or os.path.expanduser("~/.human/personas")


def load_card(persona, kind):
    """kind: 'style' | 'emotion'. None when absent or unreadable."""
    path = os.path.join(persona_dir(), f"{persona}.{kind}-card.json")
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def fetch_turns(db_path, days):
    """[(inbound, delivered, ts, channel)] for the window, read-only."""
    con = sqlite3.connect(f"file:{db_path}?mode=ro", uri=True)
    try:
        rows = con.execute(
            "SELECT prompt, chosen, send_timestamp, channel FROM production_outcomes "
            "WHERE send_timestamp >= ? AND chosen IS NOT NULL AND chosen <> '' "
            "ORDER BY send_timestamp",
            (int(datetime.datetime.now().timestamp()) - int(days) * 86400,),
        ).fetchall()
    finally:
        con.close()
    return [(p or "", c, ts, ch) for p, c, ts, ch in rows]


def classify(inbound):
    if is_distress(inbound):
        return "distress"
    if is_substantive(inbound):
        return "substantive"
    return "casual"


def register_stats(pairs):
    """reply_stats plus the two tells. Empty dict shape when n == 0."""
    st = reply_stats(pairs)
    if st.get("n", 0) == 0:
        return {"n": 0}
    replies = [r for _, r in pairs]
    st["dash_rate"] = sum(1 for r in replies if has_dash(r)) / len(replies)
    st["scaffold_rate"] = sum(1 for r in replies if SUPPORT_SCAFFOLDS.search(r)) / len(replies)
    st.pop("window_seconds", None)
    return st


def deltas(twin, ref, keys):
    """twin − ref for every key both carry; ratio for median_chars."""
    out = {}
    if not twin or twin.get("n", 0) == 0 or not ref:
        return out
    for k in keys:
        if k in twin and isinstance(ref.get(k), (int, float)):
            out[k] = round(twin[k] - ref[k], 4)
    if "median_chars" in twin and isinstance(ref.get("median_chars"), (int, float)) and ref["median_chars"]:
        out["median_chars_ratio"] = round(twin["median_chars"] / ref["median_chars"], 3)
    return out


def measure(turns, style_card, emotion_card, min_register_n):
    """Pure. turns: [(inbound, delivered, ts, channel)]. Returns the verdict body."""
    groups = {"substantive": [], "distress": [], "casual": []}
    for inbound, delivered, _, _ in turns:
        groups[classify(inbound)].append((inbound, delivered))
    all_pairs = [p for g in groups.values() for p in g]
    registers = {}
    for name, pairs in groups.items():
        st = register_stats(pairs)
        st["measured"] = st.get("n", 0) >= min_register_n
        registers[name] = st
    overall = register_stats(all_pairs)

    seth = {}
    sr = (style_card or {}).get("substantive_reply") or {}
    if sr.get("n"):
        seth["substantive"] = {k: sr[k] for k in ("n", "median_chars", "share_le_60_chars",
                                                   "answer_first_rate", "agreement_opener_rate") if k in sr}
        seth["substantive"]["source"] = "style card substantive_reply"
    dr = (emotion_card or {}).get("distress_reply") or {}
    if dr.get("n"):
        seth["distress"] = {k: dr[k] for k in ("n", "median_chars", "scaffold_rate") if k in dr}
        seth["distress"]["source"] = "emotion card distress_reply"
    dash_axis = ((style_card or {}).get("axes") or {}).get("dash_rate") or {}
    if isinstance(dash_axis.get("value"), (int, float)):
        seth["overall"] = {"dash_rate": dash_axis["value"], "n": dash_axis.get("n"),
                           "source": "style card axes.dash_rate"}

    d = {}
    if registers["substantive"]["measured"] and "substantive" in seth:
        d["substantive"] = deltas(registers["substantive"], seth["substantive"],
                                  ("share_le_60_chars", "answer_first_rate", "agreement_opener_rate"))
    if registers["distress"]["measured"] and "distress" in seth:
        d["distress"] = deltas(registers["distress"], seth["distress"], ("scaffold_rate",))
    if overall.get("n") and "overall" in seth:
        d["overall"] = deltas(overall, seth["overall"], ("dash_rate",))

    gaps = []
    sub = d.get("substantive", {})
    if sub.get("agreement_opener_rate", 0) > GAP_AGREEMENT_DELTA:
        gaps.append(f"substantive agreement_opener_rate +{sub['agreement_opener_rate']:.2f} over the persona")
    if sub.get("median_chars_ratio", 0) > GAP_LENGTH_RATIO:
        gaps.append(f"substantive replies {sub['median_chars_ratio']:.1f}x the persona's median length")
    if registers["distress"]["measured"] and registers["distress"].get("scaffold_rate", 0) > GAP_SCAFFOLD_RATE:
        gaps.append(f"distress scaffold_rate {registers['distress']['scaffold_rate']:.2f}")
    if overall.get("n") and overall.get("dash_rate", 0) > GAP_DASH_RATE:
        gaps.append(f"dash_rate {overall['dash_rate']:.2f} (persona ~0)")
    return {"registers": registers, "overall": overall, "seth": seth, "deltas_twin_minus_seth": d,
            "gaps": gaps,
            "gap_thresholds": {"agreement_opener_delta": GAP_AGREEMENT_DELTA, "dash_rate": GAP_DASH_RATE,
                               "median_chars_ratio": GAP_LENGTH_RATIO, "scaffold_rate": GAP_SCAFFOLD_RATE,
                               "status": "provisional — advisory, nothing gates on this file"}}


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--db", default=DEFAULT_DB)
    p.add_argument("--persona", default="seth")
    p.add_argument("--days", type=int, default=14)
    p.add_argument("--min-n", type=int, default=20, help="refuse below this many delivered turns")
    p.add_argument("--min-register-n", type=int, default=5, help="a register below this reports n only")
    p.add_argument("--output-json", default=DEFAULT_OUTPUT)
    p.add_argument("--dry-run", action="store_true")
    return p.parse_args(argv)


def main(argv=None) -> int:
    args = parse_args(argv)
    try:
        turns = fetch_turns(args.db, args.days)
    except sqlite3.Error as e:
        sys.stderr.write(f"REFUSED: cannot read {args.db}: {e}; wrote nothing.\n")
        return 3
    if len(turns) < args.min_n:
        sys.stderr.write(f"REFUSED: {len(turns)} delivered turns in {args.days} d (< {args.min_n}); "
                         "wrote nothing.\n")
        return 3
    body = measure(turns, load_card(args.persona, "style"), load_card(args.persona, "emotion"),
                   args.min_register_n)
    channels = {}
    for _, _, _, ch in turns:
        channels[ch] = channels.get(ch, 0) + 1
    verdict = {
        "schema": SCHEMA,
        "generated_at": datetime.datetime.now().replace(microsecond=0).isoformat(),
        "source": "scripts/eval_production_register.py",
        "what": "delivered production replies (production_outcomes.chosen) by inbound register, "
                "judge-free, against the persona's measured cards",
        "window": {"days": args.days, "n": len(turns), "min_n": args.min_n,
                   "min_register_n": args.min_register_n, "channels": channels,
                   "first": datetime.datetime.fromtimestamp(turns[0][2]).isoformat(timespec="seconds"),
                   "last": datetime.datetime.fromtimestamp(turns[-1][2]).isoformat(timespec="seconds")},
        **body,
    }
    r = verdict["registers"]
    o = verdict["overall"]
    line = (f"MEASURED n={len(turns)} ({args.days} d) substantive n={r['substantive']['n']} "
            f"distress n={r['distress']['n']} | agreement_opener {o.get('agreement_opener_rate', 0):.2f} "
            f"dash {o.get('dash_rate', 0):.2f} scaffold {o.get('scaffold_rate', 0):.2f} median {o.get('median_chars')} chars | "
            f"gaps: {'; '.join(verdict['gaps']) or 'none'}")
    if args.dry_run:
        print(json.dumps(verdict, indent=2))
        print(line)
        return 0
    os.makedirs(os.path.dirname(args.output_json) or ".", exist_ok=True)
    tmp = f"{args.output_json}.tmp-{os.getpid()}"
    with open(tmp, "w") as f:
        json.dump(verdict, f, indent=2)
    os.replace(tmp, args.output_json)
    print(line + f" -> {args.output_json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
