#!/usr/bin/env python3
"""Reply-turn latency report from the daemon's service log. Aggregate only.

Prints counts and timings, never message text, names, handles or numbers.
Handles are used in memory only, to pair each turn with its inbound line.

Per sent reply turn (method of the 2026-10-02 latency profile):

  total       inbound -> send                      (what the contact waits)
  delay       the director's deliberate delay      ("director delay: N ms")
  processing  total - delay                        (the work; the number to cut)
  in->call    inbound -> "calling agent turn"
  turn        "calling agent turn" -> "agent turn result"
  tail        "agent turn result" -> send
  calls       local chat / local embedding / cloud POSTs from inbound to send

Since 8d0ca0d06 a delay of 15 s or less is held while the work runs, so the
send waits for whichever ends last. When the hold ends last, processing is
censored (the work finished earlier); those turns are counted as hold_bound.

Also sums the HU_POST_SEND_DEFER flush lines (one per turn): jobs, facts and
facts_literal, the counts the SHADOW -> LIVE promotion reads
(docs/guides/reply-latency.md).

  python3 scripts/reply_latency_report.py [--since 2026-10-02T05:16] [--log PATH] [--json]
"""
import argparse
import json
import os
import re
import sys
from datetime import datetime

DEFAULT_LOG = os.path.expanduser("~/.human/logs/service-loop-error.log")
TS = re.compile(r"^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2})")
PATTERNS = [
    ("incoming", re.compile(r"\[imessage\] incoming handle=(\S+)")),
    ("ddelay", re.compile(r"\[human\] director delay: (\d+) ms")),
    ("call", re.compile(r"\[human\] calling agent turn for (\S+?)\.\.\.")),
    ("result", re.compile(r"\[human\] agent turn result: err=(\S+) response_len=\d+ for (\S+)")),
    ("dispatch", re.compile(r"\[human\] imessage_dispatch: ")),
    ("abort", re.compile(r"\[human\] pre-send abort")),
    ("post", re.compile(r"\[http\] POST (\S+) \(body_len=\d+\)")),
    ("restart", re.compile(r"\[human\] service loop started")),
    ("defer", re.compile(
        r"\[HU_POST_SEND_DEFER (shadow|live)\] jobs=(\d+) extract=(\d+) embed=(\d+) ran=(\d+) "
        r"inline_full=(\d+) facts=(\d+) facts_literal=(\d+) flush_ms=(\d+)")),
]
HOLD_BOUND_S = 0.5  # processing at or below this is the hold, not the work


def post_kind(url):
    if "127.0.0.1" in url or "localhost" in url:
        return "local_emb" if "/embeddings" in url else "local_chat"
    return "cloud"


def parse(lines, since=None):
    """[(datetime, kind, groups)] for the lines the report reads. Text-bearing
    lines are never matched, so no message content enters the pipeline."""
    ev = []
    for raw in lines:
        m = TS.match(raw)
        if not m or (since and m.group(1) < since):
            continue
        for name, pat in PATTERNS:
            g = pat.search(raw)
            if g:
                ev.append((datetime.fromisoformat(m.group(1)), name, g.groups()))
                break
    return ev


def turns(ev):
    """One record per agent turn that has a result line; send/abort attached."""
    out = []
    last_end = {}
    for i, (t, name, g) in enumerate(ev):
        if name != "call":
            continue
        key = g[0]
        res = None
        for j in range(i + 1, len(ev)):
            tj, nj, gj = ev[j]
            if (tj - t).total_seconds() > 900 or nj == "restart":
                break
            if nj == "result" and gj[1] == key:
                res = (tj, j)
                break
        if not res:
            continue
        end = None
        for j in range(res[1] + 1, min(len(ev), res[1] + 400)):
            tj, nj, _ = ev[j]
            if nj in ("dispatch", "abort"):
                end = (tj, nj)
                break
            if nj == "call":
                break
        lo = last_end.get(key)
        tin, delays = None, []
        for j in range(i - 1, -1, -1):
            tj, nj, gj = ev[j]
            if (t - tj).total_seconds() > 900 or (lo and tj < lo):
                break
            if nj == "incoming" and gj[0][:20] == key:  # the call line truncates to 20
                tin = tj
            elif nj == "ddelay":
                delays.append((tj, int(gj[0]) / 1000.0))
            elif nj == "result" and gj[1] == key:
                break
        # the nearest delay line logged after this turn's inbound
        delay = next((dv for tj, dv in delays if tin and tj >= tin), 0.0)
        last_end[key] = res[0]
        if tin is None:
            continue
        stop = end[0] if end else res[0]
        calls = {"local_chat": 0, "local_emb": 0, "cloud": 0}
        for tj, nj, gj in ev:
            if nj == "post" and tin <= tj <= stop:
                calls[post_kind(gj[0])] += 1
        out.append({"tin": tin, "call": t, "result": res[0], "end": end, "delay": delay,
                    "calls": calls})
    return out


