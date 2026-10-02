"""End-to-end: the REAL `human replay` binary, driven by replay_driver.py,
against a fake loopback model server. Proves the replay writes nothing outside
the run dir — including the guard-rejection training logs, which once landed
in the live ~/.human/training-data — and that those logs land in the turn's
scratch state instead.

Needs a production (non-test) build: build/human, or HU_REPLAY_TEST_BIN.
Skipped when there is none (the CI Python job has no C build); run it locally
before changing the harness. No real model, no chat.db, no ~/.human.
"""
import http.server
import json
import os
import sqlite3
import sys
import threading

import pytest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import replay_driver as rd  # noqa: E402

BIN = os.environ.get("HU_REPLAY_TEST_BIN") or os.path.join(rd.REPO_ROOT, "build", "human")
MARKER = "zqxreplaymarker"

pytestmark = pytest.mark.skipif(not os.access(BIN, os.X_OK), reason="no human binary built")


class FakeModel:
    """Loopback OpenAI-compatible server. The first reply call answers with a
    degenerate repetition the response guard rejects (so the DPO logger runs);
    the director gets a valid JSON decision; everything else a short reply."""

    def __init__(self):
        parent = self
        self.reply_calls = 0
        self.requests = 0

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def do_POST(self):
                n = int(self.headers.get("Content-Length", 0))
                req = json.loads(self.rfile.read(n) or b"{}")
                parent.requests += 1
                last = ((req.get("messages") or [{}])[-1].get("content") or "")
                if "New message from them" in last:
                    content = json.dumps({"action": "text", "delay_s": 1, "reaction": "none",
                                          "burst": False, "direction": "keep it short"})
                elif parent.reply_calls == 0:
                    parent.reply_calls += 1
                    content = " ".join([MARKER] * 40)
                else:
                    parent.reply_calls += 1
                    content = "yeah sounds good, what time"
                body = json.dumps({"id": "x", "object": "chat.completion",
                                   "model": req.get("model"),
                                   "choices": [{"index": 0, "finish_reason": "stop",
                                                "message": {"role": "assistant",
                                                            "content": content}}],
                                   "usage": {"prompt_tokens": 1, "completion_tokens": 1,
                                             "total_tokens": 2}}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

        self.srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), H)
        self.port = self.srv.server_address[1]
        threading.Thread(target=self.srv.serve_forever, daemon=True).start()

    def close(self):
        self.srv.shutdown()


def tree(root, skip):
    out = {}
    for dirpath, dirs, files in os.walk(root):
        if os.path.realpath(dirpath).startswith(os.path.realpath(skip)):
            dirs[:] = []
            continue
        for fn in files:
            p = os.path.join(dirpath, fn)
            st = os.stat(p)
            out[p] = (st.st_size, st.st_mtime_ns)
    return out


@pytest.mark.parametrize("sandbox", ["off", "auto"])
def test_real_replay_writes_only_inside_the_run_dir(tmp_path, sandbox, capsys):
    model = FakeModel()
    try:
        live = tmp_path / "home" / ".human"  # stands in for the user's live state
        (live / "training-data").mkdir(parents=True)
        (live / "training-data" / "keep.jsonl").write_text("{}\n")
        src = tmp_path / "state_src"
        (src / "personas").mkdir(parents=True)
        (src / "config.json").write_text(json.dumps({
            "default_provider": "mlx_local", "default_model": "fake-model",
            "providers": [{"name": "mlx_local", "api_key": "local-test-key",
                           "base_url": f"http://127.0.0.1:{model.port}/v1"}],
            "memory": {"backend": "sqlite"},
            "channels": {"imessage": {"daemon": {"llm_decides": True}}}}))
        sqlite3.connect(str(src / "memory.db")).close()
        root = tmp_path / "runs"
        run = root / "r1"
        run.mkdir(parents=True)
        (run / "turns.jsonl").write_text(json.dumps({
            "id": "t0001", "contact_id": "+15550001111", "ts": 1_790_000_000,
            "inbound_bubbles": ["you around tonight?"],
            # Short history: the guard's length check needs a recent average,
            # so the 40-word repetition is rejected (and logged) like in prod.
            "history": [{"from_me": False, "text": "yo", "ts": "2026-09-30 18:01:00"},
                        {"from_me": True, "text": "sup", "ts": "2026-09-30 18:02:00"},
                        {"from_me": False, "text": "nm u", "ts": "2026-09-30 18:03:00"}],
            "seth_action": "text", "seth_reply_bubbles": ["yeah"]}) + "\n")
        assert rd.main(["snapshot", "--name", "r1", "--run-root", str(root),
                        "--state-src", str(src)]) == 0
        before = tree(tmp_path, run)
        os.environ["HOME"] = str(tmp_path / "home")  # restored by the autouse fixture
        rc = rd.main(["run", "--name", "r1", "--run-root", str(root), "--human", BIN,
                      "--endpoint", f"http://127.0.0.1:{model.port}/v1", "--model", "fake-model",
                      "--temperature", "0", "--delay-ms", "0", "--sandbox", sandbox,
                      "--keep-state", "--arm", "off:"])
        assert rc == 0, capsys.readouterr()
        assert model.reply_calls >= 2  # the rejected reply and its retry
        after = tree(tmp_path, run)
        assert after == before  # nothing outside the run dir changed or appeared
        dpo = list((run / "work" / "off-t0001" / "training-data").glob("m3-dpo-rejections-*"))
        assert len(dpo) == 1 and MARKER in dpo[0].read_text()
        rows = [json.loads(line) for line in open(run / "out" / "off.jsonl")]
        assert rows[0]["channel_outbound_calls"] == 0 and rows[0]["action"] == "text"
    finally:
        model.close()


@pytest.fixture(autouse=True)
def _restore_home(monkeypatch):
    monkeypatch.setenv("HOME", os.environ.get("HOME", ""))
    yield
