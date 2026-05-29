#ifndef HU_ML_DPO_H
#define HU_ML_DPO_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/provider.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>
#endif

typedef struct hu_preference_pair {
    char prompt[2048];
    size_t prompt_len;
    char chosen[4096];
    size_t chosen_len;
    char rejected[4096];
    size_t rejected_len;
    double margin;
    int64_t timestamp;
    char source[64];
    size_t source_len;
} hu_preference_pair_t;

typedef struct hu_dpo_collector {
    hu_allocator_t *alloc;
#ifdef HU_ENABLE_SQLITE
    sqlite3 *db;
#else
    void *db;
#endif
    size_t pair_count;
    size_t max_pairs;
} hu_dpo_collector_t;

hu_error_t hu_dpo_collector_create(hu_allocator_t *alloc,
#ifdef HU_ENABLE_SQLITE
                                   sqlite3 *db,
#else
                                   void *db,
#endif
                                   size_t max_pairs, hu_dpo_collector_t *out);
void hu_dpo_collector_deinit(hu_dpo_collector_t *collector);
hu_error_t hu_dpo_init_tables(hu_dpo_collector_t *collector);

hu_error_t hu_dpo_record_pair(hu_dpo_collector_t *collector, const hu_preference_pair_t *pair);

hu_error_t hu_dpo_record_from_feedback(hu_dpo_collector_t *collector, const char *prompt,
                                       size_t prompt_len, const char *response, size_t response_len,
                                       bool positive);

hu_error_t hu_dpo_record_from_retry(hu_dpo_collector_t *collector, const char *prompt,
                                    size_t prompt_len, const char *rejected, size_t rejected_len,
                                    const char *chosen, size_t chosen_len);

hu_error_t hu_dpo_export_jsonl(hu_dpo_collector_t *collector, const char *path, size_t path_len,
                               size_t *exported_count);

hu_error_t hu_dpo_pair_count(hu_dpo_collector_t *collector, size_t *out);
hu_error_t hu_dpo_clear(hu_dpo_collector_t *collector);

/* AGI Capability-1: production outcomes — every outbound message
 * generates a row in production_outcomes. Outcome columns (tapback,
 * reply_latency, etc.) fill later as reactions arrive; a nightly job
 * generates dpo_pairs from resolved rows. See
 * docs/plans/2026-05-19-agi-path.md.
 *
 * Caller passes:
 *   channel       — stable lowercase channel name ("imessage", "slack")
 *   target        — contact ID / chat ID this message went to
 *   message_ref   — channel-specific message ID for matching incoming
 *                   reactions (may be NULL if not yet known; outcome
 *                   updater will look up by (channel,target,timestamp))
 *   prompt        — the incoming user text / assembled context
 *   chosen        — the response we sent
 *   p_seth_at_send — PersonaEval P(Seth) on `chosen` at send time
 *                    (set to -1.0 if classifier unavailable)
 *   alternatives_json — Sprint 46 R5.2 — when L5 best-of-N fires, this
 *                       is the JSON array of LOSING candidates (the ones
 *                       not chosen). Pass NULL/0 when L5 didn't fire or
 *                       only one candidate was generated. Read by
 *                       scripts/outcomes_to_dpo.py to materialize DPO
 *                       pairs (chosen > rejected) once the outcome
 *                       resolves.
 *
 * Returns HU_OK on success, HU_ERR_INVALID_ARGUMENT for null args,
 * HU_ERR_IO on SQLite failure. Skips silently when SQLITE is disabled. */
hu_error_t hu_dpo_record_outbound(hu_dpo_collector_t *collector, const char *channel,
                                  size_t channel_len, const char *target, size_t target_len,
                                  const char *message_ref, size_t message_ref_len,
                                  const char *prompt, size_t prompt_len, const char *chosen,
                                  size_t chosen_len, double p_seth_at_send,
                                  const char *alternatives_json, size_t alternatives_json_len);

/* AGI Capability-1b: outcome update — fill outcome columns on the
 * production_outcomes row that matches (channel, target, message_ref).
 * Polarity: +1 positive reaction, -1 negative, 0 ambiguous.
 * reply_latency_s: seconds between our send and the next inbound from
 *                  target on the same channel. Use -1 if "no reply yet".
 * reply_length: chars of the next inbound (-1 if no reply yet).
 *
 * Either polarity OR reply_latency_s should be non-default (not both
 * required — different signal sources resolve different ways). The
 * outcome is "resolved" once any of the columns is filled. */
hu_error_t hu_dpo_record_outcome(hu_dpo_collector_t *collector, const char *channel,
                                 size_t channel_len, const char *target, size_t target_len,
                                 const char *message_ref, size_t message_ref_len,
                                 int tapback_polarity, int reply_latency_s, int reply_length);

/* Sprint 46 R5.1 — latency ingest helper.
 *
 * Convenience wrapper called from the iMessage (or any channel's)
 * inbound dispatch path. Internally:
 *   1. Looks up the most-recent unresolved production_outcomes row
 *      for (channel, target).
 *   2. Computes latency = now - send_timestamp.
 *   3. Calls hu_dpo_record_outcome with reply_latency_s set.
 *
 * If no matching unresolved row exists (e.g. contact texted us without
 * a prior outbound), this is a no-op returning HU_OK.
 *
 * `inbound_length` is the byte length of the incoming text; passed to
 * record_outcome's reply_length column. Pass -1 if unknown.
 */
hu_error_t hu_dpo_record_inbound_arrival(hu_dpo_collector_t *collector, const char *channel,
                                         size_t channel_len, const char *target, size_t target_len,
                                         int inbound_length);