def pct(values, q):
    v = sorted(values)
    return round(v[min(len(v) - 1, int(q * len(v)))], 1) if v else None


def summarize(ev):
    ts = turns(ev)
    sent = [x for x in ts if x["end"] and x["end"][1] == "dispatch"]
    d = lambda a, b: (b - a).total_seconds()  # noqa: E731
    total = [d(x["tin"], x["end"][0]) for x in sent]
    proc = [d(x["tin"], x["end"][0]) - x["delay"] for x in sent]

    def row(vals):
        return {"n": len(vals), "p50": pct(vals, 0.5), "p90": pct(vals, 0.9),
                "mean": round(sum(vals) / len(vals), 1) if vals else None}

    defer = [g for _, n, g in ev if n == "defer"]
    rep = {
        "turns": len(ts),
        "sent": len(sent),
        "aborted": sum(1 for x in ts if x["end"] and x["end"][1] == "abort"),
        "total_s": row(total),
        "delay_s": row([x["delay"] for x in sent]),
        "processing_s": row(proc),
        "hold_bound": sum(1 for p in proc if p <= HOLD_BOUND_S),
        "in_to_call_s": row([d(x["tin"], x["call"]) for x in sent]),
        "turn_s": row([d(x["call"], x["result"]) for x in sent]),
        "tail_s": row([d(x["result"], x["end"][0]) for x in sent]),
        "calls_per_turn": {k: row([x["calls"][k] for x in sent])
                           for k in ("local_chat", "local_emb", "cloud")},
        "post_send_defer": {
            "flush_lines": len(defer),
            "modes": sorted({g[0] for g in defer}),
            "extract": sum(int(g[2]) for g in defer),
            "embed": sum(int(g[3]) for g in defer),
            "ran": sum(int(g[4]) for g in defer),
            "inline_full": sum(int(g[5]) for g in defer),
            "facts": sum(int(g[6]) for g in defer),
            "facts_literal": sum(int(g[7]) for g in defer),
            "flush_ms": row([int(g[8]) for g in defer]),
        },
    }
    return rep


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--log", default=DEFAULT_LOG)
    ap.add_argument("--since", help="ISO timestamp prefix, e.g. 2026-10-02T05:16")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args(argv)
    if not os.path.exists(a.log):
        print(f"no log at {a.log}", file=sys.stderr)
        return 2
    with open(a.log, "rb") as f:
        ev = parse((ln.decode("utf-8", "replace") for ln in f), a.since)
    rep = summarize(ev)
    if rep["sent"] == 0:
        print("no sent reply turns in the window; nothing measured", file=sys.stderr)
        return 1
    if a.json:
        print(json.dumps(rep, indent=1))
        return 0
    print(f"turns={rep['turns']} sent={rep['sent']} aborted={rep['aborted']} "
          f"hold_bound={rep['hold_bound']}")
    for k in ("total_s", "delay_s", "processing_s", "in_to_call_s", "turn_s", "tail_s"):
        r = rep[k]
        print(f"{k:14s} p50={r['p50']} p90={r['p90']} mean={r['mean']} n={r['n']}")
    for k, r in rep["calls_per_turn"].items():
        print(f"calls/{k:11s} p50={r['p50']} p90={r['p90']} mean={r['mean']}")
    psd = rep["post_send_defer"]
    if psd["flush_lines"]:
        print(f"post_send_defer modes={','.join(psd['modes'])} turns={psd['flush_lines']} "
              f"extract={psd['extract']} embed={psd['embed']} ran={psd['ran']} "
              f"inline_full={psd['inline_full']} facts={psd['facts']} "
              f"facts_literal={psd['facts_literal']} flush_ms_p50={psd['flush_ms']['p50']}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
