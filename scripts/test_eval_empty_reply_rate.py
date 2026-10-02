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
