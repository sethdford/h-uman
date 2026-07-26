#!/usr/bin/env python3
"""Pins for training_loop's scale honesty + evidence-required verdict.

Both pin 2026-07-26 recovery-run findings:
  1. The mlx config used flat lora_scale keys that mlx_lm silently ignores —
     the adapter trained at the catastrophic default scale=20.0 while the
     yaml said 2.0 (rules/lora-scale-default-or-die.md, second real instance).
  2. The regression gate printed "PASS (val_loss=None)" — a verdict with no
     evidence (the toothless-gate shape from the 2026-07-11 fleet lessons).
"""
import json
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import training_loop  # noqa: E402

fails = 0


def ok(name, cond, detail=""):
    global fails
    print(("  PASS  " if cond else "  FAIL  ") + name + ("  " + detail if detail and not cond else ""))
    if not cond:
        fails += 1


# ── 1. config emission uses the nested schema mlx_lm actually reads ──────
import inspect
src = inspect.getsource(training_loop.run_mlx_lora_training)
ok("config uses nested lora_parameters", '"lora_parameters"' in src)
ok("flat lora_scale key removed", '"lora_scale": scale' not in src)

# ── 2. read_adapter_scale reads what mlx_lm recorded ─────────────────────
with tempfile.TemporaryDirectory() as d:
    ok("missing config -> None", training_loop.read_adapter_scale(Path(d)) is None)
    (Path(d) / "adapter_config.json").write_text(json.dumps(
        {"lora_parameters": {"rank": 8, "scale": 20.0, "dropout": 0.0}}))
    ok("catastrophic default is visible, not hidden",
       training_loop.read_adapter_scale(Path(d)) == 20.0)
    (Path(d) / "adapter_config.json").write_text(json.dumps(
        {"lora_parameters": {"rank": 8, "scale": 2.0, "dropout": 0.0}}))
    ok("requested scale round-trips", training_loop.read_adapter_scale(Path(d)) == 2.0)
    (Path(d) / "adapter_config.json").write_text("not json {")
    ok("corrupt config -> None, no crash",
       training_loop.read_adapter_scale(Path(d)) is None)

# ── 3. verdict source: None val_loss must route to INCONCLUSIVE ──────────
src_main = inspect.getsource(training_loop)
ok("None val_loss routes to INCONCLUSIVE verdict",
   "verdict = 'INCONCLUSIVE'" in src_main)
ok("INCONCLUSIVE blocks the swap (returns 1)",
   "if verdict == 'INCONCLUSIVE':" in src_main)

print(("FAILED" if fails else "PASSED") + f" ({fails} failures)")
sys.exit(1 if fails else 0)
