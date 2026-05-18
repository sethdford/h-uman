#!/usr/bin/env python3
"""
Stub MLX server — satisfies the production chat contract WITHOUT loading
gemma-4-31b. Used to live-fire the M3 outcome loop end-to-end on machines
where the real MLX model isn't on disk.

What it serves (only the routes the daemon actually calls):
    GET  /health                  → {"ok": true}
    GET  /v1/adapters/current     → {"adapter_path": "...", "tensors_loaded": N}
    POST /v1/adapters/swap        → {"status": "ok", "adapter_path": "...", "tensors_loaded": N}
    POST /v1/chat/completions     → OpenAI-shaped non-streaming completion

Why this script exists (and why it's not theatre):
    The daemon's production code path calls hu_chat_complete → curl POST to
    127.0.0.1:8741 → parses the OpenAI-shaped JSON response → invokes
    hu_agent_m3_record_chat_outcome on success. Everything between curl POST
    and the record_chat_outcome call IS production code. The only piece this
    stub replaces is the model itself. So a live run against this stub
    exercises every line of the producer chain we want to verify.

Determinism:
    Response text is a fixed pool of short Seth-style answers, picked by
    request hash. That makes outcomes reproducible (same prompt → same
    response → same response_hash) for debug + dedup verification.

Run:
    python3 scripts/stub_mlx_server.py --port 8741 --adapter ~/.human/training-data/adapters/seed
    # then in another terminal: start the daemon (which will hit :8741)
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

CURRENT_ADAPTER = ""   # mutated by /v1/adapters/swap
TENSORS_LOADED = 42    # cosmetic — the daemon doesn't read this

# Six neutral Seth-style answers; the request hash picks one. Keeps the
# stub deterministic without making it boring.
RESPONSES = [
    "lol yeah that's a good one",
    "hmm let me think about it",
    "haha — depends what you're after",
    "yeah, fair point",
    "could go either way honestly",
    "good question — i'd lean toward the simpler one",
]


def pick_response(prompt: str) -> str:
    """Deterministic pick: hash prompt, mod by RESPONSES length."""
    h = hashlib.sha256(prompt.encode("utf-8")).digest()
    return RESPONSES[h[0] % len(RESPONSES)]


def estimate_tokens(text: str) -> int:
    """Same bytes/4 heuristic the C side uses for prompt_tokens estimate.
    Matters because: the daemon's chat path reads `usage.completion_tokens`
    in some routes; making this estimate match the C side keeps the two
    in lockstep when the real provider response includes a `usage` block.
    """
    return max(1, len(text.encode("utf-8")) // 4)


class StubHandler(BaseHTTPRequestHandler):
    def _send_json(self, status: int, payload: dict):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        global CURRENT_ADAPTER, TENSORS_LOADED
        if self.path == "/health":
            return self._send_json(200, {
                "ok": True,
                "active_adapter": CURRENT_ADAPTER,
                "model_loaded": True,
            })
        if self.path == "/v1/adapters/current":
            return self._send_json(200, {
                "adapter_path": CURRENT_ADAPTER,
                "tensors_loaded": TENSORS_LOADED,
            })
        return self._send_json(404, {"error": f"no route: {self.path}"})

    def do_POST(self):
        global CURRENT_ADAPTER, TENSORS_LOADED
        n = int(self.headers.get("Content-Length", "0"))
        raw = self.rfile.read(n).decode("utf-8") if n else "{}"
        try:
            body = json.loads(raw)
        except json.JSONDecodeError:
            return self._send_json(400, {"error": "invalid JSON"})

        if self.path == "/v1/adapters/swap":
            ap = body.get("adapter_path")
            if not ap or not isinstance(ap, str):
                return self._send_json(400, {"error": "missing adapter_path"})
            CURRENT_ADAPTER = ap
            TENSORS_LOADED += 1
            print(f"[stub-mlx] swap → {ap}", flush=True)
            return self._send_json(200, {
                "status": "ok",
                "adapter_path": CURRENT_ADAPTER,
                "tensors_loaded": TENSORS_LOADED,
            })

        if self.path == "/v1/chat/completions":
            # Pull last user message — that's what the model "responds to".
            messages = body.get("messages", [])
            last_user = ""
            for m in reversed(messages):
                if m.get("role") == "user":
                    last_user = m.get("content", "")
                    break
            # Treat entire system+user concat as prompt for hashing
            full_prompt = json.dumps(messages, sort_keys=True)
            answer = pick_response(full_prompt)
            # Simulate realistic inference latency. Without this delay the
            # daemon records outcomes with latency_ms ≤ 2, which the M3
            # driver's selection policy filters out (sub-50ms = cached
            # path heuristic). Real gemma-4-31b takes 100-500ms for short
            # completions; 150ms lands solidly inside the [50, 30000]
            # acceptance window.
            time.sleep(0.15)

            prompt_tokens = estimate_tokens(full_prompt)
            completion_tokens = estimate_tokens(answer)

            response = {
                "id": f"chatcmpl-stub-{int(time.time()*1000)}",
                "object": "chat.completion",
                "created": int(time.time()),
                "model": body.get("model", "stub-mlx"),
                "choices": [{
                    "index": 0,
                    "message": {"role": "assistant", "content": answer},
                    "finish_reason": "stop",
                }],
                "usage": {
                    "prompt_tokens": prompt_tokens,
                    "completion_tokens": completion_tokens,
                    "total_tokens": prompt_tokens + completion_tokens,
                },
            }
            preview = last_user[:60].replace("\n", " ")
            print(f"[stub-mlx] chat ← {preview!r:64s} → {answer!r}", flush=True)
            return self._send_json(200, response)

        return self._send_json(404, {"error": f"no route: {self.path}"})

    def log_message(self, *_):
        pass  # we print our own concise lines above


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=int, default=8741)
    ap.add_argument("--adapter", default="", help="Initial adapter path")
    args = ap.parse_args()

    global CURRENT_ADAPTER
    CURRENT_ADAPTER = args.adapter

    srv = ThreadingHTTPServer(("127.0.0.1", args.port), StubHandler)
    print(f"[stub-mlx] listening on http://127.0.0.1:{args.port}")
    print(f"[stub-mlx] initial adapter: {CURRENT_ADAPTER!r}")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        print("[stub-mlx] shutting down")


if __name__ == "__main__":
    sys.exit(main() or 0)
