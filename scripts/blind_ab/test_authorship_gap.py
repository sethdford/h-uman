import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))


def _run(args):
    return subprocess.run([sys.executable, os.path.join(HERE, "authorship_gap.py")] + args,
                          capture_output=True, text=True, timeout=60)


def test_refuses_when_zero_valid_trials_after_filtering():
    """25 rows clear --min-trials at input, but none carries an ai_response.
    Must refuse (nothing written) BEFORE loading LUAR or touching chat.db."""
    d = tempfile.mkdtemp()
    trials = os.path.join(d, "trials.json")
    json.dump({"trials": [{"i": f"item_{i:02d}", "context": f"ctx {i}", "real_seth": f"real {i}",
                           "ai_response": ""} for i in range(25)]}, open(trials, "w"))
    out = os.path.join(d, "out.json")
    r = _run(["--trials", trials, "--out", out, "--chatdb", os.path.join(d, "no-such-chat.db")])
    assert r.returncode != 0
    assert "REFUSING" in r.stderr and "0 valid" in r.stderr
    assert not os.path.exists(out)


def test_min_trials_applies_to_valid_rows_not_raw_rows():
    d = tempfile.mkdtemp()
    trials = os.path.join(d, "trials.json")
    rows = [{"i": f"item_{i:02d}", "context": f"ctx {i}", "real_seth": f"real {i}",
             "ai_response": "yeah" if i < 5 else ""} for i in range(25)]
    json.dump({"trials": rows}, open(trials, "w"))
    out = os.path.join(d, "out.json")
    r = _run(["--trials", trials, "--out", out, "--min-trials", "20",
              "--chatdb", os.path.join(d, "no-such-chat.db")])
    assert r.returncode != 0
    assert "REFUSING" in r.stderr and "5 valid" in r.stderr
    assert not os.path.exists(out)


if __name__ == "__main__":
    import pytest
    raise SystemExit(pytest.main([__file__, "-v"]))
