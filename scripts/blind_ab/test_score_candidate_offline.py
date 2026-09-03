# scripts/blind_ab/test_score_candidate_offline.py
#
# Tests for score_candidate_offline.py's --dry-run and refusal paths only.
# Deliberately does NOT exercise run_gen_worker() (needs mlx_lm) or the real
# scoring path (needs authorship_gap.py + torch/transformers via
# --eval-python) — those require model weights / LUAR and are out of scope
# for a hermetic suite (see .claude/rules/reports-success-does-nothing.md:
# a test claiming "it works" without running the real path would be exactly
# the fabricated-evidence shape this repo's rules exist to prevent).
#
# Runs under plain python3 (stdlib + numpy only, matching adapter_is_real.py's
# own dependency footprint) — no mlx, no torch required.
#   python3 -m pytest scripts/blind_ab/test_score_candidate_offline.py -v
import json
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np
import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "score_candidate_offline.py")
sys.path.insert(0, HERE)

import score_candidate_offline as sco  # noqa: E402

sys.path.insert(0, os.path.dirname(HERE))  # scripts/ — for adapter_is_real
from adapter_is_real import MIN_BYTES  # noqa: E402


# --------------------------------------------------------------------------
# fixture helpers
# --------------------------------------------------------------------------


def write_safetensors(path, tensors):
    """Minimal safetensors writer — mirrors scripts/test_adapter_is_real.py's
    write_st so a fixture adapter passes/fails adapter_is_real.py exactly
    like a real one would, without loading any actual model."""
    hdr, blobs, off = {}, [], 0
    for k, arr in tensors.items():
        b = arr.astype(np.float32).tobytes()
        hdr[k] = {"dtype": "F32", "shape": list(arr.shape), "data_offsets": [off, off + len(b)]}
        blobs.append(b)
        off += len(b)
    h = json.dumps(hdr).encode()
    with open(path, "wb") as f:
        f.write(struct.pack("<Q", len(h)))
        f.write(h)
        f.write(b"".join(blobs))


