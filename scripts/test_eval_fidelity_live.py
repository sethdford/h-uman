#!/usr/bin/env python3
"""Tests for eval_fidelity_live.py (AC-11).

Covers the pure path-decision logic and an end-to-end headless run (--dry-run)
asserting the verdict JSON carries `path_used` and a numeric `mean_score`.
"""

import json
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import eval_fidelity_live as live  # noqa: E402

SCRIPT = str(Path(__file__).parent / "eval_fidelity_live.py")


def test_determine_path_off_is_cloud():
    assert live.determine_path("off", True, True) == "cloud"


def test_determine_path_force_is_local():
    # FORCE routes local even when the server looks unreachable (fallback covers it).
    assert live.determine_path("force", False, False) == "local"


def test_determine_path_auto_local_when_ready():
    assert live.determine_path("auto", True, True) == "local"


def test_determine_path_auto_cloud_when_no_adapter():
    assert live.determine_path("auto", False, True) == "cloud"


def test_determine_path_auto_cloud_when_server_down():
    assert live.determine_path("auto", True, False) == "cloud"


def _write_fixtures(d: Path) -> Path:
    fx = d / "fixtures.jsonl"
    rows = [
        {"prompt": "you around tonight?", "reference": "yeah for a bit, what's up"},
        {"prompt": "did you eat?", "reference": "nah not yet, gonna grab something soon"},
    ]
    fx.write_text("\n".join(json.dumps(r) for r in rows) + "\n")
    return fx


def test_dry_run_produces_path_used_and_score():
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)
        fx = _write_fixtures(d)
        out = d / "verdict.json"
        rc = subprocess.run(
            [
                sys.executable,
                SCRIPT,
                "--fixtures",
                str(fx),
                "--path",
                "local",
                "--dry-run",
                "--output-json",
                str(out),
            ],
            capture_output=True,
            text=True,
        )
        assert rc.returncode in (0, 1), f"unexpected exit {rc.returncode}: {rc.stderr}"
        verdict = json.loads(out.read_text())
        assert verdict["path_used"] == "local", verdict
        assert isinstance(verdict["mean_score"], (int, float)), verdict
        assert verdict["n"] == 2, verdict


def test_dry_run_auto_path_resolves():
    # auto + dry-run treats the server as reachable; with an adapter path that
    # doesn't exist, auto resolves to cloud.
    with tempfile.TemporaryDirectory() as td:
        d = Path(td)
        fx = _write_fixtures(d)
        out = d / "verdict.json"
        subprocess.run(
            [
                sys.executable,
                SCRIPT,
                "--fixtures",
                str(fx),
                "--path",
                "auto",
                "--routing-mode",
                "auto",
                "--dry-run",
                "--output-json",
                str(out),
            ],
            capture_output=True,
            text=True,
        )
        verdict = json.loads(out.read_text())
        assert verdict["path_used"] in ("local", "cloud"), verdict
        assert "mean_score" in verdict, verdict


def main():
    tests = [
        test_determine_path_off_is_cloud,
        test_determine_path_force_is_local,
        test_determine_path_auto_local_when_ready,
        test_determine_path_auto_cloud_when_no_adapter,
        test_determine_path_auto_cloud_when_server_down,
        test_dry_run_produces_path_used_and_score,
        test_dry_run_auto_path_resolves,
    ]
    print("Testing eval_fidelity_live.py")
    print("=" * 60)
    passed = failed = 0
    for test in tests:
        try:
            test()
            print(f"✓ {test.__name__}")
            passed += 1
        except AssertionError as e:
            print(f"✗ {test.__name__}: {e}")
            failed += 1
        except Exception as e:  # noqa: BLE001
            print(f"✗ {test.__name__}: {type(e).__name__}: {e}")
            failed += 1
    print("=" * 60)
    print(f"Results: {passed} passed, {failed} failed")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
