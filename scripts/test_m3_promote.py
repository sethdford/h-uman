#!/usr/bin/env python3
"""
Phase G2 verifier — pins m3_promote CLI behavior against a fake MLX.

Tests:
  1. `current` against unreachable URL returns 0 with clear message
  2. `promote --adapter X --dry-run` doesn't actually swap
  3. `promote --adapter X --yes` against fake MLX swaps + records lineage
  4. `rollback --yes` returns to from_adapter recorded in lineage
  5. Production-path heuristic blocks promote without --yes
  6. last_promotion_from_lineage walks the manifest correctly

Run: python3 scripts/test_m3_promote.py
"""
from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PROMOTE = REPO_ROOT / "scripts" / "m3_promote.py"


def _load():
    spec = importlib.util.spec_from_file_location("m3_promote", PROMOTE)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


promote = _load()

_PASS = 0
_FAIL = 0


def _ok(name: str, cond: bool, detail: str = ""):
    global _PASS, _FAIL
    if cond:
        _PASS += 1
        print(f"  PASS  {name}")
    else:
        _FAIL += 1
        print(f"  FAIL  {name}  {detail}")


# ─────────────────────────────────────────────────────────────────────
# Fake MLX server
# ─────────────────────────────────────────────────────────────────────

class FakeMLX(BaseHTTPRequestHandler):
    CURRENT_ADAPTER = ""
    SWAP_HISTORY: list[str] = []

    def do_GET(self):
        if self.path == "/v1/adapters/current":
            body = json.dumps({"adapter_path": FakeMLX.CURRENT_ADAPTER,
                                "tensors_loaded": 42}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        self.send_response(404); self.end_headers()

    def do_POST(self):
        if self.path == "/v1/adapters/swap":
            n = int(self.headers.get("Content-Length", "0"))
            body = json.loads(self.rfile.read(n).decode()) if n else {}
            ap = body.get("adapter_path", "")
            FakeMLX.CURRENT_ADAPTER = ap
            FakeMLX.SWAP_HISTORY.append(ap)
            resp = json.dumps({"status": "ok", "adapter_path": ap,
                                "tensors_loaded": 43}).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(resp)))
            self.end_headers()
            self.wfile.write(resp)
            return
        self.send_response(404); self.end_headers()

    def log_message(self, *_): pass


def serve_fake() -> tuple[HTTPServer, str]:
    srv = HTTPServer(("127.0.0.1", 0), FakeMLX)
    port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, f"http://127.0.0.1:{port}"


def run_cli(home: Path, mlx_url: str, *args) -> subprocess.CompletedProcess:
    env = os.environ.copy()
    env["HOME"] = str(home)
    return subprocess.run(
        [sys.executable, str(PROMOTE), "--mlx-url", mlx_url, *args],
        env=env, capture_output=True, text=True, timeout=30)


# ─────────────────────────────────────────────────────────────────────
# Tests
# ─────────────────────────────────────────────────────────────────────

def test_current_against_unreachable():
    print("\n--- test_current_against_unreachable ---")
    with tempfile.TemporaryDirectory() as d:
        result = run_cli(Path(d), "http://127.0.0.1:1", "current")
        _ok("exits 0 (error reported, not fatal)", result.returncode == 0,
            f"rc={result.returncode}")
        # The exact text varies by OS but should mention server or unreachable
        text = result.stdout + result.stderr
        _ok("output mentions unreachable",
            "unreachable" in text or "ERROR" in text or "no adapter" in text,
            f"got: {text[:200]!r}")


