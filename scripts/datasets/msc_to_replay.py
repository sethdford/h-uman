#!/usr/bin/env python3
"""Multi-Session Chat (MSC, ParlAI) -> replay-harness turns + callback probes.

Input: msc_dialogue/session_5/{valid,test}.txt from ParlAI's msc_v0.1 tarball
(fetch with scripts/datasets/fetch_msc.sh). Each line is one episode: four
`previous_dialogs` plus the fifth session in `dialog`. Speakers alternate,
Speaker 1 first.

Sessions become one history. MSC gives no calendar dates, only how long ago
each earlier session was (`time_back`, e.g. "6 days 12 hours ago"), so the
fifth session is anchored at a fixed synthetic date and earlier sessions are
placed `time_back` before it.

Callback probes (where derivable, LLM-free): a persona fact the Seth speaker
established in sessions 1-4 that matches a fixed template ("I work as a
nurse", "my favorite food is ramen", "I have two cats", ...) becomes a
question the contact asks after session 5 ("what do you do for work again?").
A probe is kept only when its gold answer actually appears in one of Seth's
own history lines, so every probe is answerable from the conversation.

Crowd-sourced people, not Seth: memory mechanics only. Never train on them.
"""
import argparse
import ast
import json
import re
import sys
from datetime import timedelta
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import replay_common as rc  # noqa: E402

ANCHOR = "2024-03-01 19:00:00"
_UNIT_S = {"minute": 60, "hour": 3600, "day": 86400, "week": 7 * 86400,
           "month": 30 * 86400, "year": 365 * 86400}
_NUMBER = r"two|three|four|five|six|seven|eight|nine|ten|\d+"
_PHRASE = r"[a-z0-9' -]+?"

# (pattern over one lowercased persona sentence, question template)
TEMPLATES = (
    (rf"i work as (?:an? |the )?(?P<g>{_PHRASE})", "what do you do for work again?"),
    (rf"i work at (?:an? |the )?(?P<g>{_PHRASE})", "where do you work again?"),
    (rf"i live in (?:an? |the )?(?P<g>{_PHRASE})", "where do you live again?"),
    (rf"i(?: am|'m) from (?P<g>{_PHRASE})", "where are you from again?"),
    (rf"my favorite (?P<t>[a-z]+(?: [a-z]+)?) is (?P<g>{_PHRASE})", "what's your favorite {t} again?"),
    (rf"i drive an? (?P<g>{_PHRASE})", "what do you drive again?"),
    (rf"i have (?P<g>{_NUMBER}) (?P<t>[a-z]+)", "how many {t} do you have again?"),
    (rf"my (?P<t>[a-z]+)(?:'s)? name is (?P<g>[a-z]+)", "what's your {t}'s name again?"),
    (rf"i have an? (?P<t>[a-z]+) named (?P<g>[a-z]+)", "what's your {t}'s name again?"),
)
_COMPILED = tuple((re.compile(r"^" + p + r"$"), q) for p, q in TEMPLATES)
MAX_GOLD_WORDS = 4
# Answers too generic to show recall: any reply mentioning "the city" would match.
GENERIC_GOLDS = {"city", "big city", "small city", "town", "small town", "country",
                 "countryside", "suburbs", "suburb", "house", "apartment", "farm", "home",
                 "area", "place", "same place", "nice place", "job", "company", "store"}


def _seconds_back(text):
    return sum(int(n) * _UNIT_S[u] for n, u in
               re.findall(r"(\d+)\s*(minute|hour|day|week|month|year)s?", text or ""))


def _personas(raw):
    if isinstance(raw, list):
        return raw
    try:
        val = ast.literal_eval(raw or "[]")
    except (ValueError, SyntaxError):
        return [[], []]
    return val if isinstance(val, list) else [[], []]


def _sentences(persona_lines):
    for line in persona_lines or []:
        for s in re.split(r"(?<=[.!?])\s+", str(line)):
            s = s.strip().rstrip(".!").strip()
            if s:
                yield s


def callback_facts(persona_lines):
    """[(question, gold_answers, source_sentence)] for template-matching facts."""
    out = []
    for sent in _sentences(persona_lines):
        low = sent.lower()
        for rx, qtmpl in _COMPILED:
            m = rx.match(low)
            if not m:
                continue
            gold_low = m.group("g").strip()
            if (not gold_low or len(gold_low.split()) > MAX_GOLD_WORDS
                    or rc.normalize(gold_low) in GENERIC_GOLDS):
                break
            start = m.start("g")
            gold = sent[start:start + len(gold_low)]  # keep the source casing
            golds = [gold]
            num = rc.normalize(gold)
            if num != gold.lower() and num.isdigit():
                golds.append(num)
            q = qtmpl.format(t=m.groupdict().get("t") or "")
            out.append((q, golds, sent))
            break
    return out


