#!/usr/bin/env python3
"""Sprint 11 / US-11.3 — DPO early-stopping by `train_chosen_r` plateau-break.

This module is a **log-parsing callback**, not a deep training-loop intercept.
It parses `mlx_lm_lora`'s per-iter reporting line and applies Rule A from
Sprint 8 `insights-addressed.md` (the empirically grounded proxy for the
Anthropic 2507.21509 persona-vector projection signal):

    Stop when current `chosen_r` < `drop_ratio` * trailing-window mean for
    `confirm_consecutive` consecutive evaluation steps.

The full Anthropic persona-vector projection (reading intermediate-layer
activations, projecting onto a precomputed user-voice direction) is
explicitly out of scope for Sprint 11 and deferred to Sprint 12.

The public surface is:
  - `parse_iter_line(line)` -> Optional[(iter, chosen_r)]
  - `ChosenRPlateauDetector(...)`
      - `observe(iter, chosen_r)` -> Optional[Decision] (None if continue)
      - `format_event(decision)` -> str (one-line JSON for downstream tooling)
  - `run_with_early_stop(cmd, detector, ...)` -> int (subprocess.Popen wrapper)

All behavior is offline-testable: feed observations directly into
`observe()` from a JSONL fixture; the subprocess seam is mocked.
"""
from __future__ import annotations

import json
import os
import re
import signal
import subprocess
import sys
from collections import deque
from dataclasses import dataclass, field, asdict
from pathlib import Path
from typing import Callable, Deque, List, Optional, Tuple


# ── Regex for `Iter N: ... chosen_r X.XXX ...` lines from mlx-lm-lora ──
# Sprint 8 `/tmp/dpo80.log` format:
#   Iter 5: loss 1.014, chosen_r 17.582, rejected_r 17.588, acc 0.400, ...
# Negative values are common after the cliff: chosen_r -4.500, -8.867.
# Validation lines (`Iter X: Val loss ...`) do NOT contain `chosen_r` so
# they fall through `parse_iter_line` -> None.
_ITER_RE = re.compile(r"Iter (\d+):\s*.*?chosen_r (-?\d+(?:\.\d+)?)")


def parse_iter_line(line: str) -> Optional[Tuple[int, float]]:
    """Extract (iter, chosen_r) from an mlx-lm-lora reporting line.

    Returns None for non-matching lines (val reports, progress bars,
    Python tracebacks, blank lines). The caller must treat None as
    "ignore this line; emit it to stdout but do not observe."
    """
    if not line:
        return None
    m = _ITER_RE.search(line)
    if not m:
        return None
    try:
        return int(m.group(1)), float(m.group(2))
    except (TypeError, ValueError):
        return None


@dataclass
class Decision:
    """The detector's verdict at one observation step.

    `should_stop=False` is wrapped in `Optional[Decision]` -> None in the
    detector API, so any returned Decision implies `should_stop=True`.

    Fields:
      reason             — fixed string for log-grep stability
      stopped_iter       — the iter at which the second breach confirmed
      first_breach_iter  — the iter at which the FIRST sub-threshold drop
                           occurred (one report-step before stopped_iter).
                           AC-11.3.2 asserts against this field.
      promoted_iter      — the most recent save_every boundary STRICTLY
                           before stopped_iter. This is the adapter that
                           the caller should keep; the adapter at
                           stopped_iter may be mid-checkpoint-write at
                           SIGTERM time.
      trailing_mean      — the trailing-window mean used to compute the
                           threshold at the firing step
      threshold          — drop_ratio * trailing_mean
      chosen_r           — the chosen_r observation that triggered the
                           second breach
    """
    reason: str
    stopped_iter: int
    first_breach_iter: int
    promoted_iter: int
    trailing_mean: float
    threshold: float
    chosen_r: float

    def to_event(self) -> dict:
        """Return the structured-log dict (AC-11.3.4 schema)."""
        return {
            "event": "early_stop",
            "reason": self.reason,
            "stopped_iter": self.stopped_iter,
            "first_breach_iter": self.first_breach_iter,
            "promoted_iter": self.promoted_iter,
            "trailing_mean": round(self.trailing_mean, 6),
            "threshold": round(self.threshold, 6),
            "chosen_r": round(self.chosen_r, 6),
        }


