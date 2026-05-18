"""Sprint 11 / US-11.3 — `scripts/dpo_early_stop.py` test suite.

Covers AC-11.3.1 .. AC-11.3.5 with deterministic, offline tests:

  - AC-11.3.1: `--early-stopping-signal chosen_r` fires plateau-break.
  - AC-11.3.2: Sprint 8 fixture (`tests/fixtures/sprint8_dpo80_log.jsonl`)
    -> `first_breach_iter=65, stopped_iter=70, promoted_iter=60`.
  - AC-11.3.3: stable trajectory (within 20% of plateau mean) -> never fires.
  - AC-11.3.4: structured event JSON has the pinned schema.
  - AC-11.3.5: signal_mode='none' -> callback bypassed entirely.

Also smoke-tests the `subprocess.Popen` wrapper via a pure-Python
fake-proc seam — no real `mlx_lm_lora` invocation, no real model
weights, no network.
"""
from __future__ import annotations

import importlib.util
import io
import json
import pathlib
import sys
from typing import Iterable, List
from unittest.mock import MagicMock, patch

import pytest


# ── Module loader (hyphen-free filename so importable, but keep loader
#    for parity with `test_finetune_gemma_dpo.py`). ──
_HERE = pathlib.Path(__file__).resolve().parent
_SCRIPT = _HERE.parent / "scripts" / "dpo_early_stop.py"
_FIXTURE = _HERE / "fixtures" / "sprint8_dpo80_log.jsonl"


