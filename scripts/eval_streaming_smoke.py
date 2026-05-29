#!/usr/bin/env python3
"""Streaming-readiness tripwire for the local gemma-realtime server (:8741).

WHY THIS EXISTS
---------------
The realtime-feel of on-device conversation is gated on TOKEN-BY-TOKEN
delivery. Two halves must both be true before flipping
`cfg.mlx_local.streaming_enabled = true` in production:

  1. CLIENT side (DONE): src/providers/compatible.c consumes SSE and the
     harmony filter (src/util/harmony_filter.c) strips <|channel|> / <|...|>
     thought markers incrementally. Pinned by tests/test_harmony_filter.c.

  2. SERVER side (NOT DONE as of 2026-05-29): the gemma-realtime server on
     :8741 currently BUFFERS — it generates the entire reply, then emits it
     as a single SSE chunk, so time-to-first-token == total time. Enabling
     streaming today yields ZERO latency benefit. That server lives in a
     SEPARATE repo (~/Documents/gemma-realtime-*) we do not edit here.

This tripwire probes the live server and answers ONE question with evidence:
"Is incremental delivery working yet — i.e. would flipping streaming_enabled
actually help?" It is meant to run on a schedule (or by hand) and exit 0 the
day the server-side fix lands, turning a manual vigil into a cron-able signal.

EXIT CODES
----------
  0  streaming BENEFICIAL  — incremental delivery + clean (no Harmony leaks);
                             safe to consider flipping streaming_enabled.
  1  NOT READY             — buffered delivery, or markers leaking through.
  2  SERVER DOWN           — could not reach the server (transport failure).

The pure helpers (classify_delivery / find_harmony_leaks / streaming_verdict)
hold the decision logic and are unit-tested without a live model in
scripts/test_eval_streaming_smoke.py.
"""
import argparse
import json
import re
import sys
import time
import urllib.error
import urllib.request


# A streaming server delivers its first content token well within the first
# half of the generation window. A buffering server's "first" token arrives at
# the very end (first/last ~= 1.0). 0.5 cleanly separates the two regimes.
INCREMENTAL_TTFT_MAX = 0.5

# Harmony markers all open with "<|" — e.g. <|channel|>, <|message|>, <|start|>.
# Match the opening through an optional tag and optional close so both well-
# formed (<|channel|>) and truncated (<|start ...) leaks are caught.
_HARMONY_RE = re.compile(r"<\|[A-Za-z0-9_]*\|?>?")


class ServerDown(Exception):
    """Raised when the streaming server cannot be reached."""


def classify_delivery(chunk_timestamps_ms, content_len):
    """Classify SSE delivery as buffered vs incremental from chunk arrival times.

    chunk_timestamps_ms: wall-clock ms (relative to request start) at which
    each CONTENT chunk arrived. Returns a dict:
      chunk_count  — number of content chunks observed
      incremental  — True iff >=2 chunks AND the first arrived meaningfully
                     before the last (ttft_ratio < INCREMENTAL_TTFT_MAX)
      ttft_ratio   — first_chunk / last_chunk (1.0 when single/zero chunk)
    """
    chunk_count = len(chunk_timestamps_ms)
    if chunk_count == 0:
        return {"chunk_count": 0, "incremental": False, "ttft_ratio": 1.0,
                "content_len": content_len}
    first = chunk_timestamps_ms[0]
    last = chunk_timestamps_ms[-1]
    ttft_ratio = (first / last) if last > 0 else 1.0
    incremental = chunk_count >= 2 and ttft_ratio < INCREMENTAL_TTFT_MAX
    return {"chunk_count": chunk_count, "incremental": incremental,
            "ttft_ratio": ttft_ratio, "content_len": content_len}


def find_harmony_leaks(text):
    """Return the list of raw Harmony marker substrings present in text.

    An empty list means the text is clean (the client-side harmony filter — or
    the server — stripped everything). Any hit means raw thought-channel
    markers escaped into the visible reply.
    """
    if not text:
        return []
    return _HARMONY_RE.findall(text)


