#!/usr/bin/env python3
"""
Phase H3 (2026-05-18) — active-learning probe (the killer feature).

Periodically asks Seth which of N candidate responses he'd actually
send. Each answer is a gold-standard preference pair — orders of
magnitude richer than synthetic data, hundreds of times faster than
waiting for organic REWRITE events to accumulate.

How it works:
  1. Pick a recent INCOMING user message from H1's corpus (a real
     message someone sent Seth that he hasn't responded to yet, OR
     a held-out message we deliberately want feedback on).
  2. Generate K candidate responses via the daemon's chat completion
     endpoint (different sampling temperatures / personas).
  3. Push the candidates to Seth as a single iMessage with format:
        Help me learn — which would you send to <X>?
        A) <candidate 1>
        B) <candidate 2>
        C) <candidate 3>
        Reply with a letter (or write your own).
  4. Wait for Seth's reply (1-tap). His answer is the gold label:
       - "A" / "B" / "C" — pick that candidate as `chosen`, the
         others as `rejected` (3 pairs from 1 probe)
       - <free text> — Seth's text IS the chosen; all candidates
         become rejected (3 pairs, signal even richer)
  5. Append to ~/.human/training-data/m3-active-probe-pairs.jsonl
     (Alpaca-DPO shape, ready for dpo-train consumption).

Production wiring:
  - Delivery: writes to a queue file (default) or invokes the
    iMessage send wire when --delivery=imessage is set
  - Response capture: polls memory.db / chat.db for replies to
    the probe message ID
  - Decoupling: the probe doesn't BLOCK on the response — it queues
    the question, exits, and a separate process collects answers
    when they arrive

For local testing:
  - --simulate-delivery prints the question instead of sending it
  - --simulate-response=<letter> picks an answer automatically (so
    the test loop can prove the end-to-end shape without a real
    user)

Run:
    python3 scripts/m3_active_probe.py \\
        --corpus ~/.human/training-data/m3-corpus.jsonl \\
        --simulate-delivery --simulate-response=A

Exit codes:
    0 — probe queued / pair written successfully
    2 — input missing / no eligible messages
    3 — response timed out (in non-simulate mode)
"""
from __future__ import annotations

import argparse
import json
import os
import random
import sys
import time
from pathlib import Path

DEFAULT_CORPUS = Path.home() / ".human" / "training-data" / "m3-corpus.jsonl"
DEFAULT_PAIRS_OUT = Path.home() / ".human" / "training-data" / "m3-active-probe-pairs.jsonl"
DEFAULT_QUEUE = Path.home() / ".human" / "training-data" / "m3-active-probe-queue.jsonl"

# Sentinel header so a probe message is distinguishable from any
# ordinary text in the iMessage thread (the response-collection step
# uses this to find the right thread to listen on).
PROBE_HEADER = "🧠 [m3 probe]"


# ─────────────────────────────────────────────────────────────────────
# Probe selection — pick an eligible user message
# ─────────────────────────────────────────────────────────────────────

def pick_eligible_user_message(corpus_path: Path, rng: random.Random) -> dict | None:
    """Find an INCOMING (role=user) message from the corpus that has
    no Seth reply within 24h after it (i.e. Seth never responded —
    the perfect case for asking 'what would you have said?').

    Falls back to ANY recent user message if no unanswered ones found
    in the last N records. Caller decides whether that's acceptable.
    """
    if not corpus_path.exists():
        return None
    # Bucket by contact and sort by ts_ms (oldest first)
    by_contact: dict[str, list[dict]] = {}
    with open(corpus_path) as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            by_contact.setdefault(r.get("handle", ""), []).append(r)
    candidates = []  # user messages with no Seth reply within 24h
    fallback = []    # any recent user message
    DAY_MS = 24 * 3600 * 1000
    for handle, records in by_contact.items():
        records.sort(key=lambda r: r.get("ts_ms", 0))
        for i, r in enumerate(records):
            if r.get("role") != "user":
                continue
            fallback.append(r)
            # Check the next 24h for a Seth response
            responded = False
            for j in range(i + 1, len(records)):
                if records[j].get("ts_ms", 0) - r.get("ts_ms", 0) > DAY_MS:
                    break
                if records[j].get("role") == "assistant":
                    responded = True
                    break
            if not responded:
                candidates.append(r)
    pool = candidates or fallback
    if not pool:
        return None
    return rng.choice(pool[-200:])  # bias to RECENT (last 200 records)