class ChosenRPlateauDetector:
    """Tracks `train_chosen_r` per evaluation step and applies Rule A.

    Rule A (from Sprint 8 insights-addressed.md, US-11.3 design §1):
        At each observation `(iter_n, chosen_r_n)`:
          1. Compute `trailing_mean = mean(prior `window` observations)`.
             If fewer than `window` prior observations exist, no decision.
          2. `threshold = drop_ratio * trailing_mean`.
          3. If `chosen_r_n < threshold`, increment breach counter.
             Record `first_breach_iter` on the first breach in the
             current run. Otherwise reset the breach counter.
          4. If breach counter reaches `confirm_consecutive`, FIRE.

    `signal_mode='none'` is a backward-compat pass-through (AC-11.3.5):
    `observe()` never returns a Decision regardless of input.

    The `save_every` parameter is used to compute `promoted_iter`:
        promoted_iter = floor((stopped_iter - 1) / save_every) * save_every
    i.e., the most recent save-checkpoint boundary STRICTLY BEFORE the
    stopped_iter. For Sprint 8 fixture: stopped_iter=70, save_every=20
    -> floor(69/20)*20 = 60.
    """

    def __init__(
        self,
        window: int = 5,
        drop_ratio: float = 0.5,
        confirm_consecutive: int = 2,
        save_every: int = 20,
        signal_mode: str = "chosen_r",
    ) -> None:
        if window < 1:
            raise ValueError(f"window must be >= 1 (got {window})")
        if not (0.0 < drop_ratio < 1.0):
            raise ValueError(f"drop_ratio must be in (0,1) (got {drop_ratio})")
        if confirm_consecutive < 1:
            raise ValueError(f"confirm_consecutive must be >= 1 (got {confirm_consecutive})")
        if save_every < 1:
            raise ValueError(f"save_every must be >= 1 (got {save_every})")
        if signal_mode not in ("chosen_r", "none"):
            raise ValueError(
                f"signal_mode must be 'chosen_r' or 'none' (got {signal_mode!r})"
            )
        self.window = window
        self.drop_ratio = drop_ratio
        self.confirm_consecutive = confirm_consecutive
        self.save_every = save_every
        self.signal_mode = signal_mode

        # Full history of (iter, chosen_r). We keep all observations rather
        # than a maxlen deque so debugging and the structured event can
        # reference any prior iter.
        self._history: List[Tuple[int, float]] = []
        self._breach_count: int = 0
        self._first_breach_iter: Optional[int] = None

    def observe(self, iter_n: int, chosen_r: float) -> Optional[Decision]:
        """Record one observation; return a Decision iff Rule A fires."""
        # AC-11.3.5: signal=='none' is a pure pass-through.
        if self.signal_mode == "none":
            self._history.append((iter_n, chosen_r))
            return None

        # We need at least `window` PRIOR observations to compute the
        # trailing mean. Before that, just accumulate.
        if len(self._history) < self.window:
            self._history.append((iter_n, chosen_r))
            return None

        # Trailing mean from the `window` most recent prior observations
        # (not including the current one). This is the cleanest reading
        # of AC-11.3.1 ("drops below 50% of its trailing-5-window mean").
        trailing_vals = [v for (_, v) in self._history[-self.window :]]
        trailing_mean = sum(trailing_vals) / float(self.window)
        threshold = self.drop_ratio * trailing_mean

        # Append BEFORE deciding — the current iter must be in history
        # when we report the decision (the fixture's stopped_iter is the
        # current iter).
        self._history.append((iter_n, chosen_r))

        # Edge case: if trailing_mean is non-positive, the threshold is
        # not meaningful (we can't say a chosen_r is < some non-positive
        # threshold in the way the rule intends — once the model has
        # collapsed, the trailing mean itself is below zero). In that
        # case, treat the threshold as already breached if the prior
        # observations also collapsed. We keep behavior conservative:
        # do NOT fire on a non-positive trailing mean — the cliff has
        # already happened and stopping now would just be late.
        if trailing_mean <= 0:
            # Reset breach state so we do not carry over a stale count.
            self._breach_count = 0
            self._first_breach_iter = None
            return None

        if chosen_r < threshold:
            # Breach.
            if self._breach_count == 0:
                self._first_breach_iter = iter_n
            self._breach_count += 1
            if self._breach_count >= self.confirm_consecutive:
                # FIRE. Compute promoted_iter as the latest save_every
                # boundary strictly less than stopped_iter.
                promoted_iter = ((iter_n - 1) // self.save_every) * self.save_every
                if promoted_iter < 0:
                    promoted_iter = 0
                # mypy: first_breach_iter must be set by now (it was set
                # on the first breach, which by definition preceded
                # the second; even if confirm_consecutive==1, the same
                # iter set it).
                fb = self._first_breach_iter
                assert fb is not None
                return Decision(
                    reason="chosen_r_plateau_break",
                    stopped_iter=iter_n,
                    first_breach_iter=fb,
                    promoted_iter=promoted_iter,
                    trailing_mean=trailing_mean,
                    threshold=threshold,
                    chosen_r=chosen_r,
                )
        else:
            # Not a breach — reset the consecutive counter. This is what
            # protects us from the iter-30 warmup dip false-positive.
            self._breach_count = 0
            self._first_breach_iter = None

        return None

    def format_event(self, decision: Decision) -> str:
        """Render the firing decision as a single JSON log line."""
        return json.dumps(decision.to_event(), sort_keys=True)

    # Test/debug accessors.
    @property
    def history(self) -> List[Tuple[int, float]]:
        return list(self._history)

    @property
    def breach_count(self) -> int:
        return self._breach_count


# ── subprocess.Popen wrapper used by finetune-gemma.py ──────────────────

def run_with_early_stop(
    cmd: List[str],
    detector: ChosenRPlateauDetector,
    cwd: Optional[str] = None,
    out_stream=None,
    sentinel_path: Optional[Path] = None,
    event_log_path: Optional[Path] = None,
    terminate_grace_sec: float = 10.0,
    _popen=subprocess.Popen,
) -> Tuple[int, Optional[Decision]]:
    """Run `cmd` and stream its stdout through `detector`.

    On fire:
      - Write a `.early_stop` sentinel file (if `sentinel_path` is set).
      - Append the structured JSON event to `event_log_path` (if set).
      - SIGTERM the subprocess; if it does not exit within
        `terminate_grace_sec`, SIGKILL.
    Returns (returncode, decision_or_None).

    `out_stream` is where we echo lines we read from the subprocess
    (defaults to sys.stdout). `_popen` is the injection seam for tests.

    The detector is consulted ONLY when its signal_mode is `chosen_r`.
    With signal_mode=='none', this function still streams stdout but
    never observes — the subprocess runs to completion.
    """
    if out_stream is None:
        out_stream = sys.stdout

    proc = _popen(
        cmd,
        cwd=cwd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        bufsize=1,
    )

    fired: Optional[Decision] = None
    try:
        stdout = getattr(proc, "stdout", None)
        if stdout is not None:
            for line in stdout:
                # Echo every line; preserves user-visible logging.
                try:
                    out_stream.write(line)
                    out_stream.flush()
                except Exception:
                    # Never let a stdout failure crash the wrapper.
                    pass

                if fired is not None:
                    # We have already fired; keep draining so the subprocess
                    # can exit cleanly. Do not call observe() again.
                    continue
                if detector.signal_mode == "none":
                    continue

                parsed = parse_iter_line(line)
                if parsed is None:
                    continue
                iter_n, chosen_r = parsed
                decision = detector.observe(iter_n, chosen_r)
                if decision is not None:
                    fired = decision
                    _on_fire(
                        decision,
                        detector,
                        out_stream=out_stream,
                        sentinel_path=sentinel_path,
                        event_log_path=event_log_path,
                    )
                    _terminate(proc, terminate_grace_sec)
    finally:
        rc = proc.wait()

    return rc, fired


def _on_fire(
    decision: Decision,
    detector: ChosenRPlateauDetector,
    out_stream,
    sentinel_path: Optional[Path],
    event_log_path: Optional[Path],
) -> None:
    """Emit the structured event log and write the sentinel."""
    event_line = detector.format_event(decision)
    try:
        out_stream.write(event_line + "\n")
        out_stream.flush()
    except Exception:
        pass
    if event_log_path is not None:
        try:
            event_log_path.parent.mkdir(parents=True, exist_ok=True)
            with open(event_log_path, "a") as f:
                f.write(event_line + "\n")
        except OSError:
            pass
    if sentinel_path is not None:
        try:
            sentinel_path.parent.mkdir(parents=True, exist_ok=True)
            with open(sentinel_path, "w") as f:
                json.dump(decision.to_event(), f, sort_keys=True)
        except OSError:
            pass


def _terminate(proc, grace_sec: float) -> None:
    """Send SIGTERM; if grace expires, SIGKILL."""
    try:
        proc.terminate()
    except Exception:
        return
    try:
        proc.wait(timeout=grace_sec)
    except subprocess.TimeoutExpired:
        try:
            proc.kill()
        except Exception:
            pass


__all__ = [
    "parse_iter_line",
    "Decision",
    "ChosenRPlateauDetector",
    "run_with_early_stop",
]
