# Sprint 7 Backlog — Digital Twin via Gemma DPO + Continuous Personalization

## Goal

Land a closed preference-learning loop: mine DPO pairs from outbound corrections, run them through a real DPO-aware fine-tune path, promote the adapter only when offline gates pass, and wire nightly re-training so the loop sustains itself without manual intervention.

---

## User Stories (in priority order)

---

### US-7.1 (P0): Activate the DPO preference pass in `finetune-gemma.py`

**As a** developer operating the local fine-tune pipeline,
**I want** `scripts/finetune-gemma.py --dpo` to execute a genuine DPO preference pass (not a second SFT pass over chosen-only responses),
**so that** the adapter learns to prefer Seth's voice by contrasting chosen vs. rejected completions, producing a measurably higher fidelity delta than SFT-only.

**Rationale:**
The existing `run_dpo()` function discards the `rejected` field and re-runs SFT on `chosen` responses only. This is SFT under a DPO flag — a silent correctness bug. Every downstream story (US-7.2 miner, US-7.5 nightly cron, US-7.8 ORPO pilot) depends on having a real preference loss. This is the single highest-risk correctness fix in the sprint.

**Acceptance Criteria:**
- AC-7.1.1: GIVEN a `dpo/pairs.jsonl` or `dpo_pairs.db` file with at least one valid `{prompt, chosen, rejected}` triple, WHEN `finetune-gemma.py --dpo` runs, THEN the preference pass invokes `mlx_lm lora --fine-tune-type dpo` (not `lora`) AND passes both `chosen` and `rejected` fields to the formatter; verified by asserting the shell command string contains `--fine-tune-type dpo` (captured via `subprocess.run` mock in `tests/test_finetune_gemma_dpo.py`).
- AC-7.1.2: GIVEN the DPO pass completes successfully, WHEN `scripts/check-lora-ab.sh` is run against the DPO-trained adapter vs. the SFT-only baseline, THEN the A/B delta is ≥ 0.05 above the SFT-only baseline on the `tests/fixtures/lora_baseline_persona.json` fixture (the gate script exits 0).
- AC-7.1.3: GIVEN no DPO data files exist in any candidate location, WHEN `--dpo` is passed, THEN the script prints a clearly labeled warning and exits 0 (non-fatal), leaving the SFT adapter intact; verified in `tests/test_finetune_gemma_dpo.py::test_dpo_missing_data_nonfatal`.
- AC-7.1.4: GIVEN `--sft-only` is passed alongside `--dpo`, WHEN the pipeline runs, THEN the DPO pass is skipped entirely and `--fine-tune-type dpo` is never invoked; verified by asserting no `dpo` argument appears in the captured command.
- AC-7.1.5: `scripts/check-lora-baseline.sh` exits 0 (the SFT phase still produces a valid adapter even when DPO data is absent).

**Estimate:** M
**Dependencies:** none
**Risk tier:** MEDIUM (`scripts/` only; no vtable interfaces touched)
**Test seam:** `tests/test_finetune_gemma_dpo.py` — mock `subprocess.run`, assert argv shape; `scripts/check-lora-ab.sh` and `scripts/check-lora-baseline.sh` run in CI against fixture persona.
**Out of scope:** Native reference-GPT DPO in `src/ml/dpo.c`; ORPO/SimPO loss heads; any change to rank or target-module configuration.
**DoD:** tests pass, `/verify` pass, `/aspect-panel` CLEAN, `check-lora-ab.sh` exits 0.

---

### US-7.2 (P0): Mine DPO pairs from outbound-dedup corrections

**As a** user whose messages are drafted by the agent,
**I want** every outbound message I edit or reject before sending to be automatically recorded as a labeled preference pair (rejected draft = lose, final sent = win),
**so that** the DPO pipeline has a continuously growing, ground-truth dataset of my actual preferences without any manual labeling effort.

**Rationale:**
Commit `13b89763` added outbound-message dedup tracking. That data is the cheapest possible source of real preference signal — it is Seth's own editorial decisions, not synthetic or judge-labeled. Without this miner, the DPO pass (US-7.1) has no data to consume and the continuous-learning loop (US-7.5) stalls on day one. This story is P0 because it is the data source for every downstream ML story.

