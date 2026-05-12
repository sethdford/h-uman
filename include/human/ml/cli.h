#ifndef HU_ML_CLI_H
#define HU_ML_CLI_H

#include "human/core/allocator.h"
#include "human/core/error.h"

hu_error_t hu_ml_cli_train(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_experiment(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_prepare(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_status(hu_allocator_t *alloc, int argc, const char **argv);
hu_error_t hu_ml_cli_dpo_train(hu_allocator_t *alloc, int argc, const char **argv);
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

/* SOTA-2026 init-07 — Train a Process Reward Model (PRM) scorer on the
 * existing DPO pair format from S1 and emit a `.prm` checkpoint that
 * `hu_verifier_panel_create` can load. Reuses the GPT scaffold; the
 * output head is a single sigmoid scalar per step.
 *
 * S2 deliverable: panel + training-driver CLI lands; real gradient-
 * descent training arrives in S3 (M3 frontier bridge dependency). The
 * S2 driver writes a deterministic checkpoint seeded by `--seed` and
 * the DPO pair count, so the panel runtime + checkpoint format can be
 * exercised end to end without a real ML training run.
 *
 * Usage: human ml train-verifier --output PATH [--db PATH] [--seed N]
 *                                [--feature-dim N] */
hu_error_t hu_ml_cli_train_verifier(hu_allocator_t *alloc, int argc, const char **argv);

#endif
