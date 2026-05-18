# Sprint 8 — Close the Digital Twin Loop (Honest Edition)

**Status:** Draft (pre-planning)
**Predecessor:** Sprint 7 (closed at `v-sprint-7-close`, auditor PASS_WITH_NOTES)
**Theme:** Make Sprint 7's components actually compose into the headline outcome.

## Why this sprint exists

Sprint 7's adversarial auditor caught the deepest truth: we shipped 10 components
but the gate that was supposed to prove the headline metric (`lora-ab delta > 0.05
above SFT-only baseline`) is structurally vacuous — it measures pre-baked fixtures,
not real DPO-trained adapters.

Sprint 8 fixes that. Plus the four auditor-surfaced gaps + the UTF-8 redactor bug
the post-close smoke run found.

## In scope

| Story | Title | Source | P |
|---|---|---|---|
| US-8.1 | Wire `check-lora-ab.sh` to real DPO vs SFT comparison | auditor §4 / AC-7.1.2 vacuity | P0 |
| US-8.2 | Wire reference HUML GPT as judgment-fidelity NLL backend (US-7.6.1) | followups.md FU-7.6.b/c | P0 |
| US-8.3 | Promote FU-7.10.a to P0: hide `simpo` or wire forward-pass | auditor §2 | P0 |
| US-8.4 | `lora_retrain_failed` event gains `step` discriminator | followups.md FU-7.5.b | P1 |
| US-8.5 | Fix UTF-8 boundary preservation in PII redactor | smoke-run finding | P1 |
| US-8.6 | Fix `--lora-parameters` CLI mismatch in `scripts/finetune-gemma.py` | smoke-run finding | **P0** |

## Out of scope

