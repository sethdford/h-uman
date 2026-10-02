#!/usr/bin/env python3
# Tests for scripts/eval_empty_reply_rate.py. No server is started and no
# model is loaded: the unreachable-server case points at a port that was free
# a moment ago, and everything else exercises pure helpers.
#
# Run: ~/.human/venvs/mlxtune312/bin/python -m pytest scripts/test_eval_empty_reply_rate.py -v
import json
import socket
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent))
import eval_empty_reply_rate as ev  # noqa: E402


@pytest.mark.parametrize("port", [8741, 8743])
def test_refuses_live_ports(port, tmp_path):
    with pytest.raises(SystemExit) as e:
        ev.main(["--port", str(port), "--label", "x", "--out", str(tmp_path / "o.json")])
    assert "REFUSING" in str(e.value)
    assert not (tmp_path / "o.json").exists()


@pytest.mark.parametrize("content,empty", [
    ("", True), (None, True), ("   \n", True), ("</think>", True),
    ("<think></think>\n", True), ("<|user|>", True),
    ("yeah", False), ("yes", False), ("\nsure, noon?", False),
])
def test_is_empty_reply(content, empty):
    assert ev.is_empty_reply(content) is empty


def test_summary_rates_and_absent_denominator():
    rs = [{"category": "classifier", "empty": True}, {"category": "classifier", "empty": False},
          {"category": "chat", "empty": False}]
    s = ev.summarize(rs)
    assert s["classifier"] == {"n": 2, "empty": 1, "rate": 0.5}
    assert s["all"]["n"] == 3 and s["all"]["empty"] == 1
    assert ev.summarize([])["chat"]["rate"] is None   # 0 requests is "absent", not 0%


def test_unreachable_server_is_not_measured(tmp_path):
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        port = s.getsockname()[1]
    rc = ev.main(["--port", str(port), "--label", "x", "--out", str(tmp_path / "o.json")])
    assert rc == 2
    assert not (tmp_path / "o.json").exists()


def test_fixture_has_both_categories():
    rows = ev.load_prompts(ev.DEFAULT_PROMPTS)
    cats = {r["category"] for r in rows}
    assert cats == {"classifier", "chat"}
    assert len({r["id"] for r in rows}) == len(rows)
    assert all(json.dumps(r["messages"]) for r in rows)


# --- empty-retry must be off (gemma-realtime c02bd50: MLX_EMPTY_RETRY default ON) ---

SRV = "/x/.venv312/bin/python3.12 /x/scripts/mlx-server.py --port 8748 --adapter-path /a"


@pytest.mark.parametrize("env,state", [
    (f"{SRV} PATH=/usr/bin GEMMA_DISABLE_THINKING=1", "on"),          # unset -> default ON
    (f"{SRV} PATH=/usr/bin MLX_EMPTY_RETRY=1", "on"),
    (f"{SRV} PATH=/usr/bin MLX_EMPTY_RETRY=", "on"),
    (f"{SRV} PATH=/usr/bin XMLX_EMPTY_RETRY=0", "on"),                 # not the same var
    (f"{SRV} PATH=/usr/bin MLX_EMPTY_RETRY=0", "off"),
    (f"{SRV} MLX_EMPTY_RETRY=Off HOME=/h", "off"),
    (f"{SRV} MLX_EMPTY_RETRY=false", "off"),
])
def test_empty_retry_state_mirrors_server_switch(env, state):
    assert ev.empty_retry_state(env) == state


def test_refuses_when_server_retry_is_on(monkeypatch, tmp_path):
    monkeypatch.setattr(ev, "server_env", lambda port: f"{SRV} PATH=/usr/bin")
    monkeypatch.setattr(ev, "_http", lambda *a, **k: pytest.fail("must refuse before any request"))
    rc = ev.main(["--port", "8748", "--label", "x", "--out", str(tmp_path / "o.json")])
    assert rc == 2 and not (tmp_path / "o.json").exists()


def test_unreadable_env_needs_confirmation_and_log(monkeypatch, tmp_path):
    monkeypatch.setattr(ev, "server_env", lambda port: None)
    monkeypatch.setattr(ev, "_http", lambda *a, **k: pytest.fail("must refuse before any request"))
    out = str(tmp_path / "o.json")
    assert ev.main(["--port", "8748", "--label", "x", "--out", out]) == 2
    assert ev.main(["--port", "8748", "--label", "x", "--out", out,
                    "--retry-disabled-confirmed"]) == 2          # flag alone is not enough


def _fake_server(monkeypatch, log, retry_during_run):
    monkeypatch.setattr(ev, "server_env", lambda port: f"{SRV} MLX_EMPTY_RETRY=0")

    def http(url, body=None, timeout=180):
        if url.endswith("/health"):
            return {"model": "glm", "active_adapter": "/a", "adapter_applied": True}
        if retry_during_run:
            with open(log, "a") as f:
                f.write("  [empty-retry] adapter=a first_tokens=1 retry_tokens=5 ok=True\n")
        return {"choices": [{"message": {"content": ""}, "finish_reason": "stop"}]}
    monkeypatch.setattr(ev, "_http", http)


def test_retry_tripwire_voids_the_run(monkeypatch, tmp_path):
    log = tmp_path / "srv.log"
    log.write_text("boot\n")
    _fake_server(monkeypatch, log, retry_during_run=True)
    out = tmp_path / "o.json"
    rc = ev.main(["--port", "8748", "--label", "x", "--out", str(out), "--samples", "1",
                  "--server-log", str(log)])
    assert rc == 2 and not out.exists()


def test_measured_run_records_basis_and_definition(monkeypatch, tmp_path):
    log = tmp_path / "srv.log"
    log.write_text("boot\n")
    _fake_server(monkeypatch, log, retry_during_run=False)
    out = tmp_path / "o.json"
    rc = ev.main(["--port", "8748", "--label", "x", "--out", str(out), "--samples", "1",
                  "--server-log", str(log), "--expect-adapter", "/a"])
    assert rc == 0
    rep = json.loads(out.read_text())
    assert rep["summary"]["all"]["rate"] == 1.0      # every fake reply was empty, and seen
    assert "environment" in rep["empty_retry_off_basis"]
    assert "SUPERSET" in rep["empty_definition"]
