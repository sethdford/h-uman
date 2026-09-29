#!/usr/bin/env python3
"""Pins for the 2026-07-27 nightly's cross-family measurement fault.

That night's run produced `verdict: SKIP, score: null` on the GLM adapter —
which reads as "the adapter didn't improve anything". It wasn't. The 03:10
nightly raced the mlx-server restart window, so the live-process probe found
nothing; the ADAPTER half of resolution fell back to config.json (GLM), while
the BASE half fell back to a hardcoded gemma constant. A GLM-shaped LoRA
cannot bind to gemma weights, so no delta was ever applied:

    pre.mean_score  = 0.3178
    post.mean_score = 0.3178      <- identical to 4dp; the adapter was a no-op

Two separable defects, both pinned here:
  1. asymmetric fallback — the base half ignored config.json's mlx_local.model
     (which was correct and sitting right there)
  2. no cross-family guard — a pair that CANNOT produce a delta still emitted
     a verdict instead of deferring

Lives in its own file rather than test_eval_fidelity_nightly.py because that
file was being edited by a concurrent session at the time (05a0f0d99).
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import eval_fidelity_nightly as e  # noqa: E402

fails = 0


def ok(name, cond, detail=""):
    global fails
    print(("  PASS  " if cond else "  FAIL  ") + name + (f"  {detail}" if detail and not cond else ""))
    if not cond:
        fails += 1


# ── family tagging is conservative: unknown never fabricates a mismatch ──
ok("glm base id -> glm", e.model_family("mlx-community/GLM-4.5-Air-4bit") == "glm")
ok("gemma base id -> gemma", e.model_family("mlx-community/gemma-4-31b-it-4bit") == "gemma")
ok("glm adapter path -> glm", e.model_family("/a/seth-glm-air-v5-20260725-093742") == "glm")
ok("unfamiliar name -> None", e.model_family("seth-lora-v9-experimental") is None)
ok("None -> None", e.model_family(None) is None)

# ── the mismatch predicate ──────────────────────────────────────────────
ok("THE 07-27 PAIRING: gemma base + glm adapter is a mismatch",
   e.base_adapter_family_mismatch("mlx-community/gemma-4-31b-it-4bit",
                                  "/a/seth-glm-air-v5-20260725-093742"))
ok("matched glm pair is not a mismatch",
   not e.base_adapter_family_mismatch("mlx-community/GLM-4.5-Air-4bit",
                                      "/a/seth-glm-air-v5-20260725-093742"))
ok("matched gemma pair is not a mismatch",
   not e.base_adapter_family_mismatch("mlx-community/gemma-4-31b-it-8bit",
                                      "/a/seth-lora-v5-8bit-20260718"))
ok("unknown adapter naming never blocks a run",
   not e.base_adapter_family_mismatch("mlx-community/GLM-4.5-Air-4bit",
                                      "/a/seth-lora-v9-experimental"))

# ── server-down fallback reads CONFIG, not the hardcoded constant ───────
# ps_output="" simulates the 03:10 restart window that caused the fault.
#
# HERMETIC: a synthetic config with a sentinel base, not the live machine's.
# Reading the real config recomputes the same chain production computes, which
# only discriminates while config != DEFAULT_MODEL — revert the base to gemma,
# or run where no config exists (CI), and the assertion silently becomes a
# tautology. Caught in cross-session review, 2026-07-27.
import contextlib
import json
import tempfile

SENTINEL_CONFIG_BASE = "test-only/base-from-config"


@contextlib.contextmanager
def config_override(model):
    """Point the resolver at a synthetic config; model=None → no config file."""
    saved = e.DEFAULT_CONFIG_PATH
    with tempfile.TemporaryDirectory() as td:
        path = Path(td) / "config.json"
        if model is not None:
            path.write_text(json.dumps({"mlx_local": {"model": model}}))
        try:
            e.DEFAULT_CONFIG_PATH = path
            yield
        finally:
            e.DEFAULT_CONFIG_PATH = saved


with config_override(SENTINEL_CONFIG_BASE):
    resolved = e.resolve_serving_model(ps_output="")
ok("server-down resolves to config mlx_local.model, never DEFAULT_MODEL",
   resolved == SENTINEL_CONFIG_BASE, f"got {resolved!r}")

with config_override(None):
    last_resort = e.resolve_serving_model(ps_output="")
ok("no config at all -> DEFAULT_MODEL (documented last resort)",
   last_resort == e.DEFAULT_MODEL, f"got {last_resort!r}")

print(("FAILED" if fails else "PASSED") + f" ({fails} failures)")
sys.exit(1 if fails else 0)
