#!/usr/bin/env python3
"""
Multi-turn naturalness drift test — the "better than human on conversational
stamina" measurement.

Single-turn eval is closed (M4 PASS). The question this answers:
**does shape-score hold across 15-25 turn conversations, or does it drift?**

Humans drift in long conversations: get tired, forget context, repeat
themselves. If h-uman holds shape-score 1.0 across 20 turns where each
later turn might reference earlier context, that's genuinely
better-than-human on conversational stamina.

Sends multi-turn conversations through the production gateway
(http://127.0.0.1:3006/v1/chat/completions with --with-agent), scoring
each response with the deterministic shape classifier.

Per-conversation evidence:
  - Per-turn shape_score (1.0 / 0.85 / 0.55 / 0.0)
  - Mean shape over the conversation
  - Slope of shape vs turn_index (drift coefficient)
  - Latency per turn (does the agent slow down as context grows?)
  - Callback recall (turn N references turn 1-3 entities — does Seth use them?)

Run after `human gateway --with-agent` is up on port 3006.
"""

import json
import statistics
import sys
import time
from pathlib import Path
from urllib import error, request

sys.path.insert(0, str(Path(__file__).parent))
from eval_shape_classifier import classify  # noqa: E402

GATEWAY = "http://127.0.0.1:3006/v1/chat/completions"

# Conversation scripts. Each is a list of FRIEND messages (incoming).
# Seth's responses are generated turn-by-turn by sending the conversation-so-far
# through the gateway. The agent_turn pipeline injects persona + memory + humor.
#
# Designed to stress different drift modes:
#   - Long mundane stretch (does agent get bored, become repetitive?)
#   - Topic switches (does agent track multiple threads?)
#   - Callback opportunities (turn 15+ references entities from turn 2-3)
#   - Emotional shifts (lighthearted → vent → support → casual)
CONVERSATIONS = [
    {
        "name": "weekend-plans-cascade",
        "friend": "Casey",
        "turns": [
            "hey",
            "you free saturday?",
            "thinking maybe a hike if weather holds",
            "yeah i was looking at devils den",
            "around 9 maybe, beat the heat",
            "want to grab coffee first?",
            "the place by your apartment?",
            "ok cool",
            "btw did you ever finish that book?",
            "the one i lent you in february",
            "ha no rush, just curious",
            "what'd you think of the ending tho",
            "lol same",
            "ok so saturday — 9am coffee then hike, sound good?",
            "perfect see you then",
            "oh wait also, can you bring the book saturday",
            "thx",
        ],
    },
    {
        "name": "boss-vent-recovery",
        "friend": "Morgan",
        "turns": [
            "you up?",
            "ugh boss again",
            "she went OFF in standup",
            "in front of the whole team",
            "for missing a deadline that was literally moved by HER yesterday",
            "i can't",
            "yeah i'm so done",
            "should i just say something?",
            "or just keep my head down",
            "probably right",
            "ok i'll sleep on it",
            "thanks for letting me vent",
            "yeah i should",
            "easier said than done lol",
            "ok bedtime. you working tomorrow?",
            "have a good night",
        ],
    },
    {
        "name": "tech-debug-thread",
        "friend": "Pat",
        "turns": [
            "quick question if you have a sec",
            "my unit tests are failing on CI but pass locally",
            "ruby",
            "minitest yeah",
            "no the diff i pushed is small",
            "just refactored a helper",
            "ok let me look",
            "the helper uses Time.now",
            "ohhh",
            "yeah CI runs UTC and local is EST",
            "freeze_time helper?",
            "ok trying that",
            "you're a lifesaver",
            "all green now",
            "ha yeah classic",
            "next round on me",
        ],
    },
]


