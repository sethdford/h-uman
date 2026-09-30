"""Hermetic pytest suite for the local-judge backend added to
scripts/eval_semantic_live_gate.py (--judge-backend {local,vertex}, default
local).

Context: the owner's standing second-opinion-lane policy (2026-09-29,
scripts/second_opinion/backend.py) is that any job reading real message text
defaults to the LOCAL Gemma judge (served on this Mac, 127.0.0.1:8743) rather
than Vertex; Vertex is opt-in behind --judge-backend vertex and prints a
stderr notice on every use.

No network: the local judge is exercised at the unit level against a stub
backend object (duck-typed like second_opinion.backend.GemmaBackend — a
`.generate(system, user, max_tokens)` method and a `.model` attribute), and at
the main()-level tests preflight_judge_local()/judge_ei_reality_local() are
monkeypatched so no real HTTP call to :8743 (or anywhere else) ever happens.
Reuses the fake_server / contexts_file / _base_args / _patch_common fixtures
already established in scripts/test_eval_semantic_live_gate.py — see that
file's docstring for the network-free discipline this suite follows.
scripts/test_eval_semantic_live_gate.py's shared _base_args() pins
--judge-backend vertex (that whole suite predates this backend split and was
written against the Vertex/HU_GATE_FAKE=1 path); tests here that want the
local backend pass --judge-backend local as `extra` (argparse: the LAST
--judge-backend flag wins) or build a bare argv with no flag at all to prove
the true default.
"""
from __future__ import annotations

import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "scripts"))
import eval_semantic_live_gate as G  # noqa: E402
from test_eval_semantic_live_gate import (  # noqa: E402
    fake_server, fake_judge, contexts_file, _base_args, _patch_common,
)

# Re-exported so pytest resolves them as fixtures of THIS module too.
assert fake_server and fake_judge


class _StubBackend:
    """Duck-types second_opinion.backend.GemmaBackend without any network:
    `.generate()` returns a canned string, `.model` is a plain attribute."""

    def __init__(self, text, model="stub-gemma-model"):
        self._text = text
        self.model = model

    def generate(self, system, user, max_tokens=200):
        return self._text


class _BoomBackend:
    model = "stub-gemma-model"

    def generate(self, system, user, max_tokens=200):
        raise RuntimeError("connection refused")


# ---------------------------------------------------------------------------
# judge_ei_reality_local() — parsing, against a stubbed backend, no network
# ---------------------------------------------------------------------------
def test_judge_ei_reality_local_parses_strict_json():
    backend = _StubBackend('{"emotional_intelligence": 4, "reality_awareness": 5}')
    assert G.judge_ei_reality_local("hey you ok?", "yeah just tired", backend) == \
        {"ei": 4, "reality": 5}


def test_judge_ei_reality_local_parses_json_with_surrounding_prose():
    """Gemma has no responseSchema, so real output sometimes wraps the JSON
    in a sentence or a code fence — the parser must find the first balanced
    {...} object rather than assume the response starts with '{'."""
    backend = _StubBackend(
        "Sure, here you go:\n```json\n"
        '{"emotional_intelligence": 2, "reality_awareness": 3}\n```\nHope that helps!')
    assert G.judge_ei_reality_local("x", "y", backend) == {"ei": 2, "reality": 3}


def test_judge_ei_reality_local_returns_none_on_unparseable_text():
    """No JSON object at all in the response -> no score, never a default."""
    backend = _StubBackend("I'm not able to help with that request.")
    assert G.judge_ei_reality_local("x", "y", backend) is None


def test_judge_ei_reality_local_returns_none_when_backend_raises():
    """Network/backend failure -> no score, same contract as the Vertex judge."""
    assert G.judge_ei_reality_local("x", "y", _BoomBackend()) is None


def test_judge_ei_reality_local_returns_none_on_out_of_range_high_score():
    backend = _StubBackend('{"emotional_intelligence": 7, "reality_awareness": 3}')
    assert G.judge_ei_reality_local("x", "y", backend) is None


def test_judge_ei_reality_local_returns_none_on_out_of_range_zero_score():
    backend = _StubBackend('{"emotional_intelligence": 0, "reality_awareness": 3}')
    assert G.judge_ei_reality_local("x", "y", backend) is None


