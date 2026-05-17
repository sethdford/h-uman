# Sprint 11 — Follow-up tasks

Findings from critic + panel that don't block story close but should land in Sprint 12.

## P0 (none outstanding — US-11.2 fix landed inline at `60a24b75`)

## P1 — Sprint 12

### FU-11.2.a — `--resume` + `--train-type dora` preflight guard
Source: US-11.2 critic MED.
The `run_sft` resume path silently proceeds with a DoRA training run on top of a LoRA-shaped `adapters.safetensors`. mlx-lm-lora will surface an error at runtime, not at argparse time. Add a preflight check that warns when `--resume` is combined with a different `train_type` than the prior adapter's metadata records.

### FU-11.2.b — AC-11.2.4 doesn't actually verify DoRA loadability
Source: US-11.2 critic MED.
`check-lora-baseline.sh` runs `human ml lora-baseline` against a persona fixture — does not load the safetensors adapter at all. So AC-11.2.4 ("DoRA adapter passes the gate") is vacuously satisfied. Need a separate test that loads the DoRA adapter via mlx_lm and confirms shape.

### FU-11.3.a — `_on_fire` silently swallows OSError on sentinel/log write
Source: US-11.3 critic HIGH-1.
If the adapter directory is read-only or filesystem full, the stop is never durably recorded but `_terminate` still fires. Downstream Pareto picker / US-11.6 reads no `.early_stop` and treats the run as un-stopped. Fix: propagate OSError OR write to stderr + return non-zero from `run_with_early_stop`.

### FU-11.3.b — `proc.stdout is None` silent exit
Source: US-11.3 critic HIGH-2.
If the subprocess is created but `stdout` is somehow falsy (broken mock or future Popen subclass), wrapper silently exits with `fired=None` without parsing any line. Subprocess not terminated. Fix: raise RuntimeError if `proc.stdout is None` immediately after `_popen()` returns.

### FU-11.3.c — Missing 20%-iter sanity assertion
Source: US-11.3 panel FLAG + critic MED. (Also documented in design Risk 2.)
If `mlx_lm_lora` ever reformats output, `parse_iter_line` silently returns None forever; detector never observes; no early stop ever fires. Add: after a configurable `parse_warn_after` number of lines with zero successful parses, write a warning to `out_stream` and optionally abort.

### FU-11.3.d — Asymmetric sentinel (w) vs event log (a) write modes
Source: US-11.3 critic MED.
On re-run of the same adapter dir, event log accumulates while sentinel holds only the latest. Downstream parsers may mis-count. Fix: document the asymmetry OR clear log at start of `run_with_early_stop`.

### FU-11.3.e — `assert fb is not None` in production firing path
Source: US-11.3 critic MED.
`assert` is a no-op under `python -O`. Replace with `if fb is None: raise RuntimeError(...)`.

### FU-11.3.f — Missing test for `run_train_all` / `run_speculative_draft_training` forwarding of `early_stopping_signal`
Source: US-11.3 critic LOW.
Add one test that asserts the synthesized Namespace correctly forwards the flag through these internal callers.

### FU-11.3.g — Design doc trailing-mean off-by-one
Source: US-11.3 critic LOW.
Design says window mean = 8.848, actual is 8.4776 (iter 60 is already in history by the time iter 65 is observed). Both thresholds trigger the breach so tests pass, but design doc is wrong. Add `pytest.approx` assertion on `trailing_mean` + `threshold` to lock the correct values.

## Cross-cutting risk for Wave 1 merge review

US-11.3 restructured `run_dpo`'s subprocess call into a branch (es_signal == "chosen_r" vs else). Both US-11.1 (env=child_env) and US-11.4 (DPOP) need to keep BOTH branches in sync. The Wave 1 merge MUST verify cmd construction, env, and cwd are identical in both branches.