def make_real_adapter(d, scale=2.0):
    """A directory that PASSES adapter_is_real.py: big enough, non-zero
    lora_b, safe scale."""
    os.makedirs(d, exist_ok=True)
    big = np.zeros((MIN_BYTES // 4 + 8,), np.float32)
    write_safetensors(os.path.join(d, "adapters.safetensors"),
                       {"l.lora_a": big, "l.lora_b": np.ones((16,), np.float32)})
    json.dump({"lora_parameters": {"rank": 8, "scale": scale, "dropout": 0.0}},
              open(os.path.join(d, "adapter_config.json"), "w"))
    return d


def make_trials_file(path, n=25):
    trials = [{"i": f"item_{i:02d}", "context": f"ctx {i}", "real_seth": f"real seth reply {i}"}
              for i in range(n)]
    json.dump({"trials": trials}, open(path, "w"))
    return path


def make_config(path, adapter_path):
    json.dump({"personalization": {"lora_adapter_path": adapter_path}}, open(path, "w"))
    return path


def _run(args):
    return subprocess.run([sys.executable, SCRIPT] + args, capture_output=True, text=True, timeout=60)


# --------------------------------------------------------------------------
# pure-function unit tests
# --------------------------------------------------------------------------


def test_served_adapter_path_reads_personalization_field():
    d = tempfile.mkdtemp()
    cfg = make_config(os.path.join(d, "config.json"), "/some/adapter/dir")
    assert sco.served_adapter_path(cfg) == "/some/adapter/dir"


def test_served_adapter_path_falls_back_to_mlx_local():
    d = tempfile.mkdtemp()
    cfg = os.path.join(d, "config.json")
    json.dump({"mlx_local": {"adapter_path": "/legacy/adapter"}}, open(cfg, "w"))
    assert sco.served_adapter_path(cfg) == "/legacy/adapter"


def test_served_adapter_path_missing_file_returns_none():
    assert sco.served_adapter_path("/nonexistent/config.json") is None


def test_load_trial_contexts_refuses_missing_file():
    with pytest.raises(SystemExit, match="REFUSING"):
        sco.load_trial_contexts("/nonexistent/trials.json")


def test_load_trial_contexts_refuses_below_min_trials():
    d = tempfile.mkdtemp()
    p = make_trials_file(os.path.join(d, "trials.json"), n=5)
    with pytest.raises(SystemExit, match="usable trials"):
        sco.load_trial_contexts(p, min_trials=20)


def test_load_trial_contexts_accepts_enough_trials():
    d = tempfile.mkdtemp()
    p = make_trials_file(os.path.join(d, "trials.json"), n=25)
    trials = sco.load_trial_contexts(p, min_trials=20)
    assert len(trials) == 25


def test_check_adapter_dir_missing_path():
    ok, why = sco.check_adapter_dir("candidate", None)
    assert not ok and "missing" in why


def test_check_adapter_dir_real_adapter_passes():
    d = tempfile.mkdtemp()
    make_real_adapter(os.path.join(d, "adapter"))
    ok, why = sco.check_adapter_dir("candidate", os.path.join(d, "adapter"))
    assert ok, why


def test_check_adapter_dir_catastrophic_scale_fails():
    d = tempfile.mkdtemp()
    make_real_adapter(os.path.join(d, "adapter"), scale=20.0)
    ok, why = sco.check_adapter_dir("candidate", os.path.join(d, "adapter"))
    assert not ok and "scale" in why


def test_check_python_missing():
    ok, why = sco.check_python("gen-python", "/nonexistent/bin/python")
    assert not ok and "MISSING" in why


def test_check_python_present():
    ok, why = sco.check_python("gen-python", sys.executable)
    assert ok, why


# --------------------------------------------------------------------------
# CLI-level dry-run / refusal tests (subprocess, matches
# scripts/blind_ab/test_gen_classifier_trials.py's style)
# --------------------------------------------------------------------------


def test_dry_run_fails_when_trial_file_missing():
    d = tempfile.mkdtemp()
    cand = make_real_adapter(os.path.join(d, "candidate"))
    serv = make_real_adapter(os.path.join(d, "serving"))
    chatdb = os.path.join(d, "chat.db")
    open(chatdb, "w").close()
    r = _run([
        "--dry-run", "--candidate", cand, "--serving", serv,
        "--trials", os.path.join(d, "nope.json"), "--chatdb", chatdb,
        "--gen-python", sys.executable, "--eval-python", sys.executable,
    ])
    assert r.returncode == 1, r.stdout + r.stderr
    assert "REFUSING" in r.stdout
    assert "[dry-run] FAIL" in r.stdout
    assert "no model weights were loaded" in r.stdout


def test_dry_run_fails_when_adapter_missing():
    d = tempfile.mkdtemp()
    serv = make_real_adapter(os.path.join(d, "serving"))
    trials = make_trials_file(os.path.join(d, "trials.json"))
    chatdb = os.path.join(d, "chat.db")
    open(chatdb, "w").close()
    r = _run([
        "--dry-run", "--candidate", os.path.join(d, "does-not-exist"), "--serving", serv,
        "--trials", trials, "--chatdb", chatdb,
        "--gen-python", sys.executable, "--eval-python", sys.executable,
    ])
    assert r.returncode == 1, r.stdout + r.stderr
    assert "candidate adapter dir missing" in r.stdout


def test_dry_run_fails_when_eval_python_missing():
    d = tempfile.mkdtemp()
    cand = make_real_adapter(os.path.join(d, "candidate"))
    serv = make_real_adapter(os.path.join(d, "serving"))
    trials = make_trials_file(os.path.join(d, "trials.json"))
    chatdb = os.path.join(d, "chat.db")
    open(chatdb, "w").close()
    r = _run([
        "--dry-run", "--candidate", cand, "--serving", serv,
        "--trials", trials, "--chatdb", chatdb,
        "--gen-python", sys.executable, "--eval-python", "/nonexistent/eval312/bin/python",
    ])
    assert r.returncode == 1, r.stdout + r.stderr
    assert "eval-python" in r.stdout and "MISSING" in r.stdout


def test_dry_run_passes_when_everything_valid():
    """The full happy-path precondition set — still --dry-run, so still
    loads nothing."""
    d = tempfile.mkdtemp()
    cand = make_real_adapter(os.path.join(d, "candidate"))
    serv = make_real_adapter(os.path.join(d, "serving"))
    trials = make_trials_file(os.path.join(d, "trials.json"))
    chatdb = os.path.join(d, "chat.db")
    open(chatdb, "w").close()
    r = _run([
        "--dry-run", "--candidate", cand, "--serving", serv,
        "--trials", trials, "--chatdb", chatdb,
        "--gen-python", sys.executable, "--eval-python", sys.executable,
    ])
    assert r.returncode == 0, r.stdout + r.stderr
    assert "[dry-run] PASS" in r.stdout
    assert "no model weights were loaded" in r.stdout


def test_dry_run_resolves_serving_from_config_when_omitted():
    d = tempfile.mkdtemp()
    cand = make_real_adapter(os.path.join(d, "candidate"))
    serv = make_real_adapter(os.path.join(d, "serving"))
    cfg = make_config(os.path.join(d, "config.json"), serv)
    trials = make_trials_file(os.path.join(d, "trials.json"))
    chatdb = os.path.join(d, "chat.db")
    open(chatdb, "w").close()
    r = _run([
        "--dry-run", "--candidate", cand, "--config", cfg,
        "--trials", trials, "--chatdb", chatdb,
        "--gen-python", sys.executable, "--eval-python", sys.executable,
    ])
    assert r.returncode == 0, r.stdout + r.stderr
    assert f"serving:        {serv}" in r.stdout


def test_real_run_refuses_without_dry_run_flag_when_preconditions_fail():
    """Same preconditions, no --dry-run: must still refuse (never a partial
    run) and must NOT write --out."""
    d = tempfile.mkdtemp()
    cand = make_real_adapter(os.path.join(d, "candidate"))
    out = os.path.join(d, "out.json")
    r = _run([
        "--candidate", cand, "--serving", os.path.join(d, "nope"),
        "--trials", os.path.join(d, "nope.json"), "--out", out,
        "--gen-python", sys.executable, "--eval-python", sys.executable,
    ])
    assert r.returncode != 0
    assert "REFUSING" in r.stdout or "REFUSING" in r.stderr
    assert not os.path.exists(out)


def test_missing_candidate_flag_fails_fast():
    r = _run(["--dry-run"])
    assert r.returncode != 0
    assert "--candidate is required" in r.stdout + r.stderr


if __name__ == "__main__":
    raise SystemExit(pytest.main([__file__, "-v"]))
