# tests/test_second_opinion_backend.py
"""Local by default; Vertex only with allow; the Gemma server always stops."""
import io
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
from second_opinion import backend as be  # noqa: E402


def test_gemma_refuses_non_loopback_urls():
    for url in ("http://10.0.0.5:8743", "https://api.example.com", "http://gemma.local:8743"):
        with pytest.raises(be.BackendError):
            be.GemmaBackend(base_url=url)
    assert be.GemmaBackend().name.endswith("@local")


def test_gemma_folds_system_into_user_and_reads_content():
    seen = {}

    def post(url, body, headers, timeout):
        seen.update(url=url, body=body)
        return {"choices": [{"message": {"content": "supported\nok"}}]}

    g = be.GemmaBackend(post=post)
    assert g.generate("SYS", "USER", max_tokens=50) == "supported\nok"
    assert seen["url"] == "http://127.0.0.1:8743/v1/chat/completions"
    assert seen["body"]["messages"] == [{"role": "user", "content": "SYS\n\nUSER"}]
    assert seen["body"]["temperature"] == 0.0 and seen["body"]["max_tokens"] == 50


def test_gemma_bad_response_shape_raises():
    g = be.GemmaBackend(post=lambda *a: {"error": "x"})
    with pytest.raises(be.BackendError):
        g.generate("s", "u")


def test_vertex_is_opt_in_and_announces_itself():
    with pytest.raises(be.BackendError):
        be.VertexBackend()
    notice = io.StringIO()
    seen = {}

    def post(url, body, headers, timeout):
        seen.update(url=url, body=body, headers=headers)
        return {"candidates": [{"content": {"parts": [{"text": "unsupported"}, {"text": "\nr"}]}}]}

    v = be.VertexBackend(allow=True, post=post, token=lambda: "tok", notice=notice)
    assert "leave this Mac" in notice.getvalue()
    assert v.generate("S", "U", max_tokens=80) == "unsupported\nr"
    assert "gemini-3.8-flash:generateContent" in seen["url"] and "johnb-2025" in seen["url"]
    gc = seen["body"]["generationConfig"]
    assert gc["thinkingConfig"]["thinkingBudget"] == 0 and gc["maxOutputTokens"] == 80
    assert seen["headers"]["Authorization"] == "Bearer tok"
    assert v.name == "gemini-3.8-flash@vertex"


class FakeProc:
    def __init__(self, exit_early=False):
        self.returncode = 1 if exit_early else None
        self.terminated = False

    def poll(self):
        return self.returncode

    def terminate(self):
        self.terminated = True
        self.returncode = 0

    def wait(self, timeout=None):
        return self.returncode

    def kill(self):
        self.returncode = -9


def test_serve_gemma_yields_after_health_and_always_stops():
    proc = FakeProc()
    states = iter([False, False, True])  # pre-check: port free; then healthy on 2nd poll
    with be.serve_gemma(popen=lambda *a, **k: proc, health=lambda: next(states),
                        sleep=lambda s: None) as g:
        assert isinstance(g, be.GemmaBackend)
    assert proc.terminated


def test_serve_gemma_stops_server_when_body_raises():
    proc = FakeProc()
    states = iter([False, True])
    with pytest.raises(ValueError):
        with be.serve_gemma(popen=lambda *a, **k: proc, health=lambda: next(states),
                            sleep=lambda s: None):
            raise ValueError("job blew up")
    assert proc.terminated


def test_serve_gemma_refuses_a_port_someone_else_is_serving():
    started = []
    with pytest.raises(be.BackendError):
        with be.serve_gemma(popen=lambda *a, **k: started.append(1), health=lambda: True):
            pass
    assert started == []


def test_serve_gemma_times_out_and_stops():
    proc = FakeProc()
    t = iter([0, 0, 700])
    with pytest.raises(be.BackendError):
        with be.serve_gemma(popen=lambda *a, **k: proc, health=lambda: False,
                            sleep=lambda s: None, clock=lambda: next(t), wait_s=600):
            pass
    assert proc.terminated


def test_serve_gemma_reports_early_exit():
    proc = FakeProc(exit_early=True)
    states = iter([False, False])
    with pytest.raises(be.BackendError):
        with be.serve_gemma(popen=lambda *a, **k: proc, health=lambda: next(states),
                            sleep=lambda s: None):
            pass
