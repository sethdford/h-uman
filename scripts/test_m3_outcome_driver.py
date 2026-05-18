#!/usr/bin/env python3
"""
End-to-end test for the M3 outcome driver — closes the B3 v1 loop in a
single deterministic process.

Wires:
    fake gateway (HTTP server in a thread, serves NDJSON outcomes)
      ↓
    m3_outcome_driver.py (in --simulate-train mode)
      ↓
    fake MLX server (HTTP server in a thread, serves /v1/adapters/swap)
      ↓
    assert end state: JSONL has filtered outcomes, adapter file exists,
                      MLX server saw the swap request

Why a self-contained test:
  - The real daemon's ring is unpredictable (depends on prior traffic).
  - The real MLX server may not be running.
  - We want to PIN the selection policy + dedup + threshold + swap-call
    behavior in CI, not just smoke-test it manually.

Run:
    python3 scripts/test_m3_outcome_driver.py

Exit codes:
    0 — all assertions passed
    1 — at least one failure (printed inline)
"""
from __future__ import annotations

import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
DRIVER = REPO_ROOT / "scripts" / "m3_outcome_driver.py"

_PASS = 0
_FAIL = 0


def _ok(name: str, cond: bool, detail: str = "") -> None:
    global _PASS, _FAIL
    if cond:
        _PASS += 1
        print(f"  PASS  {name}")
    else:
        _FAIL += 1
        print(f"  FAIL  {name}  {detail}")


# ─────────────────────────────────────────────────────────────────────
# Fixture outcomes — designed to exercise every filter in the policy
# ─────────────────────────────────────────────────────────────────────

def fixture_outcomes() -> list[dict]:
    """12 outcomes covering every filter the selection policy applies.
    Order matters: server returns oldest-first.
    """
    base_t = 1_700_000_000_000
    return [
        # 0 — accept: PASS guard, base adapter, normal latency, real tokens
        {"t": base_t + 0,  "l": 250,  "ph": 100, "rh": 1100, "ch": 5001,
         "pt": 32, "ct": 64,  "m": 1, "a": 0, "g": 1, "k": 1},
        # 1 — accept: another PASS, different contact
        {"t": base_t + 1,  "l": 400,  "ph": 101, "rh": 1101, "ch": 5002,
         "pt": 40, "ct": 80,  "m": 1, "a": 0, "g": 1, "k": 1},
        # 2 — reject: guard REWRITE
        {"t": base_t + 2,  "l": 300,  "ph": 102, "rh": 1102, "ch": 5001,
         "pt": 32, "ct": 64,  "m": 1, "a": 0, "g": 2, "k": 1},
        # 3 — reject: guard REJECT
        {"t": base_t + 3,  "l": 300,  "ph": 103, "rh": 1103, "ch": 5001,
         "pt": 32, "ct": 64,  "m": 1, "a": 0, "g": 3, "k": 1},
        # 4 — reject: adapter_id != 0 (already-fine-tuned, feedback risk)
        {"t": base_t + 4,  "l": 300,  "ph": 104, "rh": 1104, "ch": 5001,
         "pt": 32, "ct": 64,  "m": 1, "a": 7, "g": 1, "k": 1},
        # 5 — reject: latency below MIN_LATENCY_MS (cached/stub)
        {"t": base_t + 5,  "l": 10,   "ph": 105, "rh": 1105, "ch": 5001,
         "pt": 32, "ct": 64,  "m": 1, "a": 0, "g": 1, "k": 1},
        # 6 — reject: latency above MAX_LATENCY_MS (cold start)
        {"t": base_t + 6,  "l": 60_000, "ph": 106, "rh": 1106, "ch": 5001,
         "pt": 32, "ct": 64,  "m": 1, "a": 0, "g": 1, "k": 1},
        # 7 — reject: zero prompt tokens (degenerate turn)
        {"t": base_t + 7,  "l": 300,  "ph": 107, "rh": 1107, "ch": 5001,
         "pt": 0,  "ct": 64,  "m": 1, "a": 0, "g": 1, "k": 1},
        # 8 — reject: zero completion tokens (degenerate turn)
        {"t": base_t + 8,  "l": 300,  "ph": 108, "rh": 1108, "ch": 5001,
         "pt": 32, "ct": 0,   "m": 1, "a": 0, "g": 1, "k": 1},
        # 9 — accept: third PASS, returning to contact 5001
        {"t": base_t + 9,  "l": 500,  "ph": 109, "rh": 1109, "ch": 5001,
         "pt": 50, "ct": 100, "m": 1, "a": 0, "g": 1, "k": 1},
        # 10 — DUPLICATE prompt_hash of #0 — driver's dedup should drop this
        {"t": base_t + 10, "l": 250,  "ph": 100, "rh": 9999, "ch": 5003,
         "pt": 32, "ct": 64,  "m": 1, "a": 0, "g": 1, "k": 1},
        # 11 — accept: new prompt, new contact
        {"t": base_t + 11, "l": 600,  "ph": 111, "rh": 1111, "ch": 5004,
         "pt": 64, "ct": 128, "m": 1, "a": 0, "g": 1, "k": 1},
    ]


