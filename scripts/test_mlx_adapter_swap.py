#!/usr/bin/env python3
"""
Regression test for the MLX server's runtime LoRA-adapter swap endpoints.

Tests the endpoints added in Phase B2 of the M3 MLX bridge:
  - GET  /v1/adapters/current  → {adapter_path, tensors_loaded}
  - POST /v1/adapters/swap     → {status, adapter_path, tensors_loaded}
  - GET  /health.active_adapter

Behavior:
  - Runs against http://127.0.0.1:8741 IF the MLX server is up.
  - Skips with exit 0 if the server is not reachable (CI without a GPU).
  - Restores the original adapter at the end so the test is idempotent.

Run:
  python3 scripts/test_mlx_adapter_swap.py

Exit codes:
  0 - all tests passed or server unavailable (skip)
  1 - one or more tests failed
"""

import json
import os
import sys
import urllib.error
import urllib.request

SERVER = "http://127.0.0.1:8741"
ADAPTER = os.path.expanduser("~/.human/training-data/adapters/seth-lora-current")
TIMEOUT = 30  # MLX adapter loads can take a few seconds


_PASSED = 0
_FAILED = 0


def _http(method, path, body=None, timeout=TIMEOUT):
    url = SERVER + path
    data = None
    headers = {}
    if body is not None:
        data = json.dumps(body).encode("utf-8")
        headers["Content-Type"] = "application/json"
    req = urllib.request.Request(url, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        try:
            payload = json.loads(e.read().decode("utf-8"))
        except Exception:
            payload = {"error": "non-json-error-body"}
        return e.code, payload


def _ok(name, cond, detail=""):
    global _PASSED, _FAILED
    if cond:
        _PASSED += 1
        print(f"  PASS  {name}")
    else:
        _FAILED += 1
        print(f"  FAIL  {name}  {detail}")


def _check_server_alive():
    try:
        req = urllib.request.Request(SERVER + "/health", method="GET")
        with urllib.request.urlopen(req, timeout=2) as resp:
            return resp.status == 200
    except Exception:
        return False


def main():
    print(f"MLX adapter-swap test (server={SERVER})")

    if not _check_server_alive():
        print(f"  SKIP  MLX server not reachable at {SERVER} — skipping (exit 0)")
        print("        Start the server with: python3 scripts/mlx-server.py")
        return 0

    # 1. GET /v1/adapters/current returns 200 with valid JSON
    code, payload = _http("GET", "/v1/adapters/current")
    _ok("GET /v1/adapters/current -> 200", code == 200, f"got {code}: {payload}")
    _ok("payload has adapter_path key", isinstance(payload, dict) and "adapter_path" in payload, f"got {payload}")
    _ok("payload has tensors_loaded key", isinstance(payload, dict) and "tensors_loaded" in payload, f"got {payload}")
    original_adapter = payload.get("adapter_path") if isinstance(payload, dict) else None
    print(f"  INFO  original adapter: {original_adapter!r}")

    # 2. POST /v1/adapters/swap with invalid body (empty path) returns 400
    code, payload = _http("POST", "/v1/adapters/swap", body={"adapter_path": ""})
    _ok("POST swap empty path -> 400", code == 400, f"got {code}: {payload}")

    code, payload = _http("POST", "/v1/adapters/swap", body={"adapter_path": None})
    _ok("POST swap null path -> 400", code == 400, f"got {code}: {payload}")

    code, payload = _http("POST", "/v1/adapters/swap", body={})
    _ok("POST swap missing key -> 400", code == 400, f"got {code}: {payload}")

    # 3. POST /v1/adapters/swap with non-existent path returns 404
    code, payload = _http("POST", "/v1/adapters/swap",
                          body={"adapter_path": "/nonexistent/definitely/not/here/12345"})
    _ok("POST swap nonexistent path -> 404", code == 404, f"got {code}: {payload}")

    # 4. POST swap with a real adapter -> 200, and active adapter changes
    if not os.path.isdir(ADAPTER):
        print(f"  SKIP  real-adapter swap test ({ADAPTER} not present)")
    elif not os.path.isfile(os.path.join(ADAPTER, "adapters.safetensors")):
        print(f"  SKIP  real-adapter swap test (adapters.safetensors missing in {ADAPTER})")
    else:
        code, payload = _http("POST", "/v1/adapters/swap",
                              body={"adapter_path": ADAPTER})
        _ok("POST swap real adapter -> 200", code == 200, f"got {code}: {payload}")
        _ok("response status=ok",
            isinstance(payload, dict) and payload.get("status") == "ok",
            f"got {payload}")
        _ok("response has tensors_loaded > 0",
            isinstance(payload, dict) and isinstance(payload.get("tensors_loaded"), int)
            and payload["tensors_loaded"] > 0,
            f"got {payload}")

        # Verify /v1/adapters/current reflects the swap
        code, payload = _http("GET", "/v1/adapters/current")
        resolved = os.path.realpath(os.path.expanduser(ADAPTER))
        _ok("GET current after swap -> 200", code == 200, f"got {code}: {payload}")
        _ok("current.adapter_path matches swapped path",
            isinstance(payload, dict) and payload.get("adapter_path") == resolved,
            f"got {payload}, expected adapter_path={resolved}")
        _ok("current.tensors_loaded > 0",
            isinstance(payload, dict) and isinstance(payload.get("tensors_loaded"), int)
            and payload["tensors_loaded"] > 0,
            f"got {payload}")

        # Verify /health also reflects it
        code, payload = _http("GET", "/health")
        _ok("GET /health has active_adapter key",
            isinstance(payload, dict) and "active_adapter" in payload,
            f"got {payload}")
        _ok("/health.active_adapter == swapped path",
            isinstance(payload, dict) and payload.get("active_adapter") == resolved,
            f"got {payload.get('active_adapter')!r}, expected {resolved!r}")

        # Restore original adapter (idempotency) — if there was one
        if original_adapter:
            code, payload = _http("POST", "/v1/adapters/swap",
                                  body={"adapter_path": original_adapter})
            _ok("POST restore original adapter -> 200",
                code == 200, f"got {code}: {payload}")
        else:
            print("  INFO  no original adapter to restore (server had no adapter at start)")

    print()
    print(f"Result: {_PASSED} passed, {_FAILED} failed")
    return 0 if _FAILED == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
