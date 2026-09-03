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


if __name__ == "__main__":
    import pytest
    raise SystemExit(pytest.main([__file__, "-v"]))
