import json, os, subprocess, sys, tempfile, http.server, threading
HERE = os.path.dirname(os.path.abspath(__file__))

class Fake(http.server.BaseHTTPRequestHandler):
    empty = False
    def log_message(self, *a): pass
    def do_POST(self):
        n = int(self.headers.get("Content-Length", 0)); body = json.loads(self.rfile.read(n))
        if self.path.endswith("/chat/completions"):
            txt = "" if Fake.empty else "yeah just chilling " + body["messages"][-1]["content"][:5]
            out = {"choices": [{"message": {"content": txt}}]}
        else:
            out = {"data": [{"index": i, "embedding": [1.0, 0.0]} for i, _ in enumerate(body["input"])]}
        b = json.dumps(out).encode(); self.send_response(200); self.send_header("Content-Length", str(len(b))); self.end_headers(); self.wfile.write(b)

def run(port, out_dir, env_extra=None):
    env = {**os.environ, "HU_DRIFT_FAKE_HEAD": "1", **(env_extra or {})}
    return subprocess.run([sys.executable, os.path.join(HERE, "drift_monitor.py"), "--chat-url", f"http://127.0.0.1:{port}/v1/chat/completions",
                           "--embed-url", f"http://127.0.0.1:{port}/v1/embeddings", "--out-dir", out_dir], capture_output=True, text=True, env=env)

def test_baseline_then_compare_and_refuse_on_empty():
    srv = http.server.HTTPServer(("127.0.0.1", 0), Fake); port = srv.server_address[1]
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    d = tempfile.mkdtemp()
    r = run(port, d); assert r.returncode == 0, r.stderr; assert os.path.exists(os.path.join(d, "baseline.json"))
    r = run(port, d); assert r.returncode == 0, r.stderr
    files = [f for f in os.listdir(d) if f.startswith("drift-")]; assert files
    doc = json.load(open(os.path.join(d, files[0]))); assert doc["verdict"] == "STABLE" and doc["mean_cos"] == 1.0
    Fake.empty = True
    r = run(port, d); assert r.returncode != 0 and "no drift file written" in r.stderr
    assert len([f for f in os.listdir(d) if f.startswith("drift-")]) == 1  # nothing new written
    srv.shutdown()