- US-7.10 ORPO/GRPO2 implementations (still deferred)
- US-7.6.2+ real Gemma-based NLL backend (US-8.2 uses reference HUML GPT)
- MoLoRA dynamic routing (Init #02 phase 2; static-router-only shipped)
- M4/M6 product pivot (Sprint 9+)

## Pre-flight (apply BEFORE Wave 0 dispatch)

Per Sprint 7 retro CHANGE-1 through CHANGE-6:

- [x] **CHANGE-1**: `.claude/settings.local.json` has `"worktree": {"baseRef": "head"}` (verified)
- [ ] **CHANGE-2**: `/tune-agent` patches applied to product-owner, tech-lead, critic, verifier (running in current session)
- [ ] **CHANGE-3**: pre-commit guard for reformat-mixed-with-feature (post-Sprint-8 hygiene)
- [ ] **CHANGE-4**: PO must `git show` referenced commits before locking AC text
- [ ] **CHANGE-5**: scrum-master Phase 5 must verify goal-metric is e2e-tested (this sprint's actual test)
- [ ] **CHANGE-6**: terse output style for critic + panel agents

## Sprint 8 success metric

**The headline gate runs against real adapters and produces a real number.** Specifically:

```bash
# This command, executed on a fresh sprint-8 worktree, must:
scripts/lora-runner-ab.sh --persona seth \
  --base ~/.human/training-data/adapters/seth-sft \
  --adapter ~/.human/training-data/adapters/seth-dpo

# Produce stdout:
#  - A real delta value computed from running both adapters against the persona
#  - JSON output that distinguishes "passed gate" from "no data" from "fail"
#  - check-lora-ab.sh --judgment that EXIT 1s when STATUS is empty (not 0)
```

If this command exits 0 with a real (non-fixture) delta above 0.05, Sprint 8 closes.

## Stories

### US-8.1 (P0): Wire `check-lora-ab.sh` to real DPO vs SFT comparison

**AC:**
- AC-8.1.1: `scripts/check-lora-ab.sh --base <sft-adapter> --adapter <dpo-adapter>` accepts both flags and routes them through `human ml fidelity-status` (or `lora-runner-ab.sh`) to produce a real comparison.
- AC-8.1.2: When `--base` and `--adapter` are real adapter paths, the script computes a real delta against the persona fingerprint (no pre-baked fixture fallback).
- AC-8.1.3: When `--base` or `--adapter` is missing, the script exits 1 (NOT 0 with fixture fallback).
- AC-8.1.4: The pre-baked fixture path (`tests/fixtures/lora_ab_before.json` + `_after.json`) remains available for CI smoke but is gated behind `--fixture` flag.
- AC-8.1.5: A new CI smoke test exercises the real-adapter path end-to-end against a tiny fixture adapter (zero forward passes; mock NLL injection per D3 pattern).

**Test seam:** new shell test `tests/test_check_lora_ab_real_adapter.sh` with HU_IS_TEST stubs for the adapter-load path.

**Risk:** MEDIUM — `lora-runner-ab.sh` already exists from US-7.4 work; we extend it.

### US-8.2 (P0): Wire reference HUML GPT as judgment-fidelity NLL backend (US-7.6.1)

**AC:**
- AC-8.2.1: `hu_ml_fidelity_set_nll_compute_fn` is called automatically at daemon startup with a fn pointer that delegates to `src/ml/gpt.c::hu_gpt_compute_loss`.
- AC-8.2.2: `hu_ml_fidelity_score_judgment` returns a real PPL value (not NOT_SUPPORTED) when called with a holdout file and an active adapter.
- AC-8.2.3: `check-lora-ab.sh --judgment` STATUS field is `"ok"` (not `"not_supported_no_local_inference"`) by default.
- AC-8.2.4: When the reference GPT model is absent on disk, the registration falls back to `NOT_SUPPORTED` (graceful degradation, not crash).
- AC-8.2.5: Existing `test_production_default_returns_not_supported` is updated or replaced; the new test asserts the wired path returns real NLL.

**Test seam:** existing mock-NLL pattern from US-7.6 + new integration test that loads a tiny reference GPT checkpoint from `tests/fixtures/`.

**Risk:** MEDIUM — touches the dormancy contract from D3.

### US-8.3 (P0): Promote FU-7.10.a — hide `simpo` from `--help` OR wire forward-pass

**AC (choose one path):**

Path A — hide:
- AC-8.3.A.1: `human ml rl-train --help` does NOT list `simpo` in the `--algorithm` choices.
- AC-8.3.A.2: `human ml rl-train --algorithm simpo` exits 2 with message "experimental; not yet wired".
- AC-8.3.A.3: `test_rl_train_simpo_e2e_fixture` still passes (test runs under HU_IS_TEST).

Path B — wire (more work):
- AC-8.3.B.1: `human ml rl-train --algorithm simpo` outside HU_IS_TEST returns HU_OK and writes a real adapter.
- AC-8.3.B.2: The forward pass uses `hu_gpt_forward` from src/ml/gpt.c.
- AC-8.3.B.3: New test asserts non-test path returns HU_OK (not NOT_SUPPORTED).

**Recommend Path A this sprint; Path B in Sprint 9.**

### US-8.4 (P1): `lora_retrain_failed` event step discriminator

**AC:**
- AC-8.4.1: `lora_retrain_failed` event payload includes `step` field with values from `{probe, finetune, gate, promote}`.
- AC-8.4.2: Existing 4 failure tests in `tests/test_w14_lora_retrain.c` are updated to assert the `step` value.
- AC-8.4.3: `scheduler.status` JSON's `lora_retrain.last_outcome` adds the `failed_step` field when `last_outcome == "failed"`.

**Risk:** LOW.

### US-8.6 (P0): `--lora-parameters` CLI mismatch in `scripts/finetune-gemma.py`

**The bug:** US-7.4 added `--lora-parameters '{"rank":32,"keys":[...]}'` to the
mlx_lm subprocess argv. Neither stock `mlx_lm.lora` nor `mlx_lm_lora.train` accepts
this CLI flag — both reject it with `unrecognized arguments`. The argv-shape test
US-7.4 added passes (it only checks the string was emitted), but the actual
subprocess call FAILS at runtime with exit 2. This means the SFT phase of
`finetune-gemma.py` is **completely broken when invoked end-to-end**.

The `lora_parameters` JSON must go through `-c <config.yaml>` (config-file
mechanism), NOT as a CLI flag. Both tools read it from the YAML.

**AC:**
- AC-8.6.1: `scripts/finetune-gemma.py --target e2b --no-dpo --iters 50` actually
  invokes `mlx_lm_lora.train` (or `mlx_lm.lora`) successfully — exit 0 — and writes a
  non-empty adapter file.
- AC-8.6.2: When `--target-modules` or `--rank` is set, the script writes a
  temporary YAML config (in `$adapter_path/config.yaml`) with the `lora_parameters`
  block and passes `-c <that path>` to the subprocess.
- AC-8.6.3: New test `test_finetune_gemma_modules_yaml_not_cli_flag` asserts:
  (a) `--lora-parameters` does NOT appear in the argv, (b) `-c <yaml>` does appear,
  (c) the YAML at that path contains the expected `lora_parameters` block.
- AC-8.6.4: All existing US-7.4 tests continue to pass (they may need to update
  their assertion from "argv contains --lora-parameters" to "YAML at -c path contains keys").
- AC-8.6.5: End-to-end smoke: `scripts/finetune-gemma.py --target e2b --no-dpo`
  on a tiny fixture dataset succeeds and produces an adapter <10MB.

**Test seam:** `tests/test_finetune_gemma_modules.py` extended; new
`tests/test_finetune_gemma_e2e_smoke.sh` runs a 5-iter SFT against a 3-row
fixture and asserts adapter file exists.

**Risk:** MEDIUM — touches `finetune-gemma.py` Phase 1 path; AC-7.4.3 JSON gate
output must remain green.

**Cross-story:** US-8.1's real-DPO gate depends on this working (US-8.1 cannot
verify a real comparison if SFT is broken).

---

### US-8.5 (P1): UTF-8 boundary preservation in PII redactor

**AC:**
- AC-8.5.1: `hu_pii_redact` preserves UTF-8 byte-sequence boundaries — never truncates a multi-byte char.
- AC-8.5.2: New test `test_pii_redact_utf8_boundary` exercises emoji + accented characters + zero-width joiner edge cases.
- AC-8.5.3: Re-running `human ml mine-corrections` on a chat.db with emoji-bearing messages produces JSONL that is 100% valid UTF-8 (`python3 -c 'open(path).read()'` does not raise UnicodeDecodeError).
- AC-8.5.4: Existing `dpo_miner` tests all still pass.

**Risk:** LOW.

## Dependency graph

```
Wave 0 (parallel):
  US-8.1 — real DPO vs SFT gate
  US-8.4 — step discriminator (independent)
  US-8.5 — UTF-8 redactor (independent)

Wave 1 (after Wave 0):
  US-8.2 — NLL backend (uses US-8.1's --base/--adapter pattern)
  US-8.3 — simpo P0 (independent, can run in Wave 0 too)
```

## Budget

5 stories, smaller than Sprint 7. Estimated:
- Implementer × 5: ~$15-25
- Verifier × 5: ~$7
- Critic × 5: ~$6
- Aspect-panel × 3 (P0 only): ~$7
- PO + tech-leads + scrum-master + audit + retro: ~$15
- **Total: ~$50-60**

Wall-clock estimate: 2-3 hours with parallel waves, post-CHANGE-2 fewer paused-mid-task nudges.

## When to start Sprint 8

After Sprint 7's real GPU smoke completes (current session) — the data from the
real DPO vs SFT comparison directly informs whether US-8.1's gate threshold should
stay at `0.05` or be re-calibrated.