# ─────────────────────────────────────────────────────────────────────
# Candidate generation — call gateway /v1/chat/completions K times
# ─────────────────────────────────────────────────────────────────────

def generate_candidates_via_gateway(gateway_url: str, user_message: str,
                                     k: int = 3, temperatures: list[float] | None = None
                                     ) -> list[str]:
    """Hit the daemon's /v1/chat/completions K times with different
    temperatures. Returns list of response strings.

    Soft-fail: returns [] if the gateway is unreachable. Caller falls
    back to synthetic candidates."""
    import urllib.error
    import urllib.request as _u
    if temperatures is None:
        temperatures = [0.3, 0.7, 1.1][:k]
    results = []
    for t in temperatures:
        body = json.dumps({
            "model": "mlx_local",
            "messages": [{"role": "user", "content": user_message}],
            "max_tokens": 80,
            "temperature": t,
        }).encode()
        req = _u.Request(f"{gateway_url.rstrip('/')}/v1/chat/completions",
                         data=body, method="POST",
                         headers={"Content-Type": "application/json"})
        try:
            with _u.urlopen(req, timeout=5) as resp:
                payload = json.loads(resp.read().decode())
            text = (payload.get("choices", [{}])[0]
                           .get("message", {}).get("content", "").strip())
            if text:
                results.append(text)
        except (urllib.error.URLError, urllib.error.HTTPError,
                json.JSONDecodeError, KeyError, TimeoutError, OSError):
            # Soft-fail on any transport/parse error — caller falls back
            # to synthetic candidates. Includes TimeoutError (Python 3.10+
            # socket.timeout) and OSError (connection refused, etc.).
            pass
    return results


def synthetic_candidates(user_message: str, k: int) -> list[str]:
    """Deterministic candidates for the simulate path. Three obvious
    styles: terse, neutral, verbose. The point isn't quality — it's
    that the probe shape works end-to-end."""
    base = user_message.strip()
    return [
        "yeah",                                          # terse
        "yeah, sounds good — let me think about it",     # medium
        "Yes, that works for me. Let me know when you'd like to proceed.",  # verbose
    ][:k]


# ─────────────────────────────────────────────────────────────────────
# Question formatting + delivery
# ─────────────────────────────────────────────────────────────────────

def format_probe_question(user_message: str, candidates: list[str], handle: str) -> str:
    """The single iMessage that gets sent to Seth. Compact, A/B/C
    labeled, with a sentinel header that the response collector
    can grep for."""
    letters = "ABCDEFGHIJ"
    lines = [PROBE_HEADER]
    lines.append(f"Which would you send to {handle[:8]}?")
    lines.append(f"They wrote: {user_message[:120]}")
    lines.append("")
    for i, c in enumerate(candidates):
        lines.append(f"{letters[i]}) {c[:200]}")
    lines.append("")
    lines.append("Reply with a letter (or write your own).")
    return "\n".join(lines)


def deliver_probe(question: str, mode: str, queue_path: Path) -> str:
    """Push the question to Seth via the chosen delivery mode.

    Modes:
      - simulate: print to stdout (test path)
      - queue: append to queue file (production-safe default — a
        separate process reads the queue and sends via iMessage)
      - imessage: directly invoke `human channel send imessage ...`
        — only available when the daemon is running with the
        imessage channel and the operator has confirmed delivery
    """
    if mode == "simulate":
        print("\n  [SIMULATE-DELIVERY] would send to Seth:")
        for line in question.split("\n"):
            print(f"  | {line}")
        return "simulated"
    if mode == "queue":
        queue_path.parent.mkdir(parents=True, exist_ok=True)
        entry = {"ts_ms": int(time.time() * 1000),
                 "question": question, "status": "pending"}
        with open(queue_path, "a") as f:
            f.write(json.dumps(entry, ensure_ascii=False) + "\n")
        print(f"\n  Queued probe → {queue_path}")
        return "queued"
    # mode == "imessage" — actually shell out to the daemon's send
    # path. The exact CLI invocation depends on the channel surface;
    # for now we document and require operator wiring.
    print(f"\n  NOTE: --delivery=imessage requires the daemon's send wire.")
    print(f"        Queue mode is the safe default; iMessage is a follow-up slice.")
    return "not-implemented"


