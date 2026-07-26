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
# Asserted against the EMITTED config, not against the source text of one
# function: the emitter moved to training_config_for_model() in 614137d32 and
# a source-grep pin silently stopped covering anything. Behavioural pins
# survive the code moving; textual ones quietly stop being pins.
import inspect

for _model, _label in (("mlx-community/gemma-4-31b-it-4bit", "gemma"),
                       ("mlx-community/GLM-4.5-Air-4bit", "glm")):
    cfg = training_loop.training_config_for_model(_model, iters=10, scale=2.0)
    ok(f"[{_label}] config uses nested lora_parameters",
       isinstance(cfg.get("lora_parameters"), dict))
    ok(f"[{_label}] nested scale is the requested one",
       cfg.get("lora_parameters", {}).get("scale") == 2.0)
    # The flat keys are the bug: mlx_lm 0.31.x ignores them, so their presence
    # means the scale silently reverts to the catastrophic 20.0 default.
    ok(f"[{_label}] no flat lora_* keys survive",
       not any(k in cfg for k in ("lora_scale", "lora_alpha", "lora_rank")))

# Prod serves GLM since 2026-07-26 — the GLM branch must actually differ, or
# the recipe that produced seth-glm-air-v5 is not the one being replayed.
_glm = training_loop.training_config_for_model("mlx-community/GLM-4.5-Air-4bit", 10, 2.0)
ok("glm recipe enables grad checkpointing (106B MoE fits)",
   _glm.get("grad_checkpoint") is True)

# Wiring pin: the emitter above is worthless if the training path stopped
# calling it (integration-done-contract — a fix with no caller is dead code).
ok("run_mlx_lora_training delegates to training_config_for_model",
   "training_config_for_model(" in inspect.getsource(training_loop.run_mlx_lora_training))

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

# ── 4. sub-minimum batches are refused BEFORE any GPU is spent ───────────
# "Refuse early" has three separable claims, and the cheap one (non-zero rc)
# is the least important:
#   - the trainer is never invoked at all (that is the "early" part)
#   - no adapters.safetensors is left behind (nothing for the C dispatcher to
#     hot-swap; lora_training_runner.c:407 POSTs /v1/adapters/swap on exit 0
#     with NO check on adapter contents, so a stray empty adapter here would
#     strip the persona weights off the live mlx-server)
#   - the exit code is non-zero, so that dispatcher bails with HU_ERR_IO
import sqlite3
from types import SimpleNamespace

with tempfile.TemporaryDirectory() as td:
    tmp = Path(td)
    db = tmp / "memory.db"
    conn = sqlite3.connect(str(db))
    conn.execute("CREATE TABLE messages (id INTEGER PRIMARY KEY, session_id TEXT, "
                 "role TEXT, content BLOB, created_at INTEGER)")
    _p, _r = "you up?", "yeah what's good"
    conn.execute("INSERT INTO messages VALUES (1,'s','user',?,1)", (_p,))
    conn.execute("INSERT INTO messages VALUES (2,'s','assistant',?,2)", (_r,))
    conn.commit()
    conn.close()

    # Exactly ONE outcome — the degenerate case: index 0 is the only index and
    # 0 % 10 == 0, so the 90/10 split leaves the train side empty.
    jsonl = tmp / "outcomes.jsonl"
    jsonl.write_text(json.dumps({
        "t": 1, "l": 10, "pt": 5, "ct": 5, "m": 1, "a": 1, "g": 0,
        "ph": training_loop.fnv1a_64(_p.encode()),
        "rh": training_loop.fnv1a_64(_r.encode()),
    }) + "\n")

    out = tmp / "auto-99"
    calls = []

    def _tripwire(*a, **kw):
        calls.append((a, kw))
        raise AssertionError("run_mlx_lora_training must not be reached")

    _saved = {k: getattr(training_loop, k) for k in (
        "resolve_serving_base_model", "resolve_serving_adapter",
        "run_mlx_lora_training", "append_lineage_entry",
        "dpo_results", "adapter_registry")}
    try:
        training_loop.resolve_serving_base_model = (
            lambda **kw: ("mlx-community/GLM-4.5-Air-4bit", "test-injected"))
        training_loop.resolve_serving_adapter = (
            lambda **kw: (tmp / "serving-ref", "test-injected"))
        training_loop.run_mlx_lora_training = _tripwire
        training_loop.append_lineage_entry = lambda *a, **kw: None
        training_loop.dpo_results = SimpleNamespace(
            append_result=lambda *a, **kw: None,
            load_recent=lambda *a, **kw: [],
            regression_verdict=lambda *a, **kw: "PASS",
            get_git_commit=lambda: "test",
            parse_mlx_losses=lambda o: (None, None))
        training_loop.adapter_registry = SimpleNamespace(
            record_training=lambda **kw: None)

        rc = training_loop.train_from_outcomes(jsonl, out, db, dry_run=False)
    finally:
        for _k, _v in _saved.items():
            setattr(training_loop, _k, _v)

    ok("1-outcome batch exits non-zero (C dispatcher bails, no swap)", rc != 0)
    ok("trainer never invoked — no GPU spent", calls == [])
    # Both the requested and base-suffixed paths: neither may hold something
    # the swap could pick up.
    ok("no swappable adapter left at requested path",
       not (out / "adapters.safetensors").exists())
    ok("no swappable adapter left at suffixed path",
       not any(p.name == "adapters.safetensors" for p in tmp.rglob("*")))

    # MIN_TRAINABLE_OUTCOMES is the floor for "splittable", so 2 must be the
    # first accepted size — pin the boundary, not just the rejected side.
    ok("minimum is the split floor (2), not an arbitrary number",
       training_loop.MIN_TRAINABLE_OUTCOMES == 2)

print(("FAILED" if fails else "PASSED") + f" ({fails} failures)")
sys.exit(1 if fails else 0)
