#!/usr/bin/env python3
"""
Phase G3 verifier — pins the canary live-fire's full lifecycle
against a fake MLX server. Tests:

  1. Captures current adapter before swap
  2. Swaps to candidate
  3. Chat completion fires
  4. Rollback restores original adapter (trap fires unconditionally)
  5. Refuses to run without --i-know-this-touches-production

Run: python3 scripts/test_m3_canary.py
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
CANARY = REPO_ROOT / "scripts" / "live_fire_m3_canary.sh"


class FakeMLX(BaseHTTPRequestHandler):
    CURRENT = "/original-adapter"
    SWAPS: list[str] = []
    CHATS = 0

    def do_GET(self):
        if self.path == "/v1/adapters/current":
            self._json(200, {"adapter_path": FakeMLX.CURRENT, "tensors_loaded": 42})
            return
        self.send_response(404); self.end_headers()

    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(n).decode()) if n else {}
        if self.path == "/v1/adapters/swap":
            ap = body.get("adapter_path", "")
            FakeMLX.CURRENT = ap
            FakeMLX.SWAPS.append(ap)
            self._json(200, {"status": "ok", "adapter_path": ap, "tensors_loaded": 43})
            return
        if self.path == "/v1/chat/completions":
            FakeMLX.CHATS += 1
            self._json(200, {
                "id": "chatcmpl-fake",
                "choices": [{"index": 0,
                              "message": {"role": "assistant",
                                          "content": "hi from " + FakeMLX.CURRENT},
                              "finish_reason": "stop"}],
                "usage": {"prompt_tokens": 5, "completion_tokens": 5, "total_tokens": 10},
            })
            return
        self.send_response(404); self.end_headers()

    def _json(self, status, payload):
        body = json.dumps(payload).encode()
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *_): pass


def serve_fake():
    srv = HTTPServer(("127.0.0.1", 0), FakeMLX)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, f"http://127.0.0.1:{port}"


_PASS = 0
_FAIL = 0


def _ok(name, cond, detail=""):
    global _PASS, _FAIL
    if cond:
        _PASS += 1
        print(f"  PASS  {name}")
    else:
        _FAIL += 1
        print(f"  FAIL  {name}  {detail}")


def test_canary_full_lifecycle():
    print("\n--- test_canary_full_lifecycle ---")
    FakeMLX.CURRENT = "/original-adapter"
    FakeMLX.SWAPS = []
    FakeMLX.CHATS = 0
    srv, url = serve_fake()
    try:
        result = subprocess.run(
            ["bash", str(CANARY),
             "--mlx-url", url,
             "--candidate", "/test-candidate",
             "--i-know-this-touches-production"],
            capture_output=True, text=True, timeout=30)
        _ok(f"canary exits 0 (rc={result.returncode})",
            result.returncode == 0,
            f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
        # Two swaps expected: 1) to candidate, 2) rollback to original
        _ok(f"exactly 2 swaps occurred (got {len(FakeMLX.SWAPS)})",
            len(FakeMLX.SWAPS) == 2,
            f"swaps: {FakeMLX.SWAPS}")
        _ok("first swap was to candidate",
            len(FakeMLX.SWAPS) >= 1 and FakeMLX.SWAPS[0] == "/test-candidate")
        _ok("second swap was rollback to original",
            len(FakeMLX.SWAPS) >= 2 and FakeMLX.SWAPS[1] == "/original-adapter")
        _ok("server is restored to original",
            FakeMLX.CURRENT == "/original-adapter")
        _ok("exactly one chat completion fired",
            FakeMLX.CHATS == 1)
    finally:
        srv.shutdown()


def test_canary_requires_confirmation():
    print("\n--- test_canary_requires_confirmation ---")
    FakeMLX.SWAPS = []
    srv, url = serve_fake()
    try:
        result = subprocess.run(
            ["bash", str(CANARY),
             "--mlx-url", url,
             "--candidate", "/X"],
            capture_output=True, text=True, timeout=10)
        _ok("exits 3 without --i-know-this-touches-production",
            result.returncode == 3,
            f"rc={result.returncode}")
        _ok("no swap occurred", len(FakeMLX.SWAPS) == 0,
            f"swaps: {FakeMLX.SWAPS}")
    finally:
        srv.shutdown()


def test_canary_requires_candidate():
    print("\n--- test_canary_requires_candidate ---")
    srv, url = serve_fake()
    try:
        result = subprocess.run(
            ["bash", str(CANARY),
             "--mlx-url", url,
             "--i-know-this-touches-production"],
            capture_output=True, text=True, timeout=10)
        _ok("exits 2 without --candidate",
            result.returncode == 2,
            f"rc={result.returncode}")
    finally:
        srv.shutdown()


def main():
    print("M3 canary live-fire (G3) verifier")
    test_canary_full_lifecycle()
    test_canary_requires_confirmation()
    test_canary_requires_candidate()
    print(f"\n--- Results: {_PASS} passed, {_FAIL} failed ---")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