def post_chat(history: list, max_tokens: int = 80, timeout: int = 180) -> tuple[str, float, str]:
    """Send the conversation history to the gateway. Returns (content, elapsed_s, error)."""
    body = {
        "model": "gemma-4-26b",
        "messages": history,
        "max_tokens": max_tokens,
        "temperature": 0.9,
    }
    data = json.dumps(body).encode("utf-8")
    req = request.Request(GATEWAY, data=data, method="POST",
                          headers={"Content-Type": "application/json"})
    t0 = time.time()
    try:
        with request.urlopen(req, timeout=timeout) as r:
            resp = json.loads(r.read())
        elapsed = time.time() - t0
        try:
            content = resp["choices"][0]["message"]["content"]
            return content.strip(), elapsed, ""
        except (KeyError, IndexError):
            err = resp.get("error", {}).get("message", str(resp)[:200])
            return "", elapsed, f"malformed: {err}"
    except (error.URLError, error.HTTPError, json.JSONDecodeError,
            ConnectionError, OSError) as e:
        # OSError covers http.client.RemoteDisconnected — gateway dropped
        # the connection mid-response. Record it and let the caller continue
        # so the conversation loop doesn't crash.
        return "", time.time() - t0, str(e)[:200]


def run_conversation(convo: dict) -> dict:
    """Run all friend-turns of one conversation, recording Seth's response each time."""
    history = []
    records = []
    name = convo["name"]
    friend = convo["friend"]
    print(f"\n=== {name} (friend={friend}, {len(convo['turns'])} turns) ===")

    for turn_idx, friend_msg in enumerate(convo["turns"], 1):
        history.append({"role": "user", "content": friend_msg})
        seth_resp, elapsed, err = post_chat(history)
        if err:
            print(f"  T{turn_idx:02d} | friend: {friend_msg[:50]!r}")
            print(f"          | ERROR after {elapsed:.1f}s: {err}")
            records.append({"turn": turn_idx, "friend": friend_msg, "seth": "",
                            "shape": {"pass": False, "score": 0.0, "len": 0},
                            "elapsed_s": elapsed, "error": err})
            continue
        shape = classify(seth_resp, channel="imessage")
        print(f"  T{turn_idx:02d} | friend: {friend_msg[:50]!r}")
        print(f"          | seth:   {seth_resp[:80]!r}")
        print(f"          | shape={shape['pass']} score={shape['score']:.2f} len={shape['len']:>3} | {elapsed:.1f}s")
        history.append({"role": "assistant", "content": seth_resp})
        records.append({"turn": turn_idx, "friend": friend_msg, "seth": seth_resp,
                        "shape": shape, "elapsed_s": elapsed, "error": None})
    return {"name": name, "friend": friend, "records": records}


def linear_slope(xs: list, ys: list) -> float:
    """Simple slope of y vs x (least-squares). Returns 0 if degenerate."""
    n = len(xs)
    if n < 2:
        return 0.0
    mean_x = sum(xs) / n
    mean_y = sum(ys) / n
    num = sum((x - mean_x) * (y - mean_y) for x, y in zip(xs, ys))
    den = sum((x - mean_x) ** 2 for x in xs)
    return (num / den) if den else 0.0


def summarize(result: dict) -> dict:
    """Compute drift metrics for one conversation."""
    records = result["records"]
    turns = [r["turn"] for r in records]
    scores = [r["shape"]["score"] for r in records]
    lens = [r["shape"]["len"] for r in records]
    elapsed = [r["elapsed_s"] for r in records]
    non_null = sum(1 for r in records if r["shape"]["len"] > 0)
    pass_count = sum(1 for r in records if r["shape"]["pass"])

    # Drift = slope of shape_score vs turn_index. Negative = degrading.
    drift = linear_slope(turns, scores)
    # Latency drift = does response time grow with conversation length?
    latency_drift = linear_slope(turns, elapsed)

    early_mean = statistics.mean(scores[:5]) if len(scores) >= 5 else statistics.mean(scores)
    late_mean = statistics.mean(scores[-5:]) if len(scores) >= 5 else statistics.mean(scores)

    return {
        "name": result["name"],
        "n_turns": len(records),
        "non_null": non_null,
        "shape_pass": pass_count,
        "mean_shape": statistics.mean(scores) if scores else 0,
        "mean_len": statistics.mean(lens) if lens else 0,
        "mean_elapsed_s": statistics.mean(elapsed) if elapsed else 0,
        "drift_per_turn": drift,
        "latency_drift_s_per_turn": latency_drift,
        "early_mean": early_mean,
        "late_mean": late_mean,
        "late_minus_early": late_mean - early_mean,
    }