# Out of the 12 fixtures:
#   - selection accepts: 0, 1, 9, 11 (four — PASS+base+latency-ok+tokens-ok)
#   - selection rejects: 2,3 (guard), 4 (adapter), 5,6 (latency), 7,8 (tokens)
#   - dedup drops: 10 (duplicate prompt_hash of 0) — but it never reaches dedup
#     because the policy would have accepted it; we want to prove dedup works
#     INDEPENDENTLY of the policy. We test that path below.
EXPECTED_ACCEPTED_PHS = {100, 101, 109, 111}


# ─────────────────────────────────────────────────────────────────────
# Fake gateway server — serves /v1/m3/outcomes
# ─────────────────────────────────────────────────────────────────────

class FakeGateway(BaseHTTPRequestHandler):
    OUTCOMES: list[dict] = []

    def do_GET(self):
        if not self.path.startswith("/v1/m3/outcomes"):
            self.send_response(404); self.end_headers(); return
        # Emit oldest-first NDJSON
        body = "\n".join(json.dumps(o) for o in self.OUTCOMES)
        if body:
            body += "\n"
        data = body.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/x-ndjson")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, *_): pass  # quiet


# ─────────────────────────────────────────────────────────────────────
# Fake MLX server — records swap requests
# ─────────────────────────────────────────────────────────────────────

class FakeMLX(BaseHTTPRequestHandler):
    SWAPS: list[dict] = []

    def do_POST(self):
        if self.path != "/v1/adapters/swap":
            self.send_response(404); self.end_headers(); return
        n = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(n).decode("utf-8")) if n else {}
        self.SWAPS.append(body)
        resp = json.dumps({"status": "ok",
                           "adapter_path": body.get("adapter_path", ""),
                           "tensors_loaded": 42}).encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(resp)))
        self.end_headers()
        self.wfile.write(resp)

    def log_message(self, *_): pass


def serve(handler, outcomes=None) -> tuple[HTTPServer, str]:
    if outcomes is not None:
        handler.OUTCOMES = outcomes
    srv = HTTPServer(("127.0.0.1", 0), handler)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, f"http://127.0.0.1:{port}"


# ─────────────────────────────────────────────────────────────────────
# Test cases
# ─────────────────────────────────────────────────────────────────────

def run_driver(env_overrides: dict, *cli_args) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    env.update(env_overrides)
    return subprocess.run(
        [sys.executable, str(DRIVER), *cli_args],
        env=env, capture_output=True, text=True, timeout=30)


def test_full_loop_with_simulate_train(tmpdir: Path):
    print("\n--- test_full_loop_with_simulate_train ---")
    gateway_srv, gateway_url = serve(FakeGateway, fixture_outcomes())
    mlx_srv, mlx_url = serve(FakeMLX)
    FakeMLX.SWAPS = []
    try:
        # Threshold=4 — exactly the number we expect the policy to accept.
        # Lets us verify the trigger fires when the JSONL reaches the bar.
        env = {
            "HOME": str(tmpdir),
            "HUMAN_GATEWAY_URL": gateway_url,
            "HUMAN_MLX_URL": mlx_url,
        }
        result = run_driver(env, "--since", "0", "--run-loop",
                            "--threshold", "4", "--simulate-train",
                            "--mlx-url", mlx_url)
        _ok("driver exit 0", result.returncode == 0,
            f"rc={result.returncode} stderr={result.stderr!r}")

        # 1. JSONL should hold exactly the accepted outcomes, one per line.
        jsonl = tmpdir / ".human" / "training-data" / "m3-outcomes.jsonl"
        _ok("JSONL exists", jsonl.exists(), f"{jsonl}")
        lines = [l for l in jsonl.read_text().splitlines() if l.strip()]
        _ok(f"JSONL has 4 lines (got {len(lines)})", len(lines) == 4)

        accepted_phs = {json.loads(l)["ph"] for l in lines}
        _ok(f"accepted prompt_hashes = {EXPECTED_ACCEPTED_PHS}",
            accepted_phs == EXPECTED_ACCEPTED_PHS,
            f"got {accepted_phs}")

        # 2. Adapter artifact should exist (simulate-train produced it).
        adapters = list((tmpdir / ".human" / "training-data" / "adapters").glob("*.safetensors"))
        _ok("exactly 1 adapter written", len(adapters) == 1,
            f"got {[str(p) for p in adapters]}")

        # 3. MLX swap was called with the right path.
        _ok("MLX server got 1 swap request", len(FakeMLX.SWAPS) == 1,
            f"got {FakeMLX.SWAPS}")
        if FakeMLX.SWAPS:
            swapped_path = FakeMLX.SWAPS[0].get("adapter_path", "")
            _ok("swap path matches adapter file",
                adapters and swapped_path == str(adapters[0]),
                f"swap={swapped_path!r} adapter={adapters[0] if adapters else None}")

        # 4. State file should have advanced the watermark.
        state_path = tmpdir / ".human" / "m3_driver_state.json"
        _ok("state file written", state_path.exists())
        state = json.loads(state_path.read_text())
        _ok("watermark advanced to newest outcome ts",
            state.get("last_ts_ms") == 1_700_000_000_011,
            f"got {state.get('last_ts_ms')}")
    finally:
        gateway_srv.shutdown()
        mlx_srv.shutdown()