def test_promote_dry_run_no_swap():
    print("\n--- test_promote_dry_run_no_swap ---")
    FakeMLX.CURRENT_ADAPTER = "/initial"
    FakeMLX.SWAP_HISTORY = []
    srv, url = serve_fake()
    try:
        with tempfile.TemporaryDirectory() as d:
            adapter = Path(d) / "new-lora.bin"
            adapter.touch()
            # --evidence is required even for --dry-run: the gate runs before
            # the dry-run short-circuit in m3_promote.py, deliberately. A
            # dry-run that exits 0 where the real promote would exit 2 is a
            # false green, and preview is exactly when an operator is deciding
            # whether they have the evidence. Pass it so this test measures
            # what its name claims — that dry-run does not swap.
            result = run_cli(Path(d), url, "--dry-run",
                              "promote", "--adapter", str(adapter), "--no-prod-check",
                              "--evidence", "blind_ab gate PASS (test fixture)")
            _ok("dry-run exits 0", result.returncode == 0,
                f"rc={result.returncode}\n{result.stdout}\n{result.stderr}")
            _ok("dry-run did NOT swap", len(FakeMLX.SWAP_HISTORY) == 0,
                f"swap history: {FakeMLX.SWAP_HISTORY}")
            # And the gate is not bypassable via --dry-run.
            r2 = run_cli(Path(d), url, "--dry-run",
                         "promote", "--adapter", str(adapter), "--no-prod-check")
            _ok("dry-run still requires evidence", r2.returncode != 0,
                f"rc={r2.returncode}\n{r2.stdout}\n{r2.stderr}")
    finally:
        srv.shutdown()


def test_promote_real_swap_and_lineage():
    print("\n--- test_promote_real_swap_and_lineage ---")
    FakeMLX.CURRENT_ADAPTER = "/old-adapter"
    FakeMLX.SWAP_HISTORY = []
    srv, url = serve_fake()
    try:
        with tempfile.TemporaryDirectory() as d:
            adapter = Path(d) / "new-lora.bin"
            adapter.touch()
            result = run_cli(Path(d), url, "promote", "--adapter", str(adapter),
                              "--yes", "--no-prod-check",
                              "--evidence", "blind_ab gate PASS (test fixture)")
            _ok("promote exits 0", result.returncode == 0,
                f"rc={result.returncode}\n{result.stdout}\n{result.stderr}")
            _ok("swap was executed", FakeMLX.CURRENT_ADAPTER == str(adapter),
                f"current={FakeMLX.CURRENT_ADAPTER}")
            # Lineage manifest should have a "promote" record
            lineage = Path(d) / ".human" / "training-data" / "adapter_lineage.jsonl"
            _ok("lineage manifest written", lineage.exists())
            if lineage.exists():
                lines = [l for l in lineage.read_text().splitlines() if l.strip()]
                last = json.loads(lines[-1])
                _ok("last lineage entry is promote",
                    last.get("action") == "promote")
                _ok("lineage records from_adapter",
                    last.get("from_adapter") == "/old-adapter")
                _ok("lineage records to_adapter",
                    last.get("to_adapter") == str(adapter))
    finally:
        srv.shutdown()


def test_production_heuristic_blocks_without_yes():
    print("\n--- test_production_heuristic_blocks_without_yes ---")
    FakeMLX.CURRENT_ADAPTER = "/initial"
    FakeMLX.SWAP_HISTORY = []
    srv, url = serve_fake()
    try:
        with tempfile.TemporaryDirectory() as d:
            # Adapter path contains "prod" → triggers heuristic
            adapter = Path(d) / "production-lora.bin"
            adapter.touch()
            result = run_cli(Path(d), url, "promote", "--adapter", str(adapter))
            # Without --yes the production-path heuristic should reject
            _ok("blocks production path without --yes", result.returncode == 3,
                f"rc={result.returncode}")
            _ok("no swap occurred", len(FakeMLX.SWAP_HISTORY) == 0)
    finally:
        srv.shutdown()


def test_rollback_reverses_promote():
    print("\n--- test_rollback_reverses_promote ---")
    FakeMLX.CURRENT_ADAPTER = "/original"
    FakeMLX.SWAP_HISTORY = []
    srv, url = serve_fake()
    try:
        with tempfile.TemporaryDirectory() as d:
            home = Path(d)
            new_adapter = home / "new-lora.bin"
            new_adapter.touch()
            # First promote
            r1 = run_cli(home, url, "promote", "--adapter", str(new_adapter),
                          "--yes", "--no-prod-check",
                          "--evidence", "blind_ab gate PASS (test fixture)")
            _ok("promote rc=0", r1.returncode == 0,
                f"{r1.stdout}\n{r1.stderr}")
            # Now rollback
            r2 = run_cli(home, url, "rollback", "--yes")
            _ok("rollback rc=0", r2.returncode == 0,
                f"{r2.stdout}\n{r2.stderr}")
            _ok("server is back to original",
                FakeMLX.CURRENT_ADAPTER == "/original",
                f"current={FakeMLX.CURRENT_ADAPTER}")
    finally:
        srv.shutdown()


