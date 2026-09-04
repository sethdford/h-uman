import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def _run(args, env=None):
    return subprocess.run(
        [sys.executable, os.path.join(HERE, "gen_classifier_trials.py")] + args,
        capture_output=True, text=True, timeout=30, env=env,
    )


def test_refuses_when_base_file_missing():
    d = tempfile.mkdtemp()
    r = _run(["--base", os.path.join(d, "nope.json"), "--out", os.path.join(d, "out.json")])
    assert r.returncode != 0
    assert "REFUSING" in r.stderr
    assert not os.path.exists(os.path.join(d, "out.json"))


def test_refuses_when_base_file_has_zero_trials():
    d = tempfile.mkdtemp()
    base = os.path.join(d, "base.json")
    json.dump({"trials": []}, open(base, "w"))
    r = _run(["--base", base, "--out", os.path.join(d, "out.json")])
    assert r.returncode != 0
    assert "REFUSING" in r.stderr
    assert not os.path.exists(os.path.join(d, "out.json"))


def test_refuses_when_base_file_is_not_json():
    d = tempfile.mkdtemp()
    base = os.path.join(d, "base.json")
    open(base, "w").write("not json{{{")
    r = _run(["--base", base, "--out", os.path.join(d, "out.json")])
    assert r.returncode != 0
    assert "REFUSING" in r.stderr
    assert not os.path.exists(os.path.join(d, "out.json"))


def test_refuses_when_mlx_unreachable_and_writes_nothing():
    """Every generation call fails against a closed port -- must land under
    --min-ok and refuse, not write a file with 0/N trials."""
    d = tempfile.mkdtemp()
    base = os.path.join(d, "base.json")
    json.dump({"trials": [
        {"i": f"item_{i:02d}", "context": f"ctx {i}", "real_seth": f"real {i}"} for i in range(25)
    ]}, open(base, "w"))
    out = os.path.join(d, "out.json")
    r = _run([
        "--base", base, "--out", out,
        "--mlx-url", "http://127.0.0.1:1/v1/chat/completions",  # nothing listens on :1
        "--timeout", "2", "--min-ok", "5",
    ])
    assert r.returncode != 0
    assert "REFUSING" in r.stderr
    assert not os.path.exists(out)


class _FakeMlx:
    """A local stand-in for mlx-server: answers /v1/adapters/current, /health
    and /v1/chat/completions. `tensors` controls what the adapter endpoints
    report; `adapter` None = no adapter loaded."""

    def __init__(self, adapter, tensors):
        import http.server
        import threading

        parent = self

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):  # quiet
                pass

            def _send(self, obj, code=200):
                body = json.dumps(obj).encode()
                self.send_response(code)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                if self.path == "/v1/adapters/current":
                    self._send({"adapter_path": parent.adapter, "tensors_loaded": parent.tensors})
                elif self.path == "/health":
                    self._send({"model": "fake-base", "active_adapter": parent.adapter,
                                "adapter_applied": bool(parent.adapter),
                                "tensors_loaded": parent.tensors})
                else:
                    self._send({"error": "nope"}, 404)

            def do_POST(self):
                n = int(self.headers.get("Content-Length", "0"))
                self.rfile.read(n)
                parent.calls += 1
                self._send({"choices": [{"message": {"content": f"fake reply {parent.calls}"}}]})

        self.adapter, self.tensors, self.calls = adapter, tensors, 0
        self.srv = http.server.HTTPServer(("127.0.0.1", 0), H)
        self.url = f"http://127.0.0.1:{self.srv.server_address[1]}/v1/chat/completions"
        self.t = threading.Thread(target=self.srv.serve_forever, daemon=True)
        self.t.start()

    def close(self):
        self.srv.shutdown()


def _base_file(d, n=25):
    base = os.path.join(d, "base.json")
    json.dump({"trials": [
        {"i": f"item_{i:02d}", "context": f"ctx {i}", "real_seth": f"real {i}"} for i in range(n)
    ]}, open(base, "w"))
    return base


def test_refuses_when_adapter_named_but_nothing_bound():
    """The 2026-07-26 → 09-04 lie: server names an adapter, binds 0 tensors.
    Trials would measure the base under an adapter label — must refuse and
    make ZERO generation calls."""
    d = tempfile.mkdtemp()
    srv = _FakeMlx(adapter="/adapters/seth-v6", tensors=0)
    try:
        out = os.path.join(d, "out.json")
        r = _run(["--base", _base_file(d), "--out", out, "--mlx-url", srv.url,
                  "--timeout", "5", "--min-ok", "5"])
        assert r.returncode != 0
        assert "REFUSING" in r.stderr and "nothing bound" in r.stderr
        assert not os.path.exists(out)
        assert srv.calls == 0
    finally:
        srv.close()


def test_records_server_provenance_not_a_literal():
    """A bound adapter: the record carries the server's own answer (path,
    tensors_loaded, adapter_bound) and a hash of the head that was sent —
    never the old hardcoded 'production :8741 + live head' string."""
    d = tempfile.mkdtemp()
    srv = _FakeMlx(adapter="/adapters/seth-v6", tensors=160)
    try:
        out = os.path.join(d, "out.json")
        r = _run(["--base", _base_file(d), "--out", out, "--mlx-url", srv.url,
                  "--timeout", "5", "--min-ok", "5"])
        assert r.returncode == 0, r.stderr
        doc = json.load(open(out))
        p = doc["provenance"]
        assert p["adapter_path"] == "/adapters/seth-v6"
        assert p["tensors_loaded"] == 160 and p["adapter_bound"] is True
        assert p["provenance_available"] is True
        assert len(p["head_sha256"]) == 64 and p["head_bytes"] >= 500
        assert doc["adapter"] == "/adapters/seth-v6"
        assert "production :8741" not in json.dumps(doc)
        assert len(doc["trials"]) == 25 and srv.calls == 25
    finally:
        srv.close()


def test_no_adapter_server_refused_unless_explicitly_allowed():
    d = tempfile.mkdtemp()
    srv = _FakeMlx(adapter=None, tensors=0)
    try:
        out = os.path.join(d, "out.json")
        r = _run(["--base", _base_file(d), "--out", out, "--mlx-url", srv.url,
                  "--timeout", "5", "--min-ok", "5"])
        assert r.returncode != 0 and "no adapter" in r.stderr
        assert not os.path.exists(out) and srv.calls == 0
        r = _run(["--base", _base_file(d), "--out", out, "--mlx-url", srv.url,
                  "--timeout", "5", "--min-ok", "5", "--allow-no-adapter"])
        assert r.returncode == 0, r.stderr
        doc = json.load(open(out))
        assert doc["adapter"] == "none" and doc["provenance"]["adapter_bound"] is False
    finally:
        srv.close()


if __name__ == "__main__":
    import pytest
    raise SystemExit(pytest.main([__file__, "-v"]))
