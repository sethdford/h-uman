# Sprint 7 — Follow-up Tasks

Findings the gates surfaced that are not in-scope for the current sprint but
should land before Sprint 8 or as part of Sprint 8 if not addressed in-sprint.

## P0 — must address before Sprint 7 closes

None as of Wave 0 close. (US-7.2's 4 HIGH findings are being fixed via
re-dispatch, not deferred.)

## P1 — should address in-sprint or early Sprint 8

### FU-7.6.a — `check-lora-ab.sh --judgment` silent-pass when STATUS empty
- **Source:** US-7.6 critic (HAS_FINDINGS severity=HIGH); accepted with
  follow-up per stakeholder decision.
- **Reproduction:** when `human ml fidelity-status --judgment` produces no
  JSON output (binary missing, OOM, wrong path), `STATUS` is empty, the
  script falls through to `exit 0` and reports a spurious PASS.
- **Fix sketch:** in `scripts/check-lora-ab.sh`, when `STATUS` is empty,
  `exit 1` (or emit SKIP on stdout so grep-based automation sees it).
- **Risk if deferred:** US-7.5 nightly cron could promote an adapter when
  the judgment gate is silently failing.
- **Owner:** next implementer to touch `scripts/check-lora-ab.sh`. Most
  likely US-7.4's implementer or a Sprint 8 cleanup.

### FU-7.1.a — `mlx-lm-lora` dep pin is documented only in Python docstring
- **Source:** US-7.1 critic (MED); panel also flagged.
- **Reproduction:** a fresh checkout + `pip install -r requirements.txt`
  does NOT pull `mlx-lm-lora>=2.1.0,<3`. The pin lives only in the
  `scripts/finetune-gemma.py` module docstring.
- **Fix sketch:** add `mlx-lm-lora>=2.1.0,<3` to whichever Python deps
  file the project uses (search for the existing `mlx-lm` pin location;
  if there is none, create `requirements.txt` and pin both).
- **Risk if deferred:** new contributors hit `ModuleNotFoundError` running
  `finetune-gemma.py --dpo` with no automated remediation. D1 explicitly
  required this; was missed.
- **Owner:** Sprint 7 close hygiene; can be a tiny PR after US-7.5 lands.

### FU-7.1.b — `chosen == rejected` rows not filtered before DPO
- **Source:** US-7.1 panel (Edge Cases FLAG).
- **Reproduction:** the JSONL exporter from US-7.2 may produce rows where
  `chosen == rejected` (e.g., when PII redaction collapses both fields
  to the same redacted form). These rows pass through DPO unfiltered
  and produce zero-gradient pairs.
- **Fix sketch:** in `_prepare_dpo_from_jsonl` and `prepare_dpo_from_db`,
  filter out rows where `chosen == rejected` (or warn-and-drop).
- **Risk if deferred:** DPO loss math is wasted compute on degenerate
  rows; could mask convergence issues during early training runs.
- **Owner:** US-7.4 implementer or US-7.5 implementer (both touch the
  fine-tune pipeline).

### FU-7.6.b — `hu_ml_fidelity_score_judgment` lacks `isfinite(nll)` guard
- **Source:** US-7.6 panel (Edge Cases FLAG).
- **Reproduction:** a registered NLL fn that returns `NaN` or `+Inf` for
  any row will poison the mean/min/max silently (NaN comparisons are
  always false in the min/max checks).
- **Fix sketch:** add `if (!isfinite(nll)) return HU_ERR_INVALID_ARGUMENT;`
  (or skip the row and increment a skip counter) inside the loop in
  `src/ml/fidelity.c`'s `hu_ml_fidelity_score_judgment`.
- **Risk if deferred:** when US-7.6.1 wires the real reference-GPT NLL
  backend, training instability could produce a NaN that silently
  corrupts the judgment metric.
- **Owner:** US-7.6.1 implementer (real backend wire-up).

### FU-7.6.c — `g_nll_fn` mutable static lacks thread-safety guard
- **Source:** US-7.6 critic (HIGH); panel (Style FLAG-minor).
- **Reproduction:** the global function pointer is read mid-loop in
  `hu_ml_fidelity_score_judgment` while a setter could be racing on
  another thread (e.g., US-7.5's nightly cron registering the backend
  while a chat-path verification call is in progress).
- **Fix sketch:** either (a) at scorer entry, copy `g_nll_fn` and
  `g_nll_ctx` into stack locals before the loop, or (b) document that
  setup must complete before any concurrent scoring call. Header comment
  currently says "main thread only during setup" but doesn't enforce.
- **Risk if deferred:** undefined behavior on data race; very low
  probability today (no concurrent caller yet).
- **Owner:** US-7.6.1 implementer.

## P2 — informational

### FU-7.3.a — vacuous absence-tests in `test_provider_all.c`
- **Source:** US-7.3 critic (MED).
- The two new tests asserting "no warning fires" never actually exercise
  the code path that would produce the warning. They pass vacuously.
- **Fix sketch:** add a positive-call variant for AC-7.3.4 specifically:
  make a real `hu_provider_load_adapter` call against the llamacpp
  provider, assert HU_OK return, assert no warning line.
- **Owner:** maintenance pass; not blocking.

### FU-7.3.b — whitespace reformatting inside protected test
- **Source:** US-7.3 critic (MED). AC-7.3.5 says diff to
  `test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat`
  must be empty; whitespace-only changes are present.
- The function body is semantically unchanged and the test passes.
  Optional revert if zero-tolerance interpretation is preferred.
- **Owner:** US-7.3 implementer or anyone with capacity.

### FU-7.6.d — clang-format churn in `src/ml/cli.c`
- **Source:** US-7.6 panel (Regression FLAG-medium).
- ~500 lines of cli.c are reformat-only changes mixed into the US-7.6
  commit. Violates "one concern per change" rule.
- **Risk if deferred:** merge-conflict surface against any concurrent
  cli.c work. US-7.2 has the same issue; cherry-pick order matters.
- **Owner:** post-sprint hygiene; document the reformat as a separate
  commit if a future split is wanted.

### FU-7.10.a — Init #06 vtable divergence documentation
- **Source:** US-7.10 design (Open Q2).
- US-7.10's 3-member vtable diverged from Init #06's 5-member proposal.
  After US-7.10 lands, update `docs/plans/2026-05-11-init-06-simpo-orpo-grpo2.md`
  to document the v1 surface and the planned widening before ORPO/GRPO-2.
- **Owner:** docs follow-up after US-7.10 cherry-pick.

---

**Status:** This file is the canonical follow-up tracker. Sprint-auditor
should read it to verify no P0 task is silently deferred and that each
follow-up has an owner and a fix sketch (not just a "TODO").
