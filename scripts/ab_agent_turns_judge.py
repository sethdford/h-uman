#!/usr/bin/env python3
"""Score an ab_agent_turns.py result file.

Per arm: replies, empties, median wall time and length, second-beat rate
(reply_pairs.has_second_beat over the reply's lines) and question rate.
Then a blind pairwise judge (Gemini via Vertex ADC, temperature 0, random A/B
order): which reply reads more like a real person texting back, and, where
Seth's own reply to that message exists, which is closer to it.

  scripts/ab_agent_turns_judge.py ~/.human/benchmarks/ab-agent-turns/ab_planner.jsonl off live

The latest row per (context, arm) counts, so a retried failure replaces the
failed row. A pair with an empty reply on either side is not judged; empties
are reported per arm (an empty reply is a failure, not a draw), and judge
errors are counted, never folded into a side.
"""
import collections, json, os, random, re, statistics, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from reply_pairs import has_second_beat  # noqa: E402

JUDGE = "gemini-3.1-pro-preview"
PROMPT = """You are judging two candidate text-message replies. Someone texted a man named Seth; each candidate is a possible reply from Seth.

Their message:
<<<{inbound}>>>
{ref}
Reply A:
<<<{a}>>>

Reply B:
<<<{b}>>>

{question}
Answer with exactly one token: A, B, or TIE."""
Q_HUMAN = ("Which reply reads more like a real person texting back a friend or partner "
           "(natural, specific, not an assistant), and actually responds to what they said?")
Q_CLOSE = "Which reply is closer to how Seth actually replied (shown above), in substance and in shape?"


def latest(rows):
    by = collections.defaultdict(dict)
    for r in rows:
        by[r["id"]][r["arm"]] = r
    return by


def metrics(rows):
    n = len(rows)
    ok = [r for r in rows if r.get("rc") == 0 and r["reply"].strip()]
    out = {"n": n, "empty_or_failed": n - len(ok)}
    if rows:
        out["secs_median"] = statistics.median(r["secs"] for r in rows)
    if ok:
        lines = [[b for b in r["reply"].split("\n") if b.strip()] for r in ok]
        out["len_median"] = statistics.median(len(r["reply"]) for r in ok)
        out["second_beat"] = round(sum(has_second_beat(l) for l in lines) / len(ok), 3)
        out["question"] = round(sum("?" in r["reply"] for r in ok) / len(ok), 3)
    return out


def parse_verdict(text):
    m = re.search(r"\b(TIE|A|B)\b", (text or "").strip().upper())
    return m.group(1) if m else None


def judge(inbound, a, b, question, ref=""):
    from eval_longmemeval_qa import call_gemini  # Vertex ADC, thinking budget explicit
    prompt = PROMPT.format(inbound=inbound, a=a, b=b, question=question, ref=ref)
    for _ in range(2):
        try:
            v = parse_verdict(call_gemini(prompt, JUDGE, thinking_budget=512, max_tokens=1024))
        except Exception:  # noqa: BLE001 — counted as an error below
            v = None
        if v:
            return v
    return "ERR"


def main(argv=None):
    argv = argv or sys.argv[1:]
    if len(argv) != 3:
        sys.exit(__doc__)
    path, arm0, arm1 = argv
    by = latest(json.loads(l) for l in open(path))
    for arm in (arm0, arm1):
        print(arm, metrics([v[arm] for v in by.values() if arm in v]))
    rng = random.Random(11)
    tally = {"human": collections.Counter(), "close": collections.Counter()}
    for v in by.values():
        if arm0 not in v or arm1 not in v:
            continue
        r0, r1 = v[arm0], v[arm1]
        if not (r0.get("rc") == 0 and r0["reply"].strip() and r1.get("rc") == 0 and r1["reply"].strip()):
            continue
        flip = rng.random() < 0.5
        a, b = (r1, r0) if flip else (r0, r1)

        def winner(verdict):
            if verdict in ("TIE", "ERR"):
                return verdict.lower()
            return (arm1 if flip else arm0) if verdict == "A" else (arm0 if flip else arm1)

        tally["human"][winner(judge(r0["inbound"], a["reply"], b["reply"], Q_HUMAN))] += 1
        if r0.get("seth"):
            ref = f"\nHow Seth actually replied:\n<<<{r0['seth']}>>>\n"
            tally["close"][winner(judge(r0["inbound"], a["reply"], b["reply"], Q_CLOSE, ref))] += 1
    for k, c in tally.items():
        if c:
            print(f"judge[{k}]: {dict(c)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