def streaming_verdict(delivery, leaks):
    """Combine delivery classification + leak scan into a go/no-go verdict.

    Beneficial ONLY when delivery is incremental AND no Harmony markers leak.
    Buffered, or incremental-but-leaking, both mean "not ready to flip the
    streaming flag yet" (exit 1).
    """
    if leaks:
        return {"streaming_beneficial": False, "exit_code": 1,
                "reason": f"Harmony markers leaking into reply: {leaks[:5]}"}
    if not delivery["incremental"]:
        return {"streaming_beneficial": False, "exit_code": 1,
                "reason": ("server buffers (TTFT==total); single/late-burst "
                           f"chunk delivery, ttft_ratio={delivery['ttft_ratio']:.3f}")}
    return {"streaming_beneficial": True, "exit_code": 0,
            "reason": (f"incremental delivery confirmed: {delivery['chunk_count']} "
                       f"chunks, ttft_ratio={delivery['ttft_ratio']:.3f}, no leaks")}


def probe_stream(server_url, prompt, timeout=120):
    """Probe the live server with one streaming request.

    Returns (chunk_timestamps_ms, full_content): the wall-clock arrival time of
    each CONTENT chunk and the reassembled reply. Raises ServerDown on transport
    failure. Mirrors the SSE wire format proven in eval_multiturn_local.py
    (data: lines, choices[0].delta.content, [DONE] terminator).
    """
    body = json.dumps({
        "model": "default",
        "messages": [{"role": "user", "content": prompt}],
        "temperature": 0.9,
        "stream": True,
        # No max_tokens — see eval_multiturn_local.py LocalBackend.chat docstring.
    }).encode()
    req = urllib.request.Request(
        f"{server_url.rstrip('/')}/v1/chat/completions", data=body,
        headers={"Content-Type": "application/json"})
    t0 = time.time()
    stamps = []
    parts = []
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            for raw_line in resp:
                line = raw_line.decode("utf-8", "replace").strip()
                if not line or not line.startswith("data:"):
                    continue
                payload = line[len("data:"):].strip()
                if payload == "[DONE]":
                    break
                try:
                    chunk = json.loads(payload)
                except ValueError:
                    continue
                try:
                    piece = chunk["choices"][0].get("delta", {}).get("content")
                except (KeyError, IndexError, TypeError):
                    continue
                if piece:
                    stamps.append((time.time() - t0) * 1000.0)
                    parts.append(piece)
    except (OSError, urllib.error.URLError) as e:
        raise ServerDown(f"{server_url}: {e}") from e
    return stamps, "".join(parts)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--server-url", default="http://127.0.0.1:8741",
                    help="gemma-realtime server base URL (default :8741)")
    ap.add_argument("--prompt",
                    default="hey what are you up to tonight? been a long week",
                    help="probe prompt (a casual turn the model should answer fast)")
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="per-request timeout in seconds")
    ap.add_argument("--output-json", default=None,
                    help="write the verdict JSON to this path")
    args = ap.parse_args(argv)

    verdict = {
        "tool": "eval_streaming_smoke",
        "server_url": args.server_url,
        "prompt": args.prompt,
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }
    try:
        stamps, content = probe_stream(args.server_url, args.prompt, args.timeout)
    except ServerDown as e:
        verdict.update({"status": "SERVER_DOWN", "error": str(e),
                        "streaming_beneficial": False, "exit_code": 2})
        _emit(verdict, args.output_json)
        return 2

    delivery = classify_delivery(stamps, content_len=len(content))
    leaks = find_harmony_leaks(content)
    v = streaming_verdict(delivery, leaks)
    verdict.update({
        "status": "OK",
        "delivery": delivery,
        "harmony_leaks": leaks,
        "streaming_beneficial": v["streaming_beneficial"],
        "reason": v["reason"],
        "exit_code": v["exit_code"],
        "content_preview": content[:160],
    })
    _emit(verdict, args.output_json)
    return v["exit_code"]


def _emit(verdict, output_json):
    text = json.dumps(verdict, indent=2)
    print(text)
    if output_json:
        from pathlib import Path
        Path(output_json).write_text(text)


if __name__ == "__main__":
    sys.exit(main())