**Acceptance Criteria:**
- AC-7.2.1: GIVEN the outbound-dedup table contains a row where `draft_text != sent_text` (an edit event), WHEN the miner runs (`human ml mine-corrections` or equivalent), THEN a `hu_preference_pair_t` is recorded via `hu_dpo_record_pair` with `rejected = draft_text`, `chosen = sent_text`, `prompt = incoming_context`, `source = "outbound_edit"`, and `margin` set to `0.5` (default); verified by a deterministic SQLite fixture test in `tests/test_dpo_miner.c`.
- AC-7.2.2: GIVEN a message was sent without any edit (draft equals sent), WHEN the miner runs, THEN no `hu_preference_pair_t` is recorded for that message; verified in `tests/test_dpo_miner.c::miner_skips_unedited_messages`.
- AC-7.2.3: GIVEN the miner has completed a run, WHEN `scripts/finetune-gemma.py --dpo --from-corrections` is invoked, THEN the script locates the output `dpo_pairs.db` in its standard candidate search path and proceeds to Phase 2 without error; verified by `tests/test_finetune_gemma_dpo.py::test_from_corrections_flag_resolves_db`.
- AC-7.2.4: GIVEN a draft message containing a contact name or email address, WHEN the miner records the pair, THEN the `prompt`, `chosen`, and `rejected` fields pass through `hu_personal_model_redact_pii` before insertion; verified in `tests/test_dpo_miner.c::miner_redacts_pii`.
- AC-7.2.5: GIVEN duplicate correction events (same `draft_text` + `sent_text` pair recorded twice), WHEN both are processed, THEN only one row is inserted (deduplicated by `(prompt_hash, rejected_hash, chosen_hash)`); verified in `tests/test_dpo_miner.c::miner_deduplicates_pairs`.
- AC-7.2.6: The miner compiles with `-Wall -Wextra -Wpedantic -Werror` and zero ASan errors under `cmake --preset dev`.

**Estimate:** M
**Dependencies:** none (outbound-dedup tables from `13b89763` are present)
**Risk tier:** MEDIUM (`src/ml/` new file; touches `hu_dpo_collector_t` API; SQLite writes)
**Test seam:** `tests/test_dpo_miner.c` with a deterministic in-memory SQLite fixture (no network, no real messages); `HU_IS_TEST` guards on any file-system side effects.
**Out of scope:** Rejection events from channels other than outbound edits; LLM-judge-scored pairs; real iMessage/Telegram hook integration.
**DoD:** tests pass, `/verify` pass, `/aspect-panel` CLEAN.

---

### US-7.3 (P0): Surface the local-inference honesty gate (INS-B)

**As an** operator or developer configuring the runtime,
**I want** a clear, logged warning when `personalization.lora_adapter_path` is set but the active provider is a cloud provider that returns `HU_ERR_NOT_SUPPORTED` from `hu_provider_load_adapter`,
**so that** I am never silently misled into believing personalization is active when the adapter is actually being ignored.

**Rationale:**
The existing test (`test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat`) proves the daemon does not crash, but the failure is silent. A user who has trained an adapter and sees no behavior change will lose trust in the product. This is a low-effort, high-trust story that unblocks honest communication about M3's current state. It is P0 because it is a correctness and operator-visibility requirement that affects every config that sets `lora_adapter_path` today.

**Acceptance Criteria:**
- AC-7.3.1: GIVEN a config where `personalization.lora_adapter_path` is set to a non-empty path, WHEN the daemon starts and the active provider returns `HU_ERR_NOT_SUPPORTED` from `hu_provider_load_adapter`, THEN a warning-level log line is emitted containing the literal string `"personalization adapter ignored"` and the provider name; verified in `tests/test_provider_all.c::test_cloud_provider_emits_adapter_ignored_warning`.
- AC-7.3.2: GIVEN the same config as AC-7.3.1, WHEN the warning fires, THEN `human doctor` output includes a `[WARN] personalization.lora_adapter_path is set but the active provider does not support adapters` line; verified by `tests/test_doctor_personalization_warning.c` with a mock config.
- AC-7.3.3: GIVEN a config where `personalization.lora_adapter_path` is NOT set, WHEN the daemon starts with a cloud provider, THEN no warning is emitted; verified in `tests/test_provider_all.c::test_no_adapter_path_no_warning`.
- AC-7.3.4: GIVEN a config where the active provider IS `llamacpp` (local), WHEN `hu_provider_load_adapter` returns `HU_OK`, THEN no warning is emitted; verified in `tests/test_provider_all.c::test_llamacpp_provider_no_spurious_warning`.
- AC-7.3.5: No change to existing test `test_m3_daemon_pattern_cloud_provider_falls_through_to_base_chat` — it must continue to pass unchanged.

