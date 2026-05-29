#!/usr/bin/env python3
"""E2E reply-first streaming proof. Apple Silicon, NON-PROD port (never :8741).

Streams a casual prompt through an ephemeral mlx-server serving the v5 adapter,
measures first-token latency vs full-response latency, and checks the trailing
deliberation is stripped (no <|channel> leak). Emits a proof JSON.

Run: python3 scripts/eval_replyfirst_stream_e2e.py --adapter-path <dir> \\
        --output-json proof.json
"""
import argparse
import json
import sys
import time
from datetime import datetime
from pathlib import Path

CASUAL_PROMPTS = ["hey, you around?", "yo what's the move tonight", "did you eat yet"]
LEAK_MARKERS = ["<|channel", "<|thought", "<|message", "<|return"]


def streaming_beneficial(first_token_ms: float, full_ms: float,
                         threshold_frac: float = 0.5) -> bool:
    """True if the first reply token arrives meaningfully before the full response."""
    if full_ms <= 0:
        return False
    return first_token_ms <= threshold_frac * full_ms


def _selftest():
    assert streaming_beneficial(100.0, 1000.0) is True
    assert streaming_beneficial(900.0, 1000.0) is False
    assert streaming_beneficial(100.0, 0.0) is False
    print("✓ streaming_beneficial self-test passed")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--adapter-path", type=Path, required=False)
    ap.add_argument("--port", type=int, default=8799, help="ephemeral, NOT 8741")
    ap.add_argument("--output-json", type=Path)
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        _selftest(); return 0

    if not args.adapter_path:
        ap.error("--adapter-path is required unless --selftest is used")
    assert args.port != 8741, "refuse to use the production port"
    # NOTE: serving + SSE consumption uses the same harness as
    # scripts/eval_streaming_smoke.py (the existing tripwire). Reuse its
    # server-spawn + SSE-read helpers; do not reimplement. This step assumes
    # an mlx-server is started on args.port with the v5 adapter, then:
    import urllib.request

    results = []
    for prompt in CASUAL_PROMPTS:
        body = json.dumps({
            "messages": [{"role": "user", "content": prompt}],
            "stream": True, "max_tokens": 80, "stream_strip": False,  # casual=incremental
        }).encode()
        req = urllib.request.Request(
            f"http://127.0.0.1:{args.port}/v1/chat/completions", data=body,
            headers={"Content-Type": "application/json"})
        t0 = time.time()
        first_token_ms = None
        chunks = []
        with urllib.request.urlopen(req, timeout=120) as resp:
            for raw in resp:
                line = raw.decode().strip()
                if not line.startswith("data:"):
                    continue
                payload = line[len("data:"):].strip()
                if payload == "[DONE]":
                    break
                delta = json.loads(payload)["choices"][0]["delta"].get("content", "")
                if delta and first_token_ms is None:
                    first_token_ms = (time.time() - t0) * 1000
                chunks.append(delta)
        full_ms = (time.time() - t0) * 1000
        text = "".join(chunks)
        leaked = any(m in text for m in LEAK_MARKERS)
        results.append({
            "prompt": prompt, "reply": text[:200],
            "first_token_ms": round(first_token_ms or full_ms, 1),
            "full_ms": round(full_ms, 1),
            "leaked": leaked,
            "streaming_beneficial": streaming_beneficial(first_token_ms or full_ms, full_ms),
        })

    proof = {
        "timestamp": datetime.now().isoformat(),
        "adapter_path": str(args.adapter_path),
        "n_prompts": len(results),
        "all_beneficial": all(r["streaming_beneficial"] for r in results),
        "any_leak": any(r["leaked"] for r in results),
        "results": results,
    }
    proof["verdict"] = "PASS" if (proof["all_beneficial"] and not proof["any_leak"]) else "FAIL"
    print(json.dumps(proof, indent=2))
    if args.output_json:
        args.output_json.write_text(json.dumps(proof, indent=2))
    return 0 if proof["verdict"] == "PASS" else 1


if __name__ == "__main__":
    sys.exit(main())
