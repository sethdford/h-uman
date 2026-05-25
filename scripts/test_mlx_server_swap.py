#!/usr/bin/env python3
"""
Spec 2026-05-19 M3 closure / AC-M3-1 — regression test for the inline
/v1/adapters/swap endpoint that scripts/mlx-server.py now implements.

This test is self-hosting: it starts the inline server in a subprocess,
hits the endpoint, verifies the documented contract, then tears down.
No real MLX model is required — the server runs in stub mode when
mlx_lm isn't installed.

Run:
    python3 scripts/test_mlx_server_swap.py

Exit codes:
    0 - all assertions passed
    1 - one or more assertions failed
"""
from __future__ import annotations

import json
import os
import socket
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path


_PASSED = 0
_FAILED = 0


def _pick_free_port() -> int:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    return port


def _http(server: str, method: str, path: str, body=None, timeout: int = 5):
    url = server + path
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            payload_bytes = resp.read()
            try:
                payload = json.loads(payload_bytes.decode("utf-8"))
            except (ValueError, json.JSONDecodeError):
                payload = {"_raw": payload_bytes.decode("utf-8", errors="replace")}
            return resp.status, payload
    except urllib.error.HTTPError as e:
        try:
            payload = json.loads(e.read().decode("utf-8"))
        except (ValueError, json.JSONDecodeError):
            payload = {"error": "non-json-error-body"}
        return e.code, payload


def _ok(name: str, cond: bool, detail: str = ""):
    global _PASSED, _FAILED
    if cond:
        _PASSED += 1
        print(f"  PASS  {name}")
    else:
        _FAILED += 1
        print(f"  FAIL  {name}  {detail}")