typedef struct hu_dpo_export {
    hu_preference_pair_t *pairs;
    size_t count;
} hu_dpo_export_t;

/* In-memory companion to hu_dpo_export_jsonl. */
hu_error_t hu_dpo_export(hu_dpo_collector_t *collector, hu_allocator_t *alloc,
                         hu_dpo_export_t *out);

void hu_dpo_export_free(hu_allocator_t *alloc, hu_dpo_export_t *export_data);

/* Build a prompt fragment from top-margin preference pairs (few-shot injection). */
hu_error_t hu_dpo_get_best_examples(hu_dpo_collector_t *collector, hu_allocator_t *alloc,
                                    size_t max_examples, char **out_prompt_fragment,
                                    size_t *out_len);

/* Result of a single judge-scored preference-pair batch.
 *
 * Phase 0 rename: the canonical name is `hu_dpo_judge_result_t`. The
 * legacy alias `hu_dpo_train_result_t` is preserved as a deprecated
 * typedef so existing code keeps compiling. The struct shape is
 * identical — only the name changed. See `hu_dpo_judge_step` below
 * for why the rename matters. */
typedef struct hu_dpo_judge_result {
    double loss;            /* Average aggregated judge loss across the batch */
    double alignment_score; /* Fraction of pairs where chosen > rejected (0.0-1.0) */
    size_t pairs_evaluated; /* Number of pairs processed */
    size_t pairs_aligned;   /* Number the judge ranked chosen > rejected */
} hu_dpo_judge_result_t;

/* Run one judge-scored preference-pair evaluation pass.
 *
 * NOT real DPO. There is no policy log-prob, no reference-model
 * log-prob, no gradient on policy weights. This function asks an
 * external LLM (the "judge") to score each preference pair and
 * aggregates the scores into a synthetic loss for reporting.
 * Real DPO with policy gradients lands in Phase 2 as
 * `hu_dpo_real_step` (see docs/plans/2026-05-11-full-sota-rl-improvement-loop-design.md
 * §1.5.2 issue #5). Until then, the name `_judge_step` keeps the
 * M3 narrative honest; the legacy `_train_step` is preserved as a
 * deprecated inline shim below.
 *
 * `beta`: judge-loss temperature (typically 0.1–0.5). `batch_size`: max
 * pairs per step (0 = all). Requires HU_ENABLE_SQLITE. Without it,
 * returns HU_ERR_NOT_SUPPORTED. */
hu_error_t hu_dpo_judge_step(hu_dpo_collector_t *collector, hu_allocator_t *alloc,
                             hu_provider_t *provider, const char *model, size_t model_len,
                             double beta, size_t batch_size, hu_dpo_judge_result_t *out);

/* Deprecated: renamed to `hu_dpo_judge_result_t` in Phase 0. The struct
 * shape is unchanged; this typedef preserves source compatibility for
 * out-of-tree callers. */
typedef hu_dpo_judge_result_t hu_dpo_train_result_t;

/* Minimum judge alignment (fraction of pairs where the judge ranked chosen >
 * rejected) required before the RLAIF nightly applies a persona style patch
 * derived from the batch's "best examples". Observed live: noise batches sit
 * at alignment 0.00-0.03 (loss ~0.693, the random baseline) while batches with
 * real signal reach ~0.94. A 0.6 majority bar cleanly separates them. */
#define HU_RLAIF_MIN_ALIGNMENT_TO_PATCH 0.6

/* Gate for the RLAIF nightly self-improvement: returns true only when the
 * judge result shows REAL preference signal, so the nightly never patches the
 * persona from a noise batch (which would drift the persona for no reason).
 * Pure predicate (no I/O) — unit-tested in tests/test_dpo.c. Requires a
 * non-empty batch whose alignment_score clears HU_RLAIF_MIN_ALIGNMENT_TO_PATCH. */
bool hu_rlaif_should_apply_style_patch(const hu_dpo_judge_result_t *result);

/* Parse the leading integer score (0-100) the judge LLM was asked to emit
 * ("Output ONLY a number"). Returns true and sets *score_out only when a digit
 * run is found; false for NULL/empty/no-digit output — which is what a failed
 * judge call OR a thinking-only/empty reply produces. Callers MUST SKIP a pair
 * when this returns false, never substitute a neutral 50: a fabricated tie
 * manufactures alignment=0 / loss=ln(2) noise (the exact signature that
 * polluted the nightly when slow 31B judge calls timed out). Pure (no I/O) —
 * unit-tested in tests/test_dpo.c. Scores above 100 are clamped to 100. */
bool hu_dpo_parse_judge_score(const char *out, size_t out_len, double *score_out);

/* Deprecated: renamed to `hu_dpo_judge_step` in Phase 0. The shim
 * forwards every argument verbatim so the result is bit-identical to
 * a direct call (pinned by tests/test_dpo_judge_naming.c). */
__attribute__((
    deprecated("renamed to hu_dpo_judge_step in Phase 0; "
               "this is a cloud-LLM judge step, not policy-gradient "
               "DPO. Real DPO is hu_dpo_real_step in Phase 2."))) static inline hu_error_t
hu_dpo_train_step(hu_dpo_collector_t *collector, hu_allocator_t *alloc, hu_provider_t *provider,
                  const char *model, size_t model_len, double beta, size_t batch_size,
                  hu_dpo_train_result_t *out) {
    return hu_dpo_judge_step(collector, alloc, provider, model, model_len, beta, batch_size, out);
}

#endif /* HU_ML_DPO_H */
