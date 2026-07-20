#ifndef HU_ML_CLI_H
#define HU_ML_CLI_H

#include "human/core/allocator.h"
#include "human/core/error.h"

hu_error_t hu_ml_cli_train(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_experiment(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_prepare(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_status(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_dpo_train(hu_allocator_t *alloc, int argc, const char **argv);

/* `human ml pair-init-singles` — converts accumulated single-sided
 * init_proposer_v1 rows in dpo_pairs into trainable two-sided
 * preference rows under init_proposer_paired_v1. Opens ~/.human/memory.db
 * read-write, registers the collector with the bridge, calls
 * hu_init_dpo_bridge_pair_singles, prints the paired count, and
 * cleans up. Safe to invoke against a live daemon's memory.db —
 * SQLite's per-connection locking handles concurrent access. Honors
 * HU_MEMORY_DB_PATH env override for tests. */
hu_error_t hu_ml_cli_pair_init_singles(hu_allocator_t *alloc, int argc, const char **argv);

/* Sprint 7 US-7.2 — mine DPO preference pairs from chat.db correction
 * triples and (optionally) export to JSONL for the finetune-gemma.py DPO
 * pass. See `human/ml/dpo_miner.h`. */
hu_error_t hu_ml_cli_mine_corrections(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_prepare_conversations(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_lora_persona(hu_allocator_t *alloc, int argc, const char **argv);
/* Track D D2.2 — offline persona-fidelity baseline.
 *
 * Loads a persona's example bank and scores each example response
 * against the persona's communication-style fingerprint (or a
 * synthetic one when no personal_model.bin exists). Reports
 * per-example fidelity + mean / min / max / count. Pure CPU, no
 * network, no provider — the scorer is the same
 * `hu_communication_style_fidelity_score` that future LoRA-vs-
 * baseline comparisons use, so the number a user sees here is
 * directly comparable to what they will see post-LoRA. */
hu_error_t hu_ml_cli_lora_baseline(hu_allocator_t *alloc, int argc, const char **argv);
/* Track D D2.2 — offline LoRA A/B comparison.
 *
 * Loads a persona's communication-style fingerprint, then loads two
 * JSON files of model responses (`--before` = pre-LoRA, `--after` =
 * post-LoRA), scores every response in both sets, and reports the
 * delta in mean fidelity. A positive delta indicates the LoRA
 * adapter is pulling the model toward persona fidelity; near-zero
 * or negative means the LoRA isn't doing useful personalization
 * work.
 *
 * JSON format (both files): a top-level array of strings, e.g.
 *   ["response 1", "response 2", ...]
 *
 * The CLI is a thin wrapper around
 * `hu_communication_style_compare_response_sets` plus a JSON
 * string-array loader. Pure CPU, no network. */
hu_error_t hu_ml_cli_lora_ab(hu_allocator_t *alloc, int argc, const char **argv);
/* Track D D2.2 — provider-driven LoRA response generator.
 *
 * Loads a persona's example bank, runs every `incoming` message
 * through a provider's chat() call, captures the response content,
 * and writes the responses as a JSON array to `--output`. The
 * intended workflow is two-pass:
 *
 *   1. `human ml lora-runner --persona X --output before.json`
 *        (no adapter loaded → captures the base model's responses)
 *   2. `human ml apply-adapter --provider embedded --adapter ...`
 *        (loads the LoRA into the active provider)
 *   3. `human ml lora-runner --persona X --output after.json`
 *        (same prompts, post-LoRA model)
 *   4. `human ml lora-ab --persona X --before before.json --after after.json`
 *
 * Pure CPU otherwise; the only non-determinism is the provider
 * itself. In HU_IS_TEST builds the provider call is mocked: the
 * runner echoes the example's existing `response` as the captured
 * content so JSON-write logic is exercisable in unit tests
 * without booting a real provider. */
hu_error_t hu_ml_cli_lora_runner(hu_allocator_t *alloc, int argc, const char **argv);

/* Track D D2.2 — fidelity-status JSON for dashboards & tooling.
 *
 * Emits a single-object JSON document with the persona-fidelity
 * metrics every UI / status surface needs to display the LoRA
 * health summary:
 *
 *   {
 *     "persona": "<name>",
 *     "fingerprint_source": "personal_model" | "synthetic",
 *     "baseline": { "scored": N, "mean": X, "min": X, "max": X },
 *     "ab": { "available": false } |
 *           { "available": true, "before_mean": X, "after_mean": X,
 *             "delta": X, "scored_before": N, "scored_after": N }
 *   }
 *
 * `--baseline-output` writes JSON to a file (or stdout when
 * unspecified). `--before` / `--after` are optional; when both are
 * provided the comparator runs and `ab.available` is true. */
hu_error_t hu_ml_cli_fidelity_status(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_train_feed_predictor(hu_allocator_t *alloc, int argc, const char **argv);
/* W13 — End-to-end adapter loading: open the local huml provider, call
 * hu_provider_load_adapter, print the active_adapter id. Closes the loop
 * "trained adapter on disk → loaded by provider runtime" so the W13
 * pipeline is verifiable from a single CLI command. */
hu_error_t hu_ml_cli_apply_adapter(hu_allocator_t *alloc, int argc, const char **argv);

/* US-7.10 + US-11.5 — `human ml rl-train --algorithm {dpo|simpo|orpo|grpo2}` router.
 *
 * `dpo`   → delegates to `hu_ml_cli_dpo_train` with the `--algorithm`
 *           flag pair stripped from argv (no behavior change vs the
 *           existing `human ml dpo-train` entry point — AC-7.10.4).
 * `simpo` → instantiates `hu_rl_trainer_simpo_create` and runs a single
 *           `train_step` against the supplied pair (AC-7.10.3). In
 *           `HU_IS_TEST` builds the model forward is mocked.
 * `orpo`  → instantiates `hu_rl_trainer_orpo_create` and runs a single
 *           `train_step` against the supplied pair (US-11.5 / AC-11.5.1).
 *           In `HU_IS_TEST` builds the model forward is mocked.
 * `grpo2` → emits a "not yet implemented" message and returns
 *           `HU_ERR_NOT_SUPPORTED` (exit code 2 — AC-7.10.5).
 *
 * Missing `--algorithm` flag returns `HU_ERR_INVALID_ARGUMENT`. */
hu_error_t hu_ml_cli_rl_train(hu_allocator_t *alloc, int argc, const char **argv);

/* US-11.8 — `human ml adapter-rollback`.
 *
 * Enumerates `<slow_dir>/slow.safetensors.v*`, picks `v{N}` = current
 * target (highest) and `v{N-1}` = previous, then:
 *   1. Quarantines `v{N}` to `<quarantine_dir>/<today>.safetensors`.
 *   2. Atomically rewires `<current_symlink>` -> `v{N-1}`.
 *
 * Required flags: --slow-dir, --quarantine-dir, --current. Optional:
 *   --today YYYY-MM-DD  (deterministic quarantine filename for tests)
 *
 * Fails with HU_ERR_TOOL_VALIDATION when there is no `v{N-1}` to roll
 * back to (operator must inspect quarantine manually). */
hu_error_t hu_ml_cli_adapter_rollback(hu_allocator_t *alloc, int argc, const char **argv);

/* Wave C — `human ml train-from-reactions`
 * Export preference pairs from memory.db (reaction / DPO collector) to JSONL,
 * then run KTO (default) or DPO training on that export. */
hu_error_t hu_ml_cli_train_from_reactions(hu_allocator_t *alloc, int argc, const char **argv);

#endif