def _http_sse(server: str, path: str, body, timeout: int = 5):
    """Phase 3a — read an SSE stream and return (status, content_type,
    [parsed_chunk_dicts], done_seen).

    Each `data: {...}` line is JSON-parsed; the `data: [DONE]` sentinel
    is recorded separately. Non-data lines are ignored (per the SSE
    spec). The function returns once the body closes or DONE is seen.
    """
    data = json.dumps(body).encode("utf-8")
    req = urllib.request.Request(
        server + path,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    chunks = []
    done = False
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        status = resp.status
        content_type = resp.headers.get("Content-Type", "")
        for raw in resp:
            line = raw.decode("utf-8", errors="replace").rstrip("\r\n")
            if not line.startswith("data: "):
                continue
            payload = line[len("data: "):]
            if payload == "[DONE]":
                done = True
                break
            try:
                chunks.append(json.loads(payload))
            except json.JSONDecodeError:
                # Non-JSON data lines violate our contract; record raw
                # for the assertion to surface.
                chunks.append({"_raw": payload})
    return status, content_type, chunks, done


def _wait_for_server(server: str, timeout: int = 10) -> bool:
    deadline = time.time() + timeout
    while time.time() < deadline:
        try:
            with urllib.request.urlopen(server + "/health", timeout=1) as r:
                if r.status == 200:
                    return True
        except (urllib.error.URLError, OSError):
            pass
        time.sleep(0.1)
    return False


def _make_fake_adapter(dest_dir: Path) -> Path:
    """Build a minimal safetensors-shaped file with N>0 tensors recorded
    in the header (no real weights though — header-only)."""
    dest_dir.mkdir(parents=True, exist_ok=True)
    adapter_file = dest_dir / "adapters.safetensors"

    import struct
    # Two cosmetic tensor entries + metadata. We are not loading these
    # via mlx_lm here — the test runs the server in stub mode, which
    # only counts header entries.
    header = {
        "lora_a.0": {
            "dtype": "F32", "shape": [16, 16], "data_offsets": [0, 1024],
        },
        "lora_b.0": {
            "dtype": "F32", "shape": [16, 16], "data_offsets": [1024, 2048],
        },
        "__metadata__": {
            "format": "test-fixture",
            "produced_by": "scripts/test_mlx_server_swap.py",
        },
    }
    header_bytes = json.dumps(header).encode("utf-8")
    body = b"\0" * 2048  # cosmetic — matches the data_offsets we declared
    with open(adapter_file, "wb") as f:
        f.write(struct.pack("<Q", len(header_bytes)))
        f.write(header_bytes)
        f.write(body)
    return adapter_file


def main() -> int:
    port = _pick_free_port()
    server_url = f"http://127.0.0.1:{port}"
    print(f"Spec M3 / AC-M3-1 — inline swap endpoint test (port={port})")

    repo_root = Path(__file__).resolve().parent.parent
    server_script = repo_root / "scripts" / "mlx-server.py"
    if not server_script.exists():
        print(f"  FAIL  server script missing: {server_script}")
        return 1

    with tempfile.TemporaryDirectory() as tmpd:
        tmpdir = Path(tmpd)
        adapter_dir = tmpdir / "adapter1"
        _make_fake_adapter(adapter_dir)
        adapter2_dir = tmpdir / "adapter2"
        _make_fake_adapter(adapter2_dir)

        # Boot the server. Use --no-upstream so we always test the
        # inline path even if the operator has gemma-realtime checked out.
        env = os.environ.copy()
        env.setdefault("HUMAN_MLX_MODEL", "")  # skip real model load
        proc = subprocess.Popen(
            [sys.executable, str(server_script),
             "--port", str(port), "--no-upstream", "--model", ""],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            env=env,
        )
        try:
            if not _wait_for_server(server_url, timeout=10):
                # Server failed to come up — capture output and fail loud.
                try:
                    proc.terminate()
                    out, _ = proc.communicate(timeout=3)
                except Exception:
                    out = b""
                print(f"  FAIL  server did not come up within 10s")
                print(f"  output: {out.decode('utf-8', errors='replace')[:2000]}")
                return 1

            # 1. /health returns 200 with the expected shape.
            code, payload = _http(server_url, "GET", "/health")
            _ok("GET /health -> 200", code == 200, f"got {code}: {payload}")
            _ok("/health has ok=True",
                isinstance(payload, dict) and payload.get("ok") is True,
                f"got {payload}")
            # Phase 3b — /health surfaces spec-decode state. Stub mode
            # (no draft env set) reports draft_model_loaded=false. The
            # field MUST be present so a bench-harness probe can detect
            # mlx-server versions that don't yet implement Phase 3b.
            _ok("/health has draft_model_loaded key",
                isinstance(payload, dict) and "draft_model_loaded" in payload,
                f"got {payload}")
            _ok("/health draft_model_loaded is false in stub mode",
                payload.get("draft_model_loaded") is False,
                f"got {payload}")
            _ok("/health has draft_model key (path or empty)",
                isinstance(payload, dict) and isinstance(payload.get("draft_model"), str),
                f"got {payload}")

            # 2. /v1/adapters/current returns 200 with the right keys.
            code, payload = _http(server_url, "GET", "/v1/adapters/current")
            _ok("GET /v1/adapters/current -> 200",
                code == 200, f"got {code}: {payload}")
            _ok("payload has adapter_path key",
                isinstance(payload, dict) and "adapter_path" in payload,
                f"got {payload}")
            _ok("payload has tensors_loaded key",
                isinstance(payload, dict) and "tensors_loaded" in payload,
                f"got {payload}")

            # 3. POST /v1/adapters/swap with missing field -> 400.
            code, payload = _http(server_url, "POST", "/v1/adapters/swap", body={})
            _ok("POST swap missing adapter_path -> 400",
                code == 400, f"got {code}: {payload}")

            # 4. POST /v1/adapters/swap with empty string -> 400.
            code, payload = _http(server_url, "POST", "/v1/adapters/swap",
                                  body={"adapter_path": ""})
            _ok("POST swap empty path -> 400",
                code == 400, f"got {code}: {payload}")

            # 5. POST /v1/adapters/swap with non-existent path -> 404.
            code, payload = _http(server_url, "POST", "/v1/adapters/swap",
                                  body={"adapter_path": "/nope/not/here"})
            _ok("POST swap nonexistent path -> 404",
                code == 404, f"got {code}: {payload}")

            # 6. POST /v1/adapters/swap with a real (synthetic) adapter -> 200.
            code, payload = _http(server_url, "POST", "/v1/adapters/swap",
                                  body={"adapter_path": str(adapter_dir),
                                        "contact_hash": "0xabc123"})
            _ok("POST swap real adapter -> 200", code == 200,
                f"got {code}: {payload}")
            _ok("response status=ok",
                isinstance(payload, dict) and payload.get("status") == "ok",
                f"got {payload}")
            _ok("response has adapter_loaded",
                isinstance(payload, dict)
                and isinstance(payload.get("adapter_loaded"), str),
                f"got {payload}")
            _ok("response has tensors_loaded > 0",
                isinstance(payload, dict)
                and isinstance(payload.get("tensors_loaded"), int)
                and payload["tensors_loaded"] > 0,
                f"got {payload}")

            # 7. Swap again to a different adapter -> 200.
            code, payload = _http(server_url, "POST", "/v1/adapters/swap",
                                  body={"adapter_path": str(adapter2_dir)})
            _ok("POST swap second adapter -> 200", code == 200,
                f"got {code}: {payload}")

            # 8. /v1/adapters/current now reflects the second swap.
            code, payload = _http(server_url, "GET", "/v1/adapters/current")
            _ok("GET current after swap -> 200",
                code == 200, f"got {code}: {payload}")
            resolved = os.path.realpath(str(adapter2_dir))
            _ok("current.adapter_path matches second swap",
                isinstance(payload, dict)
                and payload.get("adapter_path") == resolved,
                f"got {payload}, expected {resolved}")

            # Phase 3a — SSE streaming contract on /v1/chat/completions
            # with stream=true. Stub-mode server emits one delta chunk
            # + a finish-reason chunk + [DONE]. The protocol must be
            # solid in stub mode so the CI test catches a regression
            # without needing a real model.
            stream_body = {
                "model": "stub",
                "messages": [{"role": "user", "content": "hello world"}],
                "stream": True,
                "max_tokens": 16,
            }
            status, ctype, chunks, done = _http_sse(
                server_url, "/v1/chat/completions", stream_body)
            _ok("stream POST -> 200", status == 200, f"got {status}")
            _ok("stream Content-Type is text/event-stream",
                ctype.startswith("text/event-stream"),
                f"got {ctype!r}")
            _ok("stream emits at least one chunk", len(chunks) >= 1,
                f"got {len(chunks)} chunks")
            _ok("stream terminates with [DONE]", done, "no [DONE] sentinel")
            # Find the finish-reason chunk and the content delta chunk.
            content_deltas = [
                c for c in chunks
                if isinstance(c, dict) and c.get("choices")
                and c["choices"][0].get("delta", {}).get("content")
            ]
            finish_chunks = [
                c for c in chunks
                if isinstance(c, dict) and c.get("choices")
                and c["choices"][0].get("finish_reason") == "stop"
            ]
            _ok("stream has at least one content delta",
                len(content_deltas) >= 1,
                f"chunks: {chunks}")
            _ok("stream ends with finish_reason=stop",
                len(finish_chunks) >= 1,
                f"chunks: {chunks}")
            # The terminal chunk MUST carry the usage breakdown so the
            # bench harness's tps math doesn't divide by zero.
            terminal_with_usage = [
                c for c in finish_chunks
                if isinstance(c.get("usage"), dict)
                and "completion_tokens" in c["usage"]
            ]
            _ok("stream terminal chunk includes usage block",
                len(terminal_with_usage) >= 1,
                f"finish chunks: {finish_chunks}")

            # stream=false (default) still works — backwards compatible.
            code, payload = _http(
                server_url, "POST", "/v1/chat/completions",
                body={"model": "stub",
                      "messages": [{"role": "user", "content": "hi"}]})
            _ok("non-stream POST -> 200", code == 200, f"got {code}: {payload}")
            _ok("non-stream returns chat.completion object",
                isinstance(payload, dict) and payload.get("object") == "chat.completion",
                f"got {payload}")

        finally:
            try:
                proc.terminate()
                proc.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.communicate()
            except Exception:
                pass

    print()
    print(f"Result: {_PASSED} passed, {_FAILED} failed")
    return 0 if _FAILED == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