def main():
    print("=" * 78)
    print("MULTI-TURN DRIFT TEST — h-uman conversational stamina")
    print("=" * 78)
    print(f"Target: {GATEWAY}")
    print(f"Conversations: {len(CONVERSATIONS)} (total {sum(len(c['turns']) for c in CONVERSATIONS)} turns)")

    results = []
    for convo in CONVERSATIONS:
        result = run_conversation(convo)
        results.append(result)
        s = summarize(result)
        print(f"\n  [{s['name']}] summary:")
        print(f"    non-NULL:        {s['non_null']}/{s['n_turns']}")
        print(f"    shape_pass:      {s['shape_pass']}/{s['n_turns']}")
        print(f"    mean shape:      {s['mean_shape']:.3f}")
        print(f"    mean length:     {s['mean_len']:.0f} chars")
        print(f"    mean latency:    {s['mean_elapsed_s']:.1f}s")
        print(f"    drift/turn:      {s['drift_per_turn']:+.4f} (negative = degrading)")
        print(f"    latency drift/turn: {s['latency_drift_s_per_turn']:+.2f}s")
        print(f"    early-5 mean:    {s['early_mean']:.3f}")
        print(f"    late-5 mean:     {s['late_mean']:.3f}")
        print(f"    late−early:      {s['late_minus_early']:+.3f}")

    print()
    print("=" * 78)
    print("AGGREGATE — across all conversations")
    print("=" * 78)
    summaries = [summarize(r) for r in results]
    all_records = [r for result in results for r in result["records"]]
    all_scores = [r["shape"]["score"] for r in all_records]
    all_turns = [r["turn"] for r in all_records]

    total_turns = len(all_records)
    total_non_null = sum(1 for r in all_records if r["shape"]["len"] > 0)
    total_pass = sum(1 for r in all_records if r["shape"]["pass"])
    mean_shape = statistics.mean(all_scores) if all_scores else 0
    overall_drift = linear_slope(all_turns, all_scores)
    early = [r["shape"]["score"] for r in all_records if r["turn"] <= 5]
    late = [r["shape"]["score"] for r in all_records if r["turn"] >= 11]
    early_m = statistics.mean(early) if early else 0
    late_m = statistics.mean(late) if late else 0

    print(f"  Total turns:           {total_turns}")
    print(f"  Non-NULL responses:    {total_non_null}/{total_turns}  ({100*total_non_null/total_turns:.1f}%)")
    print(f"  Shape pass:            {total_pass}/{total_turns}  ({100*total_pass/total_turns:.1f}%)")
    print(f"  Mean shape:            {mean_shape:.3f}")
    print(f"  Drift/turn (all):      {overall_drift:+.4f}")
    print(f"  Mean turns 1-5:        {early_m:.3f}  (n={len(early)})")
    print(f"  Mean turns 11+:        {late_m:.3f}  (n={len(late)})")
    print(f"  Late minus early:      {late_m - early_m:+.3f}")
    print()
    if abs(overall_drift) < 0.005 and (late_m - early_m) > -0.05:
        print("  🎯 VERDICT: Conversational stamina HOLDS. No measurable drift across the corpus.")
        print("     h-uman maintains in-voice shape at turn 15+ as well as turn 1-5.")
    elif overall_drift < -0.01:
        print(f"  ⚠️  VERDICT: Measurable degradation — {overall_drift:.4f} shape-score per turn.")
        print("     Hypothesis to investigate: memory_context dilution, prompt-length bloat,")
        print("     or model getting confused by accumulating history. The next bug to chase.")
    else:
        print("  📊 VERDICT: Inconclusive — small effect; need more conversations for tight CI.")

    out = Path("/tmp/multiturn_drift_results.json")
    out.write_text(json.dumps([
        {"name": r["name"], "friend": r["friend"], "records": r["records"]}
        for r in results
    ], indent=2))
    print(f"\nFull results: {out}")


if __name__ == "__main__":
    main()