**Estimate:** S
**Dependencies:** none
**Risk tier:** LOW (read-only config path check + log emission; no vtable changes)
**Test seam:** `tests/test_provider_all.c` (extend existing file); `tests/test_doctor_personalization_warning.c` (new, fixture-driven); `HU_IS_TEST` guard on log output capture.
**Out of scope:** Changing provider behavior; adding MLX provider support; any Bridge B.1 work.
**DoD:** tests pass, `/verify` pass, `/aspect-panel` CLEAN.

---

### US-7.4 (P1): Raise LoRA rank and expand target modules in `finetune-gemma.py`

**As a** developer tuning the fine-tune pipeline,
**I want** the ability to train with rank 32–64 across Q, K, V, O, gate, up, and down projection layers (instead of rank 16 with Q+V only),
**so that** I can benchmark the quality/adapter-size tradeoff and determine the optimal configuration for the W14 nightly loop.

**Rationale:**
Current defaults (rank 16, Q+V only) were chosen conservatively for the initial SFT path. DPO training benefits from higher-rank adapters that can represent the contrastive signal more expressively. This story adds a `--target-modules` CLI flag and updates the per-model-target defaults, then validates the quality lift against the existing `lora-ab` gate. Depends on US-7.1 (real DPO pass) because benchmarking rank changes against a fake DPO path would produce misleading results.

**Acceptance Criteria:**
- AC-7.4.1: GIVEN `--rank 64 --target-modules q_proj,k_proj,v_proj,o_proj,gate_proj,up_proj,down_proj`, WHEN `finetune-gemma.py` runs, THEN the `mlx_lm lora` invocation includes `--lora-layers` (or equivalent) covering all seven module names; verified by `tests/test_finetune_gemma_modules.py::test_target_modules_propagated_to_mlx_cmd`.
- AC-7.4.2: GIVEN no `--target-modules` flag (default), WHEN `finetune-gemma.py` runs with target `31b`, THEN rank defaults to 32 (up from 16) and modules default to `q_proj,k_proj,v_proj,o_proj`; verified by `tests/test_finetune_gemma_modules.py::test_31b_default_rank_32`.
- AC-7.4.3: GIVEN rank 64 across all 7 modules is trained on the fixture dataset, WHEN `scripts/check-lora-ab.sh` is run comparing rank-64/7-module adapter vs rank-16/QV adapter, THEN the script produces a JSON result with `"delta"` and `"size_mb"` fields (exit code 0 regardless of which wins — this story measures, not gates); verified by inspecting the output JSON schema.
- AC-7.4.4: `scripts/check-lora-baseline.sh` exits 0 with the new default rank-32 adapter.
- AC-7.4.5: The `train_config.json` written by `version_adapter()` records `target_modules` as a list field alongside `rank`; verified in `tests/test_finetune_gemma_modules.py::test_train_config_records_target_modules`.

**Estimate:** S
**Dependencies:** US-7.1 (real DPO pass must be in place before benchmarking rank changes)
**Risk tier:** LOW (`scripts/` only; no C changes)
**Test seam:** `tests/test_finetune_gemma_modules.py` — mock `subprocess.run`; `scripts/check-lora-ab.sh` run in CI.
**Out of scope:** Automatic rank selection; hyperparameter search; changing edge-model (e4b/e2b) defaults in this story.
**DoD:** tests pass, `/verify` pass, `/aspect-panel` CLEAN, `check-lora-baseline.sh` exits 0.

---

### US-7.5 (P1): Wire W14 nightly re-train cron

**As a** user who wants the digital twin to improve automatically,
**I want** the W14 idle scheduler to invoke `finetune-gemma.py --dpo --from-corrections` nightly when the device is idle,
**so that** the adapter refreshes itself from new correction data without requiring manual runs.

**Rationale:**
US-7.1 and US-7.2 deliver the pipeline and data source. This story closes the continuous-learning loop by scheduling the pipeline. The adapter-rollback ADR (`docs/plans/adr/2026-05-11-adapter-rollback-signal.md`) governs promotion: the nightly run produces a candidate, `check-lora-ab.sh` gates promotion, and rollback fires if any of S1/S2/S3 trips. This story wires the trigger; promotion gating reuses existing scripts.

