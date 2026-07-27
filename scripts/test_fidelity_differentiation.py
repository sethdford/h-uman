#!/usr/bin/env python3
"""Pins that the nightly's two passes are actually DIFFERENT runs.

The fidelity nightly's entire claim is a comparison: PRE = base alone, POST =
base + adapter. If the adapter never binds, both passes run the identical
configuration, every score matches to full precision, and the delta is a
structural zero. The harness used to record that as `verdict: SKIP` — "the
adapter measurably did not help" — which is a statement about the model made
from evidence about the plumbing.

Every degenerate night in the archive has the same fingerprint:

    2026-07-12 .. 07-25   pre == post == 1.0      14 nights, all sentinels
    2026-07-27 03:14      pre == post == 0.3178   GLM adapter on a gemma base

Why it is silent: mlx_lm's tuner.utils.load_adapters ends in

    model.load_weights(adapter_path / "adapters.safetensors", strict=False)

so an adapter whose tensor keys do not match the base loads ZERO weights with
no error. LoRALinear zero-initialises `lora_b`, so the LoRA branch computes
`(x @ lora_a) @ 0 == 0` and the "adapted" model is bit-identical to base.
The family guard (test_fidelity_family_guard.py) catches only the case where
the two NAMES disagree; these pins cover the general observed no-op, including
the same-family mismatches no name check can see.

Stdlib-only and hermetic — no mlx, no model, no GPU, no ~/.human reads.
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


# ── the no-op detector ──────────────────────────────────────────────────
# Written as positive contracts (per tests-that-pin-bugs.md): each asserts the
# CORRECT behavior, so each fails against the pre-fix harness rather than
# codifying what it used to do.

BASE_RUN = ["yeah just got back", "ha no way", "sounds good", "im down"]

ok("THE 07-27 SHAPE: every prompt byte-identical -> no-op detected",
   e.responses_identical(list(BASE_RUN), list(BASE_RUN)))

ok("THE 07-12..07-25 SHAPE: identical sentinels -> no-op detected",
   e.responses_identical(["[timeout]"] * 29, ["[timeout]"] * 29))

# The discriminating case. A single differing prompt proves the LoRA bound and
# perturbed at least one greedy decode — that is a real run and must proceed.
# If this returns True the guard is not a differentiation check, it is a
# blanket DEFER, and every legitimate nightly would be thrown away.
bound = list(BASE_RUN)
bound[2] = "sounds good man"
ok("one differing prompt -> NOT a no-op (a bound adapter must not be deferred)",
   not e.responses_identical(list(BASE_RUN), bound))

ok("wholly different outputs -> NOT a no-op",
   not e.responses_identical(BASE_RUN, ["a", "b", "c", "d"]))

ok("whitespace-only difference still counts as differentiated",
   not e.responses_identical(["yeah"], ["yeah "]))

# Degenerate inputs must never be reported as a detected no-op: "no data" is
# not "no delta", and conflating them would DEFER on an empty run for the
# wrong stated reason.
ok("empty pass lists are not a no-op verdict", not e.responses_identical([], []))
ok("length mismatch is not a no-op verdict",
   not e.responses_identical(["a"], ["a", "b"]))

# ── the PRE pass really reaches mlx_lm.load() with no adapter ───────────
# Not a label check: this drives the real run_eval_pass and records what the
# model loader was handed. The failure it guards against is an adapter reaching
# the PRE load by any route — the shape that bit mlx-server.py, where a config
# inheritance silently applied a gemma adapter to GLM.
import json          # noqa: E402
import tempfile      # noqa: E402

loaded_with = []


def _fake_load_model(model_id, adapter_path=None, timeout_sec=None):
    loaded_with.append(adapter_path)
    return ("MODEL", "TOKENIZER")


_saved_load, _saved_gen = e.load_model, e._mlx_generate
try:
    e.load_model = _fake_load_model
    e._mlx_generate = lambda model, tok, prompt, max_tokens: f"reply to {prompt}"
    pre_resp, pre_stats = e.run_eval_pass("base/model", [{"prompt": "hey"}])
    post_resp, post_stats = e.run_eval_pass("base/model", [{"prompt": "hey"}],
                                            adapter_path="/a/seth-glm-air-v5")
finally:
    e.load_model, e._mlx_generate = _saved_load, _saved_gen

ok("PRE pass loads the model with adapter_path=None",
   loaded_with[0] is None, f"got {loaded_with[0]!r}")
ok("POST pass loads the model WITH the adapter",
   loaded_with[1] == "/a/seth-glm-air-v5", f"got {loaded_with[1]!r}")
ok("passes are labelled from what they were actually given",
   pre_stats["pass"].startswith("PRE") and post_stats["pass"].startswith("POST"),
   f"{pre_stats['pass']!r} / {post_stats['pass']!r}")
ok("both passes generated a response", len(pre_resp) == len(post_resp) == 1)

with tempfile.TemporaryDirectory() as td:
    adapter = Path(td) / "seth-glm-air-v5"
    adapter.mkdir()
    (adapter / "adapter_config.json").write_text(
        json.dumps({"model": "mlx-community/GLM-4.5-Air-4bit",
                    "lora_parameters": {"rank": 8, "scale": 2.0}}))
    ok("declared base is read from adapter_config.json",
       e.adapter_declared_base(adapter) == "mlx-community/GLM-4.5-Air-4bit",
       f"got {e.adapter_declared_base(adapter)!r}")

    # Provenance is EXACT where model_family() is a substring heuristic: both
    # of these are family 'glm', so only the declared base separates them.
    ok("declared base distinguishes quantizations the family tag cannot",
       e.model_family("mlx-community/GLM-4.5-Air-8bit") ==
       e.model_family(str(adapter)) ==
       "glm"
       and e.adapter_declared_base(adapter) != "mlx-community/GLM-4.5-Air-8bit")

with tempfile.TemporaryDirectory() as td:
    bare = Path(td) / "no-config"
    bare.mkdir()
    ok("missing adapter_config.json -> None, never a fabricated base",
       e.adapter_declared_base(bare) is None)
    ok("nonexistent path -> None", e.adapter_declared_base(Path(td) / "nope") is None)

# ── end-to-end: main() actually WIRES the predicate ─────────────────────
# The unit pins above prove responses_identical is correct; they cannot prove
# main() calls it. A guard defined and unit-tested but never invoked is the
# uncalled-function shape from integration-done-contract.md — it would pass
# every assertion above while the nightly kept emitting SKIP. This drives the
# real main() with generation mocked out: no mlx, no GPU, no :8741.

def _drive(gen, adapter_dir, fixture, log_dir, label):
    """Run main() end-to-end against a mocked generator; return (rc, verdict)."""
    import contextlib as _ctx
    import io as _io
    out_json = log_dir / f"verdict-{label}.json"
    saved_argv, saved_load, saved_gen = sys.argv, e.load_model, e._mlx_generate
    try:
        # tokenizer slot carries adapter_path so the fake generator can tell
        # the two passes apart the way a real bound LoRA would.
        e.load_model = lambda mid, adapter_path=None, timeout_sec=None: ("M", adapter_path)
        e._mlx_generate = gen
        sys.argv = ["prog", "--adapter-path", str(adapter_dir),
                    "--model-id", "mlx-community/GLM-4.5-Air-4bit",
                    "--held-out-fixture", str(fixture), "--min-prompts", "3",
                    "--output-json", str(out_json), "--log-dir", str(log_dir),
                    "--no-registry"]
        with _ctx.redirect_stdout(_io.StringIO()):
            rc = e.main()
    finally:
        sys.argv, e.load_model, e._mlx_generate = saved_argv, saved_load, saved_gen
    return rc, json.loads(out_json.read_text())


with tempfile.TemporaryDirectory() as td:
    td = Path(td)
    adapter_dir = td / "seth-glm-air-v5-fake"
    adapter_dir.mkdir()
    (adapter_dir / "adapter_config.json").write_text(
        json.dumps({"model": "mlx-community/GLM-4.5-Air-4bit",
                    "lora_parameters": {"rank": 8}}))
    fixture = td / "prompts.jsonl"
    fixture.write_text("\n".join(json.dumps({"prompt": f"prompt {i}"}) for i in range(5)))

    # A silently non-binding adapter: identical output with or without it.
    rc, verdict = _drive(lambda m, tok, p, max_tokens: f"yeah {p}",
                         adapter_dir, fixture, td, "noop")
    ok("no-op adapter DEFERS (exit 2), never records a SKIP verdict",
       (rc, verdict["verdict"]) == (e.EXIT_DEFERRED, "DEFERRED"),
       f"got exit={rc} verdict={verdict['verdict']!r}")
    ok("the deferral reason names the byte-identical output",
       "byte-identical" in verdict.get("reason", ""),
       f"got {verdict.get('reason', '')[:80]!r}")

    # A genuinely bound adapter must still be measured — the guard must not
    # swallow real runs. POST differs because the tokenizer slot is non-None.
    rc, verdict = _drive(
        lambda m, tok, p, max_tokens: (f"yeah {p} lol" if tok
                                       else f"Certainly! I am happy to help with {p}."),
        adapter_dir, fixture, td, "bound")
    ok("a bound adapter is measured, not deferred",
       rc != e.EXIT_DEFERRED and verdict["verdict"] != "DEFERRED",
       f"got exit={rc} verdict={verdict['verdict']!r}")
    ok("every measured verdict carries differentiation evidence",
       verdict.get("differentiation", {}).get("n_differing_pairs") == 5,
       f"got {verdict.get('differentiation')!r}")


print(("FAILED" if fails else "PASSED") + f" ({fails} failures)")
sys.exit(1 if fails else 0)
