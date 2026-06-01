#!/usr/bin/env python3
"""GraphRAG grounding A/B — measures the MARGINAL effect of community-summary
grounding (HU_GRAPH_GROUNDING=on vs off) on reply quality.

The existing scripts/eval_blinded_ab.py binds NO contact, so grounding never
fires there and the blind_ab_gate.json cannot measure grounding. This harness
fixes that by driving the `human agent --contact <id>` seam (PR #258): for each
incoming, it generates a reply with grounding ON and one with it OFF — same
contact, same model — and asks the gemini judge which reply is more natural /
human. Grounding "wins" when ON is judged better; a win-rate at or below 50%
means grounding is not substantiated.

Privacy: defaults to the SYNTHETIC incomings already in eval_blinded_ab.py — no
chat.db export, nothing real leaves the machine except synthetic prompts + the
two AI replies to the judge (the user's own Vertex project via ADC). Pass
--real-triples <file.jsonl> to use real incomings (opt-in).

Usage:
  python3 scripts/grounding_ab.py --contact "+447914633409" --n 10
  python3 scripts/grounding_ab.py --contact "+447914633409" --real-triples t.jsonl
  python3 scripts/grounding_ab.py --contact "+447914633409" --n 2 --pilot

Writes results to data/grounding_ab_result.json (resumable). With --write-gate,
records the proxy half into docs/evaluation/blind_ab_gate.json.
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import eval_blinded_ab as eab  # reuse call_gemini + judge schema  # noqa: E402

HUMAN_BIN = os.environ.get("HUMAN_BIN", os.path.join(HERE, "..", "build", "human"))
RESULT_PATH = os.path.join(HERE, "..", "data", "grounding_ab_result.json")


def gen_reply(contact, incoming, grounding, timeout):
    """One agent turn with grounding on|off, contact bound. Returns reply text."""
    env = {**os.environ, "HU_GRAPH_GROUNDING": grounding}
    try:
        r = subprocess.run(
            [HUMAN_BIN, "agent", "--contact", contact, "-m", incoming],
            capture_output=True, text=True, timeout=timeout, env=env,
        )
    except subprocess.TimeoutExpired:
        return None
    import re
    out = re.sub(r"\x1b\[[0-9;?]*[a-zA-Z]", "", r.stdout)
    lines = [l.strip() for l in out.splitlines()
             if l.strip() and l.strip() != "Goodbye."]
    return " ".join(lines) if lines else None


def judge_more_human(incoming, a, b):
    """Reuse eval_blinded_ab's blinded judge. Returns 'A'|'B'|None."""
    try:
        verdict = eab.blinded_judge(incoming, a, b)
        return verdict.get("choice") if isinstance(verdict, dict) else None
    except Exception as e:
        print(f"  judge error: {e}", file=sys.stderr)
        return None


def load_incomings(args):
    if args.real_triples:
        rows = []
        with open(args.real_triples) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                d = json.loads(line)
                inc = d.get("incoming") or d.get("message")
                if inc:
                    rows.append(inc)
        return rows[: args.n] if args.n else rows
    incs = [s["incoming"] for s in eab.SYNTHETIC_SCENARIOS]
    n = 2 if args.pilot else (args.n or len(incs))
    # repeat synthetic set if n exceeds the pool
    out = []
    while len(out) < n:
        out.extend(incs)
    return out[:n]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--contact", required=True)
    ap.add_argument("--n", type=int, default=0)
    ap.add_argument("--pilot", action="store_true")
    ap.add_argument("--real-triples", default=None)
    ap.add_argument("--timeout", type=int, default=180)
    ap.add_argument("--write-gate", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(HUMAN_BIN):
        print(f"ERROR: human binary not found at {HUMAN_BIN}", file=sys.stderr)
        return 2

    incomings = load_incomings(args)
    print(f"grounding A/B: contact={args.contact} n={len(incomings)} "
          f"source={'real' if args.real_triples else 'synthetic'}")

    trials, on_wins, off_wins, errs = [], 0, 0, 0
    for i, inc in enumerate(incomings):
        t0 = time.time()
        on = gen_reply(args.contact, inc, "on", args.timeout)
        off = gen_reply(args.contact, inc, "off", args.timeout)
        if not on or not off:
            errs += 1
            print(f"[{i+1}/{len(incomings)}] gen-fail (on={bool(on)} off={bool(off)})")
            continue
        # Randomize order so the judge can't position-bias toward ON.
        on_is_a = (i % 2 == 0)
        a, b = (on, off) if on_is_a else (off, on)
        choice = judge_more_human(inc, a, b)
        winner = None
        if choice == "A":
            winner = "on" if on_is_a else "off"
        elif choice == "B":
            winner = "off" if on_is_a else "on"
        if winner == "on":
            on_wins += 1
        elif winner == "off":
            off_wins += 1
        else:
            errs += 1
        trials.append({"incoming": inc, "on": on, "off": off,
                       "winner": winner, "secs": round(time.time() - t0, 1)})
        print(f"[{i+1}/{len(incomings)}] winner={winner} ({trials[-1]['secs']}s)")

    decided = on_wins + off_wins
    on_rate = (on_wins / decided * 100) if decided else 0.0
    result = {
        "contact": args.contact,
        "n_incomings": len(incomings),
        "source": "real" if args.real_triples else "synthetic",
        "on_wins": on_wins, "off_wins": off_wins, "errors": errs,
        "on_win_rate_pct": round(on_rate, 1),
        "decided": decided,
        "trials": trials,
    }
    os.makedirs(os.path.dirname(RESULT_PATH), exist_ok=True)
    with open(RESULT_PATH, "w") as f:
        json.dump(result, f, indent=2)

    print("\n=== GROUNDING A/B RESULT ===")
    print(f"  ON wins:  {on_wins}")
    print(f"  OFF wins: {off_wins}")
    print(f"  errors:   {errs}")
    print(f"  ON win-rate: {on_rate:.1f}% of {decided} decided")
    verdict = ("SUBSTANTIATED (ON > 55%)" if on_rate > 55 else
               "INCONCLUSIVE (50-55%)" if on_rate >= 50 else
               "NOT SUBSTANTIATED (ON < 50% — consider default SHADOW)")
    print(f"  verdict: {verdict}")
    print(f"  written: {RESULT_PATH}")

    if args.write_gate:
        try:
            import blind_ab_gate as gate
            gate.write_proxy_half(  # type: ignore[attr-defined]
                fool_rate=on_rate, n_real_pairs=decided,
                baseline=None)
            print(f"  gate updated: {gate.GATE_PATH}")
        except Exception as e:
            print(f"  gate write skipped: {e}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