**Acceptance Criteria:**
- AC-7.5.1: GIVEN the W14 idle scheduler is running and the device has been idle for the configured threshold, WHEN the nightly window fires, THEN the scheduler emits a `lora_retrain_scheduled` event and invokes `finetune-gemma.py --dpo --from-corrections --no-restart-server --no-version` as a background subprocess; verified in `tests/test_w14_lora_retrain.c` with a mock scheduler tick and a `HU_IS_TEST` subprocess guard.
- AC-7.5.2: GIVEN the nightly finetune subprocess exits 0, WHEN `check-lora-ab.sh` runs against the new candidate, THEN if the delta ≥ promotion threshold the adapter symlink (`seth-lora-current`) is updated; if delta < threshold the candidate is discarded and the previous symlink is preserved; verified by `tests/test_w14_lora_retrain.c::test_retrain_promotes_on_pass_skips_on_fail` using fixture metadata files.
- AC-7.5.3: GIVEN the nightly finetune subprocess exits non-zero (training failure), WHEN the scheduler observes the failure, THEN the current adapter is NOT modified and a `lora_retrain_failed` event is emitted with the exit code; verified in `tests/test_w14_lora_retrain.c::test_retrain_failure_preserves_adapter`.
- AC-7.5.4: GIVEN no correction pairs have accumulated since the last retrain run (miner produces 0 new pairs), WHEN the scheduler fires, THEN the retrain is skipped and a `lora_retrain_skipped_no_new_data` event is emitted; verified in `tests/test_w14_lora_retrain.c::test_retrain_skipped_on_empty_delta`.
- AC-7.5.5: The scheduler status JSON (`~/.human/scheduler.status`) gains a `lora_retrain` block with fields `last_run_ts`, `last_outcome` (`skipped|pass|fail`), and `pairs_consumed`; `human doctor scheduler` parses and displays this block; verified by existing `hu_scheduler_status_parse_json` contract test extended in `tests/test_scheduler_status.c`.

**Estimate:** M
**Dependencies:** US-7.1, US-7.2
**Risk tier:** MEDIUM (touches W14 scheduler invocation path and adapter promotion logic)
**Test seam:** `tests/test_w14_lora_retrain.c` with `HU_IS_TEST` subprocess mock; fixture `tests/fixtures/candidate_adapter_metadata.json` for promotion gate simulation; `tests/test_scheduler_status.c` extended.
**Out of scope:** Multi-day rollback history UI; S3 user-feedback signal wiring (ADR item, future sprint); any cloud-side telemetry.
**DoD:** tests pass, `/verify` pass, `/aspect-panel` CLEAN.

---

### US-7.6 (P1): Add judgment-fidelity eval (INS-A) — held-out perplexity on real continuations

**As a** developer evaluating adapter quality,
**I want** `hu_ml_fidelity_score_baseline` (or a sibling function) to include a held-out perplexity test on real message continuations — "given this incoming message, predict Seth's actual next turn" — in addition to the existing lexical-surface metrics,
**so that** the `lora-baseline` and `lora-ab` gates catch adapters that pass surface-style checks but produce decisions that are un-Seth-like.

**Rationale:**
The existing fidelity scoring in `src/ml/fidelity.c` measures lexical surface properties (lowercase ratio, emoji frequency, message length, etc.) against `hu_communication_style_t`. These metrics can be gamed by a degenerate adapter that produces short, lowercase, low-emoji output. The held-out perplexity test measures semantic judgment fidelity — whether the adapter's probability distribution over real continuations is tighter than the base model's. This directly addresses INS-A. It must land before US-7.5 (nightly cron) goes live so the promotion gate is honest.