# ─────────────────────────────────────────────────────────────────────
# Response parsing → preference pairs
# ─────────────────────────────────────────────────────────────────────

def response_to_pairs(user_message: str, candidates: list[str],
                       response: str) -> list[dict]:
    """Turn Seth's answer into one or more Alpaca-DPO pairs.

    Cases:
      - Single letter A-J: pick that candidate as chosen, others as
        rejected. Yields (len-1) pairs.
      - Free text: that text is the chosen; all candidates are
        rejected. Yields len(candidates) pairs.
      - Empty / unparseable: returns [] (probe wasn't answered).
    """
    response = (response or "").strip()
    if not response:
        return []
    letters = "ABCDEFGHIJ"
    first = response[0].upper()
    pairs = []
    if first in letters[:len(candidates)] and (len(response) <= 3 or response[1] in " )."):
        idx = letters.index(first)
        chosen = candidates[idx]
        for i, c in enumerate(candidates):
            if i == idx:
                continue
            pairs.append({"prompt": user_message,
                           "chosen": chosen, "rejected": c,
                           "_source": "active_probe"})
    else:
        # Free text — Seth wrote his own. Strongest signal possible.
        for c in candidates:
            pairs.append({"prompt": user_message,
                           "chosen": response, "rejected": c,
                           "_source": "active_probe_freetext"})
    return pairs


# ─────────────────────────────────────────────────────────────────────
# Entry point
# ─────────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--corpus", type=Path, default=DEFAULT_CORPUS)
    ap.add_argument("--pairs-out", type=Path, default=DEFAULT_PAIRS_OUT)
    ap.add_argument("--queue", type=Path, default=DEFAULT_QUEUE)
    ap.add_argument("--gateway-url", default=os.environ.get(
        "HUMAN_GATEWAY_URL", "http://127.0.0.1:3006"))
    ap.add_argument("--candidates", type=int, default=3)
    ap.add_argument("--simulate-delivery", action="store_true",
                    help="Print the probe instead of sending (test path)")
    ap.add_argument("--simulate-response",
                    help="Auto-answer the probe — letter or free text — to "
                         "exercise the response→pairs flow without a real user")
    ap.add_argument("--delivery", choices=["simulate", "queue", "imessage"],
                    default="queue", help="Delivery mode (default: queue)")
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()

    delivery_mode = "simulate" if args.simulate_delivery else args.delivery

    print(f"\n{'='*60}")
    print(f"  M3 ACTIVE PROBE (H3)")
    print(f"{'='*60}")
    print(f"  Corpus:    {args.corpus}")
    print(f"  Delivery:  {delivery_mode}")
    print(f"  Pairs out: {args.pairs_out}")

    rng = random.Random(args.seed) if args.seed else random.Random()
    target = pick_eligible_user_message(args.corpus, rng)
    if not target:
        print(f"  ERROR: no eligible user messages in corpus {args.corpus}",
              file=sys.stderr)
        return 2
    user_message = target.get("content", "")
    handle = target.get("handle", "")
    print(f"  Target:    handle={handle} msg={user_message[:60]!r}")

    # Try the gateway first; fall back to synthetic
    candidates = generate_candidates_via_gateway(args.gateway_url, user_message,
                                                  args.candidates)
    if not candidates:
        print(f"  Gateway unreachable / no completions — using synthetic candidates")
        candidates = synthetic_candidates(user_message, args.candidates)
    print(f"  Candidates: {len(candidates)}")
    for i, c in enumerate(candidates):
        print(f"    {chr(65+i)}) {c[:80]}")

    question = format_probe_question(user_message, candidates, handle)
    status = deliver_probe(question, delivery_mode, args.queue)

    if args.simulate_response is None:
        print(f"\n  Probe {status}. Pair generation waits for response.")
        return 0

    # Simulate-response path: immediately consume the simulated answer
    pairs = response_to_pairs(user_message, candidates, args.simulate_response)
    if not pairs:
        print(f"  WARN: simulated response {args.simulate_response!r} produced no pairs",
              file=sys.stderr)
        return 0

    args.pairs_out.parent.mkdir(parents=True, exist_ok=True)
    with open(args.pairs_out, "a") as f:
        for p in pairs:
            f.write(json.dumps(p, ensure_ascii=False) + "\n")
    print(f"\n  Wrote {len(pairs)} pair(s) → {args.pairs_out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
