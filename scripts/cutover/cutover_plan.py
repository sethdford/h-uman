#!/usr/bin/env python3
"""Planning helpers for the human-level cut-over kit (scripts/cutover/run_cutover.sh).

  gates        which candidate humanness gates the binary actually contains
  arms         the replay_driver.py --arm specs: A, B, and B minus each gate
  estimate     the runtime of a run, printed before anything starts
  prep-probes  memory probes (LoCoMo/MSC replay format) -> a driver turns file

Prints counts, gate names and arm specs only; never message text.
Runbook: docs/guides/cutover-kit.md.
"""
import argparse
import datetime
import json
import os
import re
import sys

# The new humanness stack, in arm order. Each is HU_<NAME>=off|shadow|live,
# default OFF. Arm A sets every present one to off, arm B sets every present
# one to live, and ablation "B-no-<name>" is B with that one gate off.
CANDIDATE_GATES = (
    "HU_THREAD_CONTEXT",     # #581
    "HU_HISTORY_BUDGET",     # #581
    "HU_IMMERSIVE_CONTEXT",  # #575 (may be absent)
    "HU_LENGTH_POLICY",      # #580 (merged)
    "HU_LEARNED_STYLE",      # #586
    "HU_DIRECTOR_V2",        # #590
    "HU_CONTEXT_RELEVANCE",  # #602
)
# Never part of the humanness arms: these keep production's value in every arm.
EXCLUDED_GATES = {
    "HU_GRIEF_DECAY": "not on the reactive reply path",
    "HU_POST_SEND_DEFER": "a latency change, not a humanness one",
}

# Runtime model. The replay runbook measured 15-25 min for 2 arms x 40 turns
# (80 turn-arms) at --delay-ms 3000 on :8741: 11-19 s per turn-arm, all in.
# The kit plans with the slow end of that, plus a margin for the longer
# memory-probe prompts; the judge is ~4 s per item on Gemma 4 26B plus pacing.
SEC_PER_TURN_ARM = 18.0
SEC_PER_PROBE_ARM = 22.0
SEC_PER_JUDGE_ITEM = 4.0
TEXT_SHARE = 0.85  # share of turns where both Seth and the arm answered in text

_GATE_TOKEN = re.compile(rb"HU_[A-Z0-9_]{3,64}")


def detect_gates(binary_path):
    """The CANDIDATE_GATES whose env name appears as a string in the binary
    (`human replay` has no --list-gates). A name can also appear outside its
    gate (a log tag); the report's INERT check catches a gate that is present
    by name but never changes a reply request."""
    with open(binary_path, "rb") as f:
        found = {m.group(0).decode() for m in _GATE_TOKEN.finditer(f.read())}
    return [g for g in CANDIDATE_GATES if g in found]


def arm_name_for(gate):
    return "B-no-" + gate[len("HU_"):].lower()


def gate_for_arm(arm):
    if not arm.startswith("B-no-"):
        return None
    return "HU_" + arm[len("B-no-"):].upper()


def build_arms(gates):
    """[(name, {gate: value})] in run order: A, B, then one ablation per gate."""
    for g in gates:
        if g not in CANDIDATE_GATES:
            raise ValueError(f"{g} is not a candidate humanness gate")
    if not gates:
        raise ValueError("no candidate gate is present in the binary: nothing to measure")
    arms = [("A", {g: "off" for g in gates}), ("B", {g: "live" for g in gates})]
    for g in gates:
        env = {x: "live" for x in gates}
        env[g] = "off"
        arms.append((arm_name_for(g), env))
    return arms


def arm_spec(name, env):
    return name + ":" + ",".join(f"{k}={v}" for k, v in env.items())


def estimate(turns, probes, n_arms, mem_arms, delay_ms, judge_pace_s):
    """Seconds per phase. delay_ms is already inside SEC_PER_TURN_ARM at 3000;
    a different delay shifts it by the difference."""
    shift = (delay_ms - 3000) / 1000.0
    gen = turns * n_arms * max(1.0, SEC_PER_TURN_ARM + shift)
    mem = probes * mem_arms * max(1.0, SEC_PER_PROBE_ARM + shift)
    items = int(round(turns * TEXT_SHARE)) * n_arms
    judge = items * (SEC_PER_JUDGE_ITEM + judge_pace_s)
    return {"turn_arms": turns * n_arms, "probe_arms": probes * mem_arms, "judge_items": items,
            "generation_s": gen, "memory_s": mem, "judge_s": judge,
            "total_s": gen + mem + judge}