**Acceptance Criteria:**
- AC-7.6.1: GIVEN a held-out fixture file `tests/fixtures/judgment_fidelity_holdout.jsonl` with ≥ 10 `{prompt, continuation}` pairs representing real message exchanges, WHEN `human ml fidelity-status --judgment` is run, THEN the output includes a `judgment_ppl` field (float) computed as mean negative log-likelihood over the continuations under the current adapter (or base model if no adapter is active); verified in `tests/test_ml_fidelity_judgment.c::test_judgment_ppl_computed_on_holdout`.
- AC-7.6.2: GIVEN a synthetic "bad" adapter fixture that produces uniformly low NLL on random text, WHEN `check-lora-ab.sh` includes the judgment-PPL check, THEN the adapter fails the gate (judgment-PPL delta < 0 against baseline) even if lexical surface metrics pass; verified by `tests/test_ml_fidelity_judgment.c::test_judgment_ppl_catches_degenerate_adapter` with a deterministic mock NLL function.
- AC-7.6.3: GIVEN `HU_IS_TEST` is defined, WHEN the judgment-PPL path is exercised, THEN no real model weights are loaded — the NLL values are injected via the mock seam `hu_ml_nll_compute_fn_t` registered in test setup; verified by asserting the real file-load path is never reached.
- AC-7.6.4: The `hu_ml_fidelity_score_baseline` function signature does not change (backward compatible); the judgment path is exposed through a new `hu_ml_fidelity_score_judgment` function with its own header entry in `include/human/ml/fidelity.h`; verified by confirming existing callers compile without modification.
- AC-7.6.5: `scripts/check-lora-ab.sh` gains an optional `--judgment` flag; when passed, the script runs `human ml fidelity-status --judgment` and includes the `judgment_ppl_delta` in its JSON output; verified by `tests/test_check_lora_ab_judgment.sh`.

**Estimate:** M
**Dependencies:** none (can be developed in parallel with US-7.1/7.2; must be merged before US-7.5 so the nightly cron uses the honest gate)
**Risk tier:** MEDIUM (new function in `src/ml/fidelity.c`; extends gate scripts)
**Test seam:** `tests/test_ml_fidelity_judgment.c` with `HU_IS_TEST` mock NLL seam; `tests/fixtures/judgment_fidelity_holdout.jsonl` (≥ 10 rows, no PII, deterministic); `tests/test_check_lora_ab_judgment.sh`.
**Out of scope:** Real perplexity computation via loaded model weights in CI; integration with the reference GPT (`src/ml/gpt.c`); per-channel PPL breakdown.
**DoD:** tests pass, `/verify` pass, `/aspect-panel` CLEAN.

---

### US-7.7 (P1): Test-time persona scoring — best-of-N at inference

**As a** user whose messages are generated by the llamacpp provider,
**I want** the provider to optionally sample N completions and return the one with the highest `hu_communication_style_fidelity_score`,
**so that** inference-time selection improves persona fidelity without requiring further training, at a configurable cost.

**Rationale:**
Best-of-N is a well-understood technique that can lift fidelity meaningfully at 4x generation cost. It is purely inference-time and does not require the bridge stack to be complete — it works against the current llamacpp stub and will work against the full bridge when it lands. Gated off by default to protect existing behavior. Depends on US-7.3 (honesty gate) being merged first so the config surface is consistent.

**Acceptance Criteria:**
- AC-7.7.1: GIVEN `inference.best_of_n = 4` in config and the llamacpp provider is active, WHEN a chat request is made, THEN the provider calls the completion path exactly 4 times and returns the completion with the highest `hu_communication_style_fidelity_score`; verified in `tests/test_llamacpp_best_of_n.c::test_best_of_4_returns_highest_score` using a mock completion function that returns fixed strings with known fidelity scores.
- AC-7.7.2: GIVEN `inference.best_of_n` is absent or set to 1, WHEN a chat request is made, THEN the completion path is called exactly once (no behavioral change from current); verified in `tests/test_llamacpp_best_of_n.c::test_best_of_1_is_single_call`.
- AC-7.7.3: GIVEN `inference.best_of_n = 4` but the active provider is a cloud provider, WHEN the config is loaded, THEN `human doctor` emits a `[WARN] inference.best_of_n has no effect with cloud providers` warning; verified in `tests/test_doctor_best_of_n_warning.c`.
- AC-7.7.4: GIVEN the best-of-N path runs, WHEN telemetry is enabled, THEN a `best_of_n_pick` event is recorded with fields `n`, `picked_score`, `min_score`, `max_score`; verified in `tests/test_llamacpp_best_of_n.c::test_best_of_n_telemetry_emitted`.
- AC-7.7.5: GIVEN `inference.best_of_n_cost_cap_ms` is set to a finite value and N completions would exceed it, WHEN the cap is reached, THEN the best completion seen so far is returned and a `best_of_n_cost_cap_hit` event is emitted; verified in `tests/test_llamacpp_best_of_n.c::test_cost_cap_returns_best_seen`.
- AC-7.7.6: `hu_communication_style_fidelity_score` is called through the existing public API with no signature change; verified by confirming `include/human/memory/personal_model.h` is unmodified.