def test_below_threshold_skips_train(tmpdir: Path):
    print("\n--- test_below_threshold_skips_train ---")
    # Only first two fixtures → 2 acceptances → below threshold of 10
    gateway_srv, gateway_url = serve(FakeGateway, fixture_outcomes()[:2])
    mlx_srv, mlx_url = serve(FakeMLX)
    FakeMLX.SWAPS = []
    try:
        env = {"HOME": str(tmpdir), "HUMAN_GATEWAY_URL": gateway_url}
        result = run_driver(env, "--since", "0", "--run-loop",
                            "--threshold", "10", "--simulate-train",
                            "--mlx-url", mlx_url)
        _ok("driver exit 0 (below threshold)", result.returncode == 0,
            f"rc={result.returncode}")

        _ok("no swap requests when below threshold", len(FakeMLX.SWAPS) == 0,
            f"got {FakeMLX.SWAPS}")

        # JSONL still got the 2 outcomes — driver writes regardless of trigger.
        jsonl = tmpdir / ".human" / "training-data" / "m3-outcomes.jsonl"
        lines = [l for l in jsonl.read_text().splitlines() if l.strip()]
        _ok("JSONL holds 2 lines even when train skipped", len(lines) == 2)
    finally:
        gateway_srv.shutdown()
        mlx_srv.shutdown()


def test_dedup_across_runs(tmpdir: Path):
    print("\n--- test_dedup_across_runs ---")
    # Two runs with same fixtures: second run must dedup all of them.
    gateway_srv, gateway_url = serve(FakeGateway, fixture_outcomes())
    mlx_srv, mlx_url = serve(FakeMLX)
    try:
        env = {"HOME": str(tmpdir), "HUMAN_GATEWAY_URL": gateway_url}
        result1 = run_driver(env, "--since", "0", "--mlx-url", mlx_url)
        _ok("first run rc=0", result1.returncode == 0)

        jsonl = tmpdir / ".human" / "training-data" / "m3-outcomes.jsonl"
        first_lines = [l for l in jsonl.read_text().splitlines() if l.strip()]

        # Second run — same fixtures, no --since override so watermark applies
        result2 = run_driver(env, "--mlx-url", mlx_url)
        _ok("second run rc=0", result2.returncode == 0)
        second_lines = [l for l in jsonl.read_text().splitlines() if l.strip()]

        _ok(f"second run added no new lines ({len(second_lines)}=={len(first_lines)})",
            len(second_lines) == len(first_lines))

        # Force a backfill with --since 0 — every prompt is now in seen_hashes
        # so dedup should drop ALL of them on the third run.
        result3 = run_driver(env, "--since", "0", "--mlx-url", mlx_url)
        _ok("third run rc=0", result3.returncode == 0)
        third_lines = [l for l in jsonl.read_text().splitlines() if l.strip()]
        _ok(f"backfill after seen-set populated does not duplicate "
            f"({len(third_lines)}=={len(first_lines)})",
            len(third_lines) == len(first_lines))
    finally:
        gateway_srv.shutdown()
        mlx_srv.shutdown()


def test_mlx_offline_is_soft_failure(tmpdir: Path):
    print("\n--- test_mlx_offline_is_soft_failure ---")
    # Gateway up, MLX server NOT started. Driver should produce adapter
    # but report swap-failed and still exit 0 (producer's job is done).
    gateway_srv, gateway_url = serve(FakeGateway, fixture_outcomes())
    try:
        env = {"HOME": str(tmpdir), "HUMAN_GATEWAY_URL": gateway_url}
        result = run_driver(env, "--since", "0", "--run-loop",
                            "--threshold", "4", "--simulate-train",
                            "--mlx-url", "http://127.0.0.1:1")  # unreachable
        _ok("driver exits 0 even when MLX unreachable",
            result.returncode == 0,
            f"rc={result.returncode}")
        _ok("output mentions swap-skipped/failed",
            "skipped/failed" in result.stdout or "unreachable" in result.stdout,
            f"stdout={result.stdout!r}")

        # Adapter still gets written.
        adapters = list((tmpdir / ".human" / "training-data" / "adapters").glob("*.safetensors"))
        _ok("adapter still written when MLX offline",
            len(adapters) == 1, f"got {[str(p) for p in adapters]}")
    finally:
        gateway_srv.shutdown()


def main():
    print("M3 outcome driver e2e tests")

    for test_fn in [test_full_loop_with_simulate_train,
                    test_below_threshold_skips_train,
                    test_dedup_across_runs,
                    test_mlx_offline_is_soft_failure]:
        with tempfile.TemporaryDirectory() as d:
            test_fn(Path(d))

    print(f"\n--- Results: {_PASS} passed, {_FAIL} failed ---")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
