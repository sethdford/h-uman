#!/usr/bin/env python3
"""Tests for fidelity_scorecard.py (W7-4)."""

import sqlite3
import subprocess
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))

import fidelity_scorecard as sc

SCRIPT = str(Path(__file__).parent / "fidelity_scorecard.py")


def test_synthesize_empty_is_safe():
    out = sc.synthesize_scorecard(None, None, None)
    assert "no sections ran" in out["headline"]
    # Even with nothing, the ground-truth recommendation is always present.
    assert any("TRUTH" in r for r in out["recommendations"])


def test_synthesize_starved_data_is_p0_and_favors_rag():
    audit = {"verdict": "starved", "stats": {"assistant_messages": 12}}
    out = sc.synthesize_scorecard(audit, None, None)
    assert "DATA=starved" in out["headline"]
    assert any(r.startswith("P0 DATA") for r in out["recommendations"])


def test_synthesize_names_weak_axes():
    axes = {"aggregate": 0.62, "weakest_axes": [{"axis": "rhythm", "score": 0.4},
                                                {"axis": "humor", "score": 0.5}]}
    out = sc.synthesize_scorecard(None, axes, None)
    assert "FIDELITY=0.62" in out["headline"]
    assert any("rhythm" in r and "humor" in r for r in out["recommendations"])


def test_synthesize_rag_winner_is_p0_approach():
    abr = {"overall_winner": "rag"}
    out = sc.synthesize_scorecard(None, None, abr)
    assert "APPROACH=rag" in out["headline"]
    assert any(r.startswith("P0 APPROACH") for r in out["recommendations"])


def test_recommendations_priority_sorted():
    out = sc.synthesize_scorecard(
        {"verdict": "starved", "stats": {"assistant_messages": 5}},
        {"aggregate": 0.5, "weakest_axes": [{"axis": "casing", "score": 0.3}]},
        {"overall_winner": "lora"},
    )
    prios = [r.split()[0] for r in out["recommendations"]]
    assert prios == sorted(prios), prios  # P0s before P1s before P2s


def test_end_to_end_cli_partial_run():
    """CLI with only a DB produces a scorecard with the audit section."""
    d = Path(tempfile.mkdtemp())
    db = d / "memory.db"
    conn = sqlite3.connect(db)
    conn.execute("CREATE TABLE messages (id INTEGER PRIMARY KEY, session_id TEXT, role TEXT, content TEXT)")
    conn.executemany(
        "INSERT INTO messages (session_id, role, content) VALUES (?,?,?)",
        [("s1", "user", "hey"), ("s1", "assistant", "yo whats good")],
    )
    conn.commit()
    conn.close()
    out = d / "scorecard.json"
    rc = subprocess.run([sys.executable, SCRIPT, "--db", str(db), "--output-json", str(out)],
                        capture_output=True, text=True)
    assert rc.returncode == 0, rc.stderr
    import json
    report = json.loads(out.read_text())
    assert "audit" in report and "scorecard" in report
    assert report["audit"]["verdict"] == "starved"


def main():
    tests = [
        test_synthesize_empty_is_safe,
        test_synthesize_starved_data_is_p0_and_favors_rag,
        test_synthesize_names_weak_axes,
        test_synthesize_rag_winner_is_p0_approach,
        test_recommendations_priority_sorted,
        test_end_to_end_cli_partial_run,
    ]
    print("Testing fidelity_scorecard.py")
    print("=" * 60)
    p = f = 0
    for t in tests:
        try:
            t()
            print(f"✓ {t.__name__}")
            p += 1
        except AssertionError as e:
            print(f"✗ {t.__name__}: {e}")
            f += 1
        except Exception as e:  # noqa: BLE001
            print(f"✗ {t.__name__}: {type(e).__name__}: {e}")
            f += 1
    print("=" * 60)
    print(f"Results: {p} passed, {f} failed")
    return 0 if f == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