def fmt_dur(s):
    m = int(round(s / 60.0))
    return f"{m // 60}h{m % 60:02d}m" if m >= 60 else f"{m}m"


def _epoch(ts):
    if isinstance(ts, (int, float)):
        return int(ts)
    return int(datetime.datetime.strptime(str(ts), "%Y-%m-%d %H:%M:%S").timestamp())


def sample_probes(probes, n):
    """Deterministic, category-balanced: round-robin over categories in sorted
    order, each category in file order, until n."""
    by_cat = {}
    for p in probes:
        by_cat.setdefault(p["probe"]["category"], []).append(p)
    cats = sorted(by_cat)
    out, depth = [], 0
    while len(out) < n and any(depth < len(by_cat[c]) for c in cats):
        for c in cats:
            if depth < len(by_cat[c]) and len(out) < n:
                out.append(by_cat[c][depth])
        depth += 1
    return out


def prep_probes(src, dst, n):
    """Probe file -> driver turns file (0600): `ts` becomes epoch seconds (the
    driver's time filter needs a number); every other field is kept, so
    memory_probe_score.py reads the same file as its --probes."""
    with open(src) as f:
        probes = [json.loads(line) for line in f if line.strip()]
    probes = [p for p in probes if isinstance(p.get("probe"), dict)]
    picked = sample_probes(probes, n)
    if not picked:
        raise ValueError("no probes in the source file")
    fd = os.open(dst, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w") as f:
        for p in picked:
            q = dict(p)
            q["ts"] = _epoch(p["ts"])
            f.write(json.dumps(q, ensure_ascii=False) + "\n")
    cats = {}
    for p in picked:
        cats[p["probe"]["category"]] = cats.get(p["probe"]["category"], 0) + 1
    return len(picked), cats


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("gates")
    g.add_argument("--binary", required=True)
    a = sub.add_parser("arms")
    a.add_argument("--gates", required=True, help="comma-separated, from `gates`")
    a.add_argument("--only", default="", help="comma-separated arm names to keep")
    e = sub.add_parser("estimate")
    e.add_argument("--turns", type=int, required=True)
    e.add_argument("--probes", type=int, required=True)
    e.add_argument("--arms", type=int, required=True)
    e.add_argument("--mem-arms", type=int, required=True)
    e.add_argument("--delay-ms", type=int, default=3000)
    e.add_argument("--judge-pace-s", type=float, default=2.0)
    p = sub.add_parser("prep-probes")
    p.add_argument("--src", required=True)
    p.add_argument("--dst", required=True)
    p.add_argument("--n", type=int, default=50)
    o = ap.parse_args(argv)
    try:
        if o.cmd == "gates":
            for gate in detect_gates(o.binary):
                print(gate)
        elif o.cmd == "arms":
            keep = set(filter(None, o.only.split(",")))
            for name, env in build_arms([x for x in o.gates.split(",") if x]):
                if not keep or name in keep:
                    print(arm_spec(name, env))
        elif o.cmd == "estimate":
            est = estimate(o.turns, o.probes, o.arms, o.mem_arms, o.delay_ms, o.judge_pace_s)
            print(f"runtime estimate: {fmt_dur(est['total_s'])} total = "
                  f"generation {fmt_dur(est['generation_s'])} ({est['turn_arms']} turn-arms on "
                  f"the replay endpoint) + memory {fmt_dur(est['memory_s'])} "
                  f"({est['probe_arms']} probe-arms) + judge {fmt_dur(est['judge_s'])} "
                  f"(~{est['judge_items']} items)")
        else:
            n, cats = prep_probes(o.src, o.dst, o.n)
            print(f"memory probes: {n} " + " ".join(f"{k}={v}" for k, v in sorted(cats.items())))
    except (OSError, ValueError) as err:
        print(f"refusing: {err}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