def test_judge_ei_reality_local_returns_none_on_missing_field():
    backend = _StubBackend('{"emotional_intelligence": 4}')
    assert G.judge_ei_reality_local("x", "y", backend) is None


def test_judge_ei_reality_local_returns_none_on_non_integer_value():
    backend = _StubBackend('{"emotional_intelligence": "high", "reality_awareness": 3}')
    assert G.judge_ei_reality_local("x", "y", backend) is None


# ---------------------------------------------------------------------------
# _first_json_object() — the robust-parse helper directly
# ---------------------------------------------------------------------------
def test_first_json_object_finds_balanced_object_after_prefix_text():
    text = 'not json { "a": {"b": 1}, "c": 2 } trailing text'
    assert G._first_json_object(text) == '{ "a": {"b": 1}, "c": 2 }'


def test_first_json_object_returns_none_when_no_brace_present():
    assert G._first_json_object("no braces here at all") is None


def test_first_json_object_returns_none_on_unbalanced_braces():
    assert G._first_json_object("{ this never closes") is None


# ---------------------------------------------------------------------------
# _local_judge_healthy() / preflight_judge_local() — no real network
# ---------------------------------------------------------------------------
def test_local_judge_healthy_true_on_200(monkeypatch):
    class _FakeResp:
        status = 200

        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

    monkeypatch.setattr(G.urllib.request, "urlopen", lambda url, timeout=10: _FakeResp())
    assert G._local_judge_healthy("http://127.0.0.1:8743") is True


def test_local_judge_healthy_false_on_connection_error(monkeypatch):
    def boom(url, timeout=10):
        raise OSError("connection refused")
    monkeypatch.setattr(G.urllib.request, "urlopen", boom)
    assert G._local_judge_healthy("http://127.0.0.1:8743") is False


def test_local_judge_healthy_false_on_non_200(monkeypatch):
    class _FakeResp:
        status = 500

        def __enter__(self):
            return self

        def __exit__(self, *a):
            return False

    monkeypatch.setattr(G.urllib.request, "urlopen", lambda url, timeout=10: _FakeResp())
    assert G._local_judge_healthy("http://127.0.0.1:8743") is False


def test_preflight_judge_local_delegates_to_local_judge_healthy(monkeypatch):
    monkeypatch.setattr(G, "_local_judge_healthy", lambda url, timeout=10: True)
    assert G.preflight_judge_local("http://127.0.0.1:8743") is True
    monkeypatch.setattr(G, "_local_judge_healthy", lambda url, timeout=10: False)
    assert G.preflight_judge_local("http://127.0.0.1:8743") is False


# ---------------------------------------------------------------------------
# main() — default backend, preflight refusal + start command, provenance
# ---------------------------------------------------------------------------
def _no_backend_flag_args(fake_server, contexts_file, out_path, extra=None):
    """A raw argv with NO --judge-backend flag at all, unlike the shared
    _base_args() (which pins vertex) — the only way to prove the true
    argparse default is 'local'."""
    args = [
        "--contexts", contexts_file,
        "--n", "32",
        "--min-n", "30",
        "--server", fake_server,
        "--embed-url", fake_server,
        "--memory-db", "/dev/null",
        "--out", out_path,
    ]
    return args + (extra or [])


def test_main_default_backend_is_local_with_no_flag_given(monkeypatch, fake_server,
                                                           contexts_file, tmp_path):
    _patch_common(monkeypatch, recall_hit_ratio=1.0)
    monkeypatch.setattr(G, "preflight_judge_local", lambda url: True)
    monkeypatch.setattr(G, "judge_ei_reality_local",
                        lambda incoming, reply, backend: {"ei": 4, "reality": 5})
    out = str(tmp_path / "gate.json")
    rc = G.main(_no_backend_flag_args(fake_server, contexts_file, out))
    assert rc in (0, 1)
    doc = json.loads(Path(out).read_text())
    assert doc["judge_backend"] == "local"
    assert doc["judge_model"] == G.LOCAL_JUDGE_MODEL
    assert doc["shadow"]["n_ei"] >= 30
    assert doc["live"]["n_ei"] >= 30