def _load_module():
    spec = importlib.util.spec_from_file_location("dpo_early_stop", _SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    # Register BEFORE exec so dataclass's `sys.modules.get(cls.__module__)`
    # lookup succeeds (Python 3.14 strictness).
    sys.modules["dpo_early_stop"] = mod
    spec.loader.exec_module(mod)
    return mod


es = _load_module()


def _load_fixture():
    assert _FIXTURE.exists(), f"fixture missing: {_FIXTURE}"
    rows = []
    for line in _FIXTURE.read_text().splitlines():
        line = line.strip()
        if not line:
            continue
        rows.append(json.loads(line))
    return rows


# ────────────────────────────────────────────────────────────────────────
# `parse_iter_line` regex coverage
# ────────────────────────────────────────────────────────────────────────
def test_parse_iter_line_matches_canonical_mlx_lm_lora_line():
    line = (
        "Iter 65: loss 0.009, chosen_r 3.170, rejected_r -21.920, "
        "acc 1.000, margin 25.090, lr 1.000e-05, it/s 1.252\n"
    )
    parsed = es.parse_iter_line(line)
    assert parsed == (65, 3.170)


def test_parse_iter_line_handles_negative_chosen_r():
    line = "Iter 80: loss 0.019, chosen_r -8.867, rejected_r -36.446, acc 1.000\n"
    assert es.parse_iter_line(line) == (80, -8.867)


def test_parse_iter_line_returns_none_for_val_line():
    # mlx-lm-lora's validation lines do NOT contain `chosen_r`.
    line = "Iter 20: Val loss 1.234, Val took 5.6s\n"
    assert es.parse_iter_line(line) is None


def test_parse_iter_line_returns_none_for_blank_and_garbage():
    assert es.parse_iter_line("") is None
    assert es.parse_iter_line("\n") is None
    assert es.parse_iter_line("Some progress bar: |####    | 50%\n") is None


# ────────────────────────────────────────────────────────────────────────
# AC-11.3.1 — plateau-break fires on a minimal synthetic trajectory
# ────────────────────────────────────────────────────────────────────────
def test_chosen_r_plateau_break_fires():
    """Two consecutive observations strictly below 50% of the trailing-5
    mean must produce a Decision with should_stop semantics (the API
    returns Optional[Decision]; non-None == should_stop)."""
    det = es.ChosenRPlateauDetector(
        window=5, drop_ratio=0.5, confirm_consecutive=2, save_every=20
    )
    # Stable plateau iters 5..25 around chosen_r = 10.0.
    stable = [(5, 10.0), (10, 10.0), (15, 10.0), (20, 10.0), (25, 10.0)]
    for it, cr in stable:
        assert det.observe(it, cr) is None, f"unexpected fire on stable {it}"
    # Cliff at iter 30 and iter 35: both < 5.0 (= 0.5 * 10.0).
    d30 = det.observe(30, 2.0)
    assert d30 is None, "single sub-threshold dip must not fire with confirm=2"
    d35 = det.observe(35, 1.5)
    assert d35 is not None, "two consecutive sub-threshold drops must fire"
    assert d35.reason == "chosen_r_plateau_break"
    assert d35.stopped_iter == 35
    assert d35.first_breach_iter == 30


# ────────────────────────────────────────────────────────────────────────
# AC-11.3.2 — Sprint 8 fixture replay produces the expected decision
# ────────────────────────────────────────────────────────────────────────
def test_sprint8_trajectory_stops_at_iter65():
    """Replay the captured Sprint 8 DPO-80 log; assert the detector fires
    at iter 70 with `first_breach_iter=65` and `promoted_iter=60`.

    The story text says "halt at iter 65" — with `confirm_consecutive=2`
    the FIRST breach is iter 65 and the decision FIRES at iter 70 once
    the second consecutive sub-threshold drop confirms. Both fields are
    emitted in the structured event; AC-11.3.2 asserts the breach iter."""
    det = es.ChosenRPlateauDetector(
        window=5, drop_ratio=0.5, confirm_consecutive=2, save_every=20
    )
    fired = None
    rows = _load_fixture()
    fire_index = None
    for i, row in enumerate(rows):
        d = det.observe(row["iter"], row["chosen_r"])
        if d is not None:
            fired = d
            fire_index = i
            break

    assert fired is not None, "expected fire on Sprint 8 fixture"
    assert fired.first_breach_iter == 65, (
        f"AC-11.3.2: first_breach_iter must be 65, got {fired.first_breach_iter}"
    )
    assert fired.stopped_iter == 70, (
        f"AC-11.3.2: stopped_iter must be 70 (one report-step after first "
        f"breach with confirm_consecutive=2), got {fired.stopped_iter}"
    )
    assert fired.promoted_iter == 60, (
        f"AC-11.3.4: promoted_iter must be 60 (most recent save_every=20 "
        f"boundary strictly before stopped_iter=70), got {fired.promoted_iter}"
    )
    assert fired.reason == "chosen_r_plateau_break"

    # Sanity: the warmup dip at iter 25/30 (chosen_r 4.303, 4.868) MUST
    # NOT have triggered a spurious fire earlier in the replay.
    assert fire_index is not None
    assert rows[fire_index]["iter"] == 70


# ────────────────────────────────────────────────────────────────────────
# AC-11.3.3 — stable trajectory never fires
# ────────────────────────────────────────────────────────────────────────
def test_stable_chosen_r_no_early_stop():
    """A run where chosen_r stays within 20% of its plateau mean for the
    full training horizon must never trigger a stop."""
    det = es.ChosenRPlateauDetector(
        window=5, drop_ratio=0.5, confirm_consecutive=2, save_every=20
    )
    # 80-iter trajectory; chosen_r oscillates within [8.0, 9.6] (plateau
    # mean ~ 8.8, 20% of plateau ~ 1.76 -> within band by definition).
    oscillation = [8.0, 9.6, 8.2, 9.4, 8.4, 9.2, 8.6, 9.0, 8.8, 8.8] * 2
    for i, cr in enumerate(oscillation):
        iter_n = (i + 1) * 5
        d = det.observe(iter_n, cr)
        assert d is None, f"unexpected fire on stable trajectory at iter {iter_n}"
    assert det.breach_count == 0
    assert det.history[-1] == (100, oscillation[-1])


def test_warmup_dip_does_not_fire_spuriously():
    """The Sprint 8 fixture has a single dip at iter 25 (chosen_r=4.303
    from a trailing mean ~ 17.2). With confirm_consecutive=2 the next
    iter (iter 30, chosen_r=4.868) is also below threshold — but the
    SUBSEQUENT iter 35 (chosen_r=9.199) is above, so the breach counter
    must RESET and the warmup dip must not strand the detector in a
    pre-fire state."""
    det = es.ChosenRPlateauDetector(
        window=5, drop_ratio=0.5, confirm_consecutive=2, save_every=20
    )
    rows = _load_fixture()
    # Feed only iters 5..40 (well before the real cliff at 65/70).
    for row in rows:
        if row["iter"] > 40:
            break
        d = det.observe(row["iter"], row["chosen_r"])
        # Some breach states may be set transiently, but no FIRE.
        assert d is None, f"warmup must not fire (saw iter {row['iter']})"
    # After iter 40 (chosen_r=8.921, above threshold), the breach counter
    # must have reset.
    assert det.breach_count == 0


# ────────────────────────────────────────────────────────────────────────
# AC-11.3.4 — structured event JSON schema is pinned
# ────────────────────────────────────────────────────────────────────────
def test_early_stop_log_format():
    """The firing event MUST be a single JSON object with the keys
    downstream tooling depends on. Lock the schema here so a future
    refactor cannot silently break the Pareto picker integration."""
    det = es.ChosenRPlateauDetector(
        window=5, drop_ratio=0.5, confirm_consecutive=2, save_every=20
    )
    rows = _load_fixture()
    fired = None
    for row in rows:
        d = det.observe(row["iter"], row["chosen_r"])
        if d is not None:
            fired = d
            break
    assert fired is not None
    event_line = det.format_event(fired)
    obj = json.loads(event_line)

    expected_keys = {
        "event",
        "reason",
        "stopped_iter",
        "first_breach_iter",
        "promoted_iter",
        "trailing_mean",
        "threshold",
        "chosen_r",
    }
    assert set(obj.keys()) == expected_keys, (
        f"AC-11.3.4: event schema drift; got {sorted(obj.keys())}"
    )
    assert obj["event"] == "early_stop"
    assert obj["reason"] == "chosen_r_plateau_break"
    assert obj["stopped_iter"] == 70
    assert obj["first_breach_iter"] == 65
    assert obj["promoted_iter"] == 60
    assert isinstance(obj["trailing_mean"], float)
    assert isinstance(obj["threshold"], float)
    assert isinstance(obj["chosen_r"], float)


# ────────────────────────────────────────────────────────────────────────
# AC-11.3.5 — signal_mode='none' is a no-op
# ────────────────────────────────────────────────────────────────────────
def test_disabled_signal_is_noop():
    """With `signal_mode='none'` the detector must never return a
    Decision regardless of input, preserving backward-compat with the
    Sprint 8 baseline CI scripts."""
    det = es.ChosenRPlateauDetector(
        window=5, drop_ratio=0.5, confirm_consecutive=2, save_every=20,
        signal_mode="none",
    )
    rows = _load_fixture()
    for row in rows:
        d = det.observe(row["iter"], row["chosen_r"])
        assert d is None, (
            f"signal_mode='none' must never fire (iter {row['iter']})"
        )
    # All observations were still recorded so we can inspect history if needed.
    assert len(det.history) == len(rows)


def test_invalid_signal_mode_rejected():
    with pytest.raises(ValueError):
        es.ChosenRPlateauDetector(signal_mode="persona_vector")


# ────────────────────────────────────────────────────────────────────────
# subprocess.Popen wrapper — fake-proc seam, no real spawn
# ────────────────────────────────────────────────────────────────────────
class _FakeProc:
    """Iterable-stdout fake of subprocess.Popen for hermetic testing."""

    def __init__(self, lines: Iterable[str], returncode: int = 0):
        self._lines = list(lines)
        self.stdout = iter(self._lines)
        self.returncode = returncode
        self.terminated = False
        self.killed = False

    def wait(self, timeout=None):
        return self.returncode

    def terminate(self):
        self.terminated = True

    def kill(self):
        self.killed = True


def _sprint8_stdout_lines():
    """Build the per-iter stdout lines the wrapper would see from
    mlx-lm-lora on the Sprint 8 fixture."""
    out = []
    for row in _load_fixture():
        out.append(
            f"Iter {row['iter']}: loss {row['loss']:.3f}, "
            f"chosen_r {row['chosen_r']:.3f}, "
            f"rejected_r {row['rejected_r']:.3f}, "
            f"acc {row['acc']:.3f}, margin {row['margin']:.3f}\n"
        )
    return out


def test_wrapper_streams_lines_and_fires_on_sprint8(tmp_path):
    """End-to-end test of `run_with_early_stop`: feed Sprint 8 lines
    through a fake Popen; assert SIGTERM was sent, sentinel + event log
    written, returned Decision matches the fixture."""
    det = es.ChosenRPlateauDetector(
        window=5, drop_ratio=0.5, confirm_consecutive=2, save_every=20
    )
    fake = _FakeProc(_sprint8_stdout_lines(), returncode=0)
    buf = io.StringIO()
    sentinel = tmp_path / "adapter" / ".early_stop"
    event_log = tmp_path / "adapter" / "early_stop_events.jsonl"

    rc, decision = es.run_with_early_stop(
        cmd=["python3", "-m", "mlx_lm_lora.train", "--fake"],
        detector=det,
        out_stream=buf,
        sentinel_path=sentinel,
        event_log_path=event_log,
        terminate_grace_sec=0.1,
        _popen=lambda *a, **kw: fake,
    )

    assert decision is not None, "wrapper must return the firing Decision"
    assert decision.first_breach_iter == 65
    assert decision.stopped_iter == 70
    assert decision.promoted_iter == 60
    assert fake.terminated, "wrapper must SIGTERM the subprocess on fire"
    assert sentinel.exists(), "sentinel file must be written"
    assert event_log.exists(), "event log must be written"
    # Echoed lines include the structured event in stdout buffer.
    echoed = buf.getvalue()
    assert "chosen_r 3.170" in echoed
    assert "early_stop" in echoed


def test_wrapper_signal_none_runs_to_completion(tmp_path):
    """With signal_mode='none' the wrapper must NEVER call terminate()
    even if the trajectory would have triggered a stop in chosen_r mode."""
    det = es.ChosenRPlateauDetector(
        window=5, drop_ratio=0.5, confirm_consecutive=2, save_every=20,
        signal_mode="none",
    )
    fake = _FakeProc(_sprint8_stdout_lines(), returncode=0)
    buf = io.StringIO()

    rc, decision = es.run_with_early_stop(
        cmd=["python3", "-m", "mlx_lm_lora.train", "--fake"],
        detector=det,
        out_stream=buf,
        sentinel_path=tmp_path / ".early_stop",
        event_log_path=tmp_path / "events.jsonl",
        terminate_grace_sec=0.1,
        _popen=lambda *a, **kw: fake,
    )

    assert decision is None, "signal_mode='none' must never fire"
    assert not fake.terminated, (
        "signal_mode='none' must NOT terminate the subprocess"
    )
    assert rc == 0