**Estimate:** M
**Dependencies:** US-7.3 (honesty gate ensures config surface is coherent before adding another config key)
**Risk tier:** MEDIUM (`src/providers/llamacpp.c`; new config keys; touches inference hot path when enabled)
**Test seam:** `tests/test_llamacpp_best_of_n.c` with mock completion function and `HU_IS_TEST` guard; `tests/test_doctor_best_of_n_warning.c` with fixture config.
**Out of scope:** MLX provider best-of-N (deferred to Bridge B.1); beam-search or tree-search variants; per-channel N configuration.
**DoD:** tests pass, `/verify` pass, `/aspect-panel` CLEAN.

---

### US-7.8 (P2): MoLoRA static per-channel router — Init #02 (phase 1)

**As a** user who communicates differently across Telegram, iMessage, Slack, and Discord,
**I want** the agent to select a channel-specific LoRA adapter at inference time rather than applying a single adapter to all channels,
**so that** my Telegram-casual voice does not bleed into my Slack-professional responses.

**Rationale:**
Init #02 (`docs/plans/2026-05-11-init-02-molora-channels.md`) proposes a MoLoRA architecture. This story delivers Phase 1 only: a static router (channel_id → adapter slot, no learned MLP, no differentiable routing). The static router is a prerequisite for training the learned router and for validating that per-channel adapters actually improve fidelity before investing in the dynamic routing machinery. Gated behind `HU_ENABLE_MOLORA`. P2 because it depends on US-7.1/7.2/7.5 being stable first.