def _messages(ep, seth_speaker, anchor):
    prev = ep.get("previous_dialogs") or []
    sessions = [(p.get("dialog") or [], p) for p in prev] + [(ep.get("dialog") or [], None)]
    msgs, prev_end = [], None
    for si, (dialog, meta) in enumerate(sessions):
        start = anchor - timedelta(seconds=_seconds_back(meta.get("time_back"))) if meta else anchor
        if prev_end is not None and start <= prev_end:
            start = prev_end + timedelta(seconds=60)
        dt = start
        for ti, turn in enumerate(dialog):
            text = str(turn.get("text", "")).strip()
            if not text:
                continue
            speaker = 1 if ti % 2 == 0 else 2  # previous dialogs carry no ids
            if msgs:
                dt = max(dt, msgs[-1]["dt"] + timedelta(seconds=1))
            msgs.append({"from_me": speaker == seth_speaker, "text": text, "dt": dt,
                         "session": si + 1})
            dt = dt + timedelta(seconds=rc.typing_gap_seconds(text))
        prev_end = msgs[-1]["dt"] if msgs else prev_end
    return msgs


def convert(episodes, seth_speaker=2, window=rc.DAEMON_HISTORY_WINDOW, probe_gap_hours=24,
            history_limit=0, anchor=ANCHOR):
    anchor_dt = rc.parse_dt(anchor, rc.TS_FMT)
    turns, probes = [], []
    for n, ep in enumerate(episodes):
        eid = str((ep.get("metadata") or {}).get("initial_data_id") or f"ep{n}")
        contact_id = rc.bench_contact_id("msc", eid)
        msgs = _messages(ep, seth_speaker, anchor_dt)
        if not msgs:
            continue
        turns += rc.build_turns(msgs, contact_id, f"msc:{eid}", history_limit)

        full = [rc.history_entry(m["from_me"], m["text"], m["dt"]) for m in msgs]
        hist = full[-history_limit:] if history_limit else full
        offset = len(full) - len(hist)
        seth_norm = [(i, rc.normalize(m["text"])) for i, m in enumerate(msgs) if m["from_me"]]
        ask_at = rc.fmt_ts(msgs[-1]["dt"] + timedelta(hours=probe_gap_hours))
        seen = set()
        for p in ep.get("previous_dialogs") or []:
            lines = _personas(p.get("personas"))
            mine = lines[seth_speaker - 1] if len(lines) >= seth_speaker else []
            for question, golds, sent in callback_facts(mine):
                if question in seen:
                    continue
                gnorm = [rc.normalize(g) for g in golds]
                idxs = [i for i, t in seth_norm if any(rc.contains_phrase(t, g) for g in gnorm)]
                if not idxs:
                    continue  # never said in the conversation: not a fair probe
                seen.add(question)
                rel = [i - offset for i in idxs if i - offset >= 0]
                in_win = any(i >= len(hist) - window for i in rel)
                pid = f"msc:{eid}:probe:{len(seen) - 1:04d}"
                probes.append({
                    "id": pid,
                    "contact_id": contact_id,
                    "inbound_bubbles": [rc.casual_question(question, "", "", pid)],
                    "ts": ask_at,
                    "history": hist,
                    "probe": {
                        "dataset": "msc",
                        "sample_id": eid,
                        "category": "single_hop",
                        "question_original": question,
                        "gold_answers": golds,
                        "adversarial_answer": None,
                        "persona_fact": sent,
                        "evidence_history_idx": rel,
                        "evidence_in_history": bool(rel),
                        "evidence_in_window": in_win,
                        "window": window,
                    },
                })
    stats = rc.common_stats(turns, probes, len(episodes))
    stats.update({"dataset": "msc", "window": window, "anchor": anchor})
    return {"turns": turns, "probes": probes, "stats": stats}


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--input", nargs="+", required=True, help="session_5/{valid,test}.txt")
    ap.add_argument("--out-dir", required=True)
    ap.add_argument("--seth-speaker", type=int, choices=(1, 2), default=2)
    ap.add_argument("--window", type=int, default=rc.DAEMON_HISTORY_WINDOW)
    ap.add_argument("--probe-gap-hours", type=float, default=24.0)
    ap.add_argument("--history-limit", type=int, default=0)
    a = ap.parse_args(argv)
    eps = [json.loads(line) for f in a.input
           for line in Path(f).read_text(encoding="utf-8").splitlines() if line.strip()]
    res = convert(eps, a.seth_speaker, a.window, a.probe_gap_hours, a.history_limit)
    if not res["turns"]:
        print("msc_to_replay: 0 turns produced; refusing to write", file=sys.stderr)
        return 2
    print(rc.write_outputs(a.out_dir, res))
    return 0


if __name__ == "__main__":
    sys.exit(main())