def test_last_promotion_from_lineage():
    print("\n--- test_last_promotion_from_lineage ---")
    with tempfile.TemporaryDirectory() as d:
        # Point promote.LINEAGE_PATH at a temp file
        original = promote.LINEAGE_PATH
        promote.LINEAGE_PATH = Path(d) / "lineage.jsonl"
        try:
            # No file → None
            _ok("missing lineage → None",
                promote.last_promotion_from_lineage() is None)
            # Write a non-promote then a promote
            promote.LINEAGE_PATH.write_text(
                json.dumps({"action": "promote", "ok": False,
                            "from_adapter": "/X", "to_adapter": "/Y"}) + "\n" +
                json.dumps({"action": "rollback", "ok": True,
                            "from_adapter": "/Y", "to_adapter": "/X"}) + "\n" +
                json.dumps({"action": "promote", "ok": True,
                            "from_adapter": "/A", "to_adapter": "/B"}) + "\n"
            )
            last = promote.last_promotion_from_lineage()
            _ok("returns most recent OK promote",
                last and last.get("to_adapter") == "/B")
        finally:
            promote.LINEAGE_PATH = original


def test_promote_blocked_without_evidence():
    """The evidence gate itself (e922b887b). Nothing covered this contract, so
    when promote started requiring --evidence the only signal was six unrelated
    assertions going red in the happy-path tests — which reads as "promote is
    broken", not "promote grew a gate". Assert the gate directly: refusal must
    be a non-zero exit AND no swap, so a regression that merely warns and
    proceeds still fails here."""
    print("\n--- test_promote_blocked_without_evidence ---")
    FakeMLX.CURRENT_ADAPTER = "/original"
    FakeMLX.SWAP_HISTORY = []
    srv, url = serve_fake()
    try:
        with tempfile.TemporaryDirectory() as d:
            adapter = Path(d) / "new-lora.bin"
            adapter.touch()
            r = run_cli(Path(d), url, "promote", "--adapter", str(adapter),
                        "--yes", "--no-prod-check")
            _ok("promote without --evidence exits non-zero", r.returncode != 0,
                f"rc={r.returncode}\n{r.stdout}\n{r.stderr}")
            _ok("refused promote did NOT swap", len(FakeMLX.SWAP_HISTORY) == 0,
                f"swap history: {FakeMLX.SWAP_HISTORY}")
            _ok("server still on original adapter",
                FakeMLX.CURRENT_ADAPTER == "/original",
                f"current={FakeMLX.CURRENT_ADAPTER}")
            # The documented override must still work — otherwise the gate is a
            # wall, not a gate, and operators will reach for --no-prod-check.
            r2 = run_cli(Path(d), url, "promote", "--adapter", str(adapter),
                         "--yes", "--no-prod-check", "--force-no-evidence")
            _ok("--force-no-evidence override promotes", r2.returncode == 0,
                f"rc={r2.returncode}\n{r2.stdout}\n{r2.stderr}")
            _ok("override actually swapped",
                FakeMLX.CURRENT_ADAPTER == str(adapter),
                f"current={FakeMLX.CURRENT_ADAPTER}")
    finally:
        srv.shutdown()


def main():
    print("M3 promote CLI (G2) verifier")
    test_current_against_unreachable()
    test_promote_dry_run_no_swap()
    test_promote_real_swap_and_lineage()
    test_production_heuristic_blocks_without_yes()
    test_promote_blocked_without_evidence()
    test_rollback_reverses_promote()
    test_last_promotion_from_lineage()
    print(f"\n--- Results: {_PASS} passed, {_FAIL} failed ---")
    return 0 if _FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