**Acceptance Criteria:**
- AC-7.8.1: GIVEN `molora.enabled = true` and a `molora.channel_adapters` config map of `{channel_id: adapter_path}`, WHEN an agent turn fires for channel `"telegram"`, THEN the llamacpp provider selects the adapter registered for `"telegram"` (or falls back to the default `lora_adapter_path` if no channel entry exists); verified in `tests/test_molora_router.c::test_static_router_selects_channel_adapter`.
- AC-7.8.2: GIVEN a channel not present in `channel_adapters`, WHEN an agent turn fires, THEN the default adapter (or no adapter if none configured) is used and no error is emitted; verified in `tests/test_molora_router.c::test_static_router_fallback_to_default`.
- AC-7.8.3: GIVEN `molora.enabled = false` (default), WHEN the agent starts, THEN `hu_molora_router_select` is never called and binary behavior is identical to pre-story behavior; verified by a compile-time `#ifndef HU_ENABLE_MOLORA` guard check in `tests/test_molora_router.c::test_disabled_molora_no_call`.
- AC-7.8.4: The `hu_molora_router_t` struct and `hu_molora_router_select` function are declared in a new `include/human/ml/molora.h`; the struct is zero-initializable (no constructor required for static router); verified by a compile-only test.
- AC-7.8.5: Binary size delta with `HU_ENABLE_MOLORA=ON` vs `OFF` is ≤ 8 KB (per Init #02 binary budget); verified by `cmake --preset release` + `scripts/check-binary-size.sh` diff (or equivalent size gate script).

**Estimate:** L
**Dependencies:** US-7.1, US-7.2, US-7.5 (need stable adapter pipeline before per-channel adapters are meaningful)
**Risk tier:** MEDIUM (new config key; new include; conditional compile; touches inference dispatch path)
**Test seam:** `tests/test_molora_router.c` with mock adapter-load and `HU_IS_TEST` guard; binary size delta checked in CI via `cmake --preset release`.
**Out of scope:** Learned MLP router; differentiable routing (LD-MoLE/DynMoLE); training the per-channel adapters themselves (training uses US-7.1 pipeline invoked per-channel bank); MLX provider integration.
**DoD:** tests pass, `/verify` pass, `/aspect-panel` CLEAN, binary size delta ≤ 8 KB.

---

### US-7.9 (P2): Constitutional style self-critique at generation time

**As a** user with explicit style rules (e.g., "never start texts with 'Sure!'" or "no em-dashes when texting"),
**I want** the agent to check its draft response against my style rules before sending and regenerate if any rule is violated,
**so that** persona rules I've explicitly stated are enforced mechanically, not just hinted in the system prompt.

**Rationale:**
`hu_persona_t.style_rules` and `constitutional_ai` flags already exist in the codebase. This story wires them: at post-generation time, before the response is returned to the channel, a lightweight rule-checker scans the draft against `style_rules` and triggers a single regeneration if any rule fires. This is a deterministic, local check — no LLM judge needed. Depends on US-7.1/7.3 being stable (clean config surface).

**Acceptance Criteria:**
- AC-7.9.1: GIVEN `constitutional.style_rules_enabled = true` and `persona.style_rules = ["never start with 'Sure!'", "no em-dashes"]`, WHEN a draft response begins with "Sure!" or contains "—", THEN the agent invokes one regeneration attempt and the final response does not contain the violating pattern; verified in `tests/test_style_self_critique.c::test_sure_prefix_triggers_regen`.
- AC-7.9.2: GIVEN a draft that passes all style rules on first attempt, WHEN the self-critique runs, THEN exactly zero regeneration attempts are made; verified in `tests/test_style_self_critique.c::test_clean_draft_no_regen`.
- AC-7.9.3: GIVEN a draft that still violates a rule after one regeneration (worst-case), WHEN the second attempt also fails, THEN the agent returns the second attempt (best effort) and emits a `style_rule_violation_unresolved` event with the rule name; it does NOT loop indefinitely; verified by `tests/test_style_self_critique.c::test_max_one_regen_on_persistent_violation`.
- AC-7.9.4: GIVEN `constitutional.style_rules_enabled = false` (default), WHEN the agent generates a response, THEN the self-critique path is never entered; verified by asserting `hu_style_critique_check` is never called in the test mock.
- AC-7.9.5: The style-rule checker is implemented in `src/persona/style_critique.c` as a pure string-pattern matcher (no LLM call); each rule is matched as a case-insensitive prefix or substring check; verified by unit tests in `tests/test_style_critique_patterns.c` covering ≥ 5 rule patterns.

**Estimate:** M
**Dependencies:** US-7.3 (clean config surface)
**Risk tier:** MEDIUM (touches agent response path; new `src/persona/` file; new config keys)
**Test seam:** `tests/test_style_self_critique.c` with mock provider returning fixed strings; `tests/test_style_critique_patterns.c` for pure pattern matching; `HU_IS_TEST` guard.
**Out of scope:** LLM-judge critique; multi-turn self-correction loops; learning new rules from behavior; rule authoring UI.
**DoD:** tests pass, `/verify` pass, `/aspect-panel` CLEAN.

---

### US-7.10 (P2): ORPO/SimPO pilot — Init #06 phase 1 (vtable + one loss head)

**As a** developer researching preference optimization algorithms,
**I want** a `hu_rl_trainer_t` vtable with a SimPO loss head registered behind it,
**so that** I can run a single-stage SFT+preference pass and compare it against the two-stage SFT+DPO baseline from US-7.1, without coupling the new algorithm to the existing DPO code paths.

**Rationale:**
Init #06 (`docs/plans/2026-05-11-init-06-simpo-orpo-grpo2.md`) defines a clean vtable (`hu_rl_trainer_t`) behind which SimPO, ORPO, and GRPO-2 all live. This story delivers the vtable definition and exactly one factory (SimPO) with a deterministic golden test. ORPO and GRPO-2 are explicitly deferred. The vtable must be in place before any of the three algorithms ships so that the binary interface is stable. P2 because it is research frontier work that must not block P0/P1.

**Acceptance Criteria:**
- AC-7.10.1: `include/human/ml/rl_trainer.h` declares `hu_rl_trainer_t` with vtable fields `train_step`, `compute_loss`, `deinit`, and a `hu_rl_trainer_type_t` enum with values `HU_RL_TRAINER_DPO`, `HU_RL_TRAINER_SIMPO`, `HU_RL_TRAINER_ORPO`, `HU_RL_TRAINER_GRPO2`; verified by compile-only test and header parse.
- AC-7.10.2: GIVEN a `hu_rl_trainer_t` created via `hu_rl_trainer_simpo_create`, WHEN `compute_loss` is called with a golden fixture `{prompt, chosen, rejected, beta=0.1, gamma=0.5}`, THEN the returned loss matches the analytically computed SimPO loss to within 1e-4 absolute tolerance; verified in `tests/test_rl_trainer_simpo.c::test_simpo_loss_golden`.
- AC-7.10.3: GIVEN the `human ml rl-train --algorithm simpo` CLI subcommand, WHEN invoked with the fixture dataset, THEN it selects the SimPO factory and runs `train_step` without crashing; exit code 0; verified in `tests/test_ml_cli_rl_train.c::test_rl_train_simpo_e2e_fixture` with `HU_IS_TEST` guards on file writes.
- AC-7.10.4: GIVEN `--algorithm dpo`, WHEN `human ml rl-train` is invoked, THEN the existing `hu_dpo_collector_t` path is selected (no behavior change); verified in `tests/test_ml_cli_rl_train.c::test_rl_train_dpo_backward_compat`.
- AC-7.10.5: GIVEN `--algorithm orpo` or `--algorithm grpo2`, WHEN `human ml rl-train` is invoked, THEN it exits with a clear `"not yet implemented"` message and exit code 2; verified in `tests/test_ml_cli_rl_train.c::test_rl_train_unimplemented_algorithms`.
- AC-7.10.6: All new code compiles with `-Wall -Wextra -Wpedantic -Werror` and zero ASan errors.

**Estimate:** L
**Dependencies:** US-7.1 (DPO baseline must exist to serve as the comparison reference)
**Risk tier:** MEDIUM (new vtable in `include/human/ml/`; new `src/ml/rl_trainer.c`; new CLI subcommand)
**Test seam:** `tests/test_rl_trainer_simpo.c` with analytical golden fixture; `tests/test_ml_cli_rl_train.c` with `HU_IS_TEST` guard; no real model weights in CI.
**Out of scope:** ORPO and GRPO-2 loss implementations; integration with `finetune-gemma.py`; A/B comparison against DPO baseline (that is a follow-on story after both adapters are trained).
**DoD:** tests pass, `/verify` pass, `/aspect-panel` CLEAN.

---

## Non-goals

- We will NOT implement Bridge B.1 (full MLX provider chat inference) — US-7.7 best-of-N and US-7.3 honesty gate are explicitly designed to work without it.
- We will NOT ship ORPO, GRPO-2, or LD-MoLE/DynMoLE routing in this sprint — US-7.10 gates behind unimplemented stubs and US-7.8 ships static routing only.
- We will NOT touch `src/security/`, `src/gateway/gateway.c`, or vtable interfaces outside of the explicitly scoped new additions in US-7.8 and US-7.10.
- We will NOT collect S3 user-feedback signal (thumbs-down rate, retry rate) for the rollback ADR — the ADR describes this as Phase C5.1; W14 wiring (US-7.5) uses S1/S2 signals only.
- We will NOT change `hu_communication_style_fidelity_score` signature or the existing `lora-baseline` fixture — all new fidelity work in US-7.6 is additive only.

---

## Dependency Graph — Parallel Waves

```
Wave 0 (parallel, no dependencies):
  US-7.1  — DPO preference pass (finetune-gemma.py)
  US-7.2  — Mine DPO pairs from outbound corrections
  US-7.3  — Local-inference honesty gate
  US-7.6  — Judgment-fidelity eval (INS-A)

Wave 1 (after Wave 0 merges):
  US-7.4  — Rank + target-modules expansion   [requires US-7.1]
  US-7.5  — W14 nightly re-train cron          [requires US-7.1, US-7.2]
  US-7.7  — Best-of-N at inference             [requires US-7.3]
  US-7.9  — Constitutional style self-critique [requires US-7.3]

Wave 2 (after Wave 1 is stable):
  US-7.8  — MoLoRA static router              [requires US-7.1, US-7.2, US-7.5]
  US-7.10 — ORPO/SimPO vtable pilot           [requires US-7.1]
```

Stories in the same wave share no state and may be assigned to separate worktrees in parallel. Stories in Wave 2 should not start until the Wave 1 `check-lora-ab.sh` and `check-lora-baseline.sh` gates are both green on main.

---

## Open Questions for Stakeholder

None. All initiatives have enough design material to author defensible AC. P2 stories carry explicit "not yet implemented" stubs for deferred features so the sprint auditor has a clear boundary.

---

RESULT_product-owner=READY