def test_main_local_backend_refuses_with_start_command_when_unreachable(
        monkeypatch, fake_server, contexts_file, tmp_path, capsys):
    """Preflight failure must refuse (exit 2, no file) AND name the exact
    command to start the :8743 server, so an operator isn't left guessing."""
    _patch_common(monkeypatch)
    monkeypatch.setattr(G, "preflight_judge_local", lambda url: False)
    out = str(tmp_path / "gate.json")
    rc = G.main(_base_args(fake_server, contexts_file, out, extra=["--judge-backend", "local"]))
    assert rc == 2
    assert not Path(out).exists()
    captured = capsys.readouterr()
    assert G.LOCAL_JUDGE_START_CMD in captured.err
    assert "mlx_lm.server" in captured.err


def test_main_vertex_backend_prints_stderr_notice(monkeypatch, fake_server, contexts_file,
                                                   tmp_path, capsys):
    """--judge-backend vertex (opt-in) must notice, every time, that real
    message text is leaving this Mac."""
    _patch_common(monkeypatch, recall_hit_ratio=1.0)
    out = str(tmp_path / "gate.json")
    rc = G.main(_base_args(fake_server, contexts_file, out))  # _base_args pins vertex
    assert rc in (0, 1)
    captured = capsys.readouterr()
    assert "backend=vertex" in captured.err
    assert "leave this Mac" in captured.err
    assert G.GEMINI_PROJECT_ID in captured.err


def test_local_backend_does_not_print_vertex_notice(monkeypatch, fake_server, contexts_file,
                                                     tmp_path, capsys):
    _patch_common(monkeypatch, recall_hit_ratio=1.0)
    monkeypatch.setattr(G, "preflight_judge_local", lambda url: True)
    monkeypatch.setattr(G, "judge_ei_reality_local",
                        lambda incoming, reply, backend: {"ei": 4, "reality": 5})
    out = str(tmp_path / "gate.json")
    rc = G.main(_base_args(fake_server, contexts_file, out, extra=["--judge-backend", "local"]))
    assert rc in (0, 1)
    captured = capsys.readouterr()
    assert "backend=vertex" not in captured.err
    assert "leave this Mac" not in captured.err


def test_local_and_vertex_verdicts_carry_distinct_provenance(monkeypatch, fake_server,
                                                              contexts_file, tmp_path):
    """A local-judged verdict must be distinguishable from a Vertex-judged one."""
    _patch_common(monkeypatch, recall_hit_ratio=1.0)
    monkeypatch.setattr(G, "preflight_judge_local", lambda url: True)
    monkeypatch.setattr(G, "judge_ei_reality_local",
                        lambda incoming, reply, backend: {"ei": 4, "reality": 5})

    out_local = str(tmp_path / "local.json")
    rc = G.main(_base_args(fake_server, contexts_file, out_local,
                           extra=["--judge-backend", "local"]))
    assert rc in (0, 1)
    local_doc = json.loads(Path(out_local).read_text())

    out_vertex = str(tmp_path / "vertex.json")
    rc = G.main(_base_args(fake_server, contexts_file, out_vertex))  # vertex via _base_args
    assert rc in (0, 1)
    vertex_doc = json.loads(Path(out_vertex).read_text())

    assert local_doc["judge_backend"] == "local"
    assert vertex_doc["judge_backend"] == "vertex"
    assert local_doc["judge_model"] == G.LOCAL_JUDGE_MODEL
    assert vertex_doc["judge_model"] == G.GEMINI_MODEL
    assert local_doc["judge_backend"] != vertex_doc["judge_backend"]
    assert local_doc["judge_model"] != vertex_doc["judge_model"]


def test_main_local_backend_uses_judge_local_model_override(monkeypatch, fake_server,
                                                             contexts_file, tmp_path):
    _patch_common(monkeypatch, recall_hit_ratio=1.0)
    monkeypatch.setattr(G, "preflight_judge_local", lambda url: True)
    monkeypatch.setattr(G, "judge_ei_reality_local",
                        lambda incoming, reply, backend: {"ei": 4, "reality": 5})
    out = str(tmp_path / "gate.json")
    rc = G.main(_base_args(fake_server, contexts_file, out,
                           extra=["--judge-backend", "local",
                                  "--judge-local-model", "some-other-gemma-build"]))
    assert rc in (0, 1)
    doc = json.loads(Path(out).read_text())
    assert doc["judge_model"] == "some-other-gemma-build"
