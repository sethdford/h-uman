#ifndef HU_AGENT_OUTBOUND_PIPELINE_H
#define HU_AGENT_OUTBOUND_PIPELINE_H

/* Outbound validation pipeline — Sprint 59 SOTA outbound safety.
 *
 * Background: the 2026-05-26 Annie/Mindy/Betty incident shipped 16
 * unsafe messages to family contacts. Root cause: no single egress
 * funnel through which all outbound messages flowed. Each send path
 * (proactive, F25, temporal, scheduled, burst, reactive) had its own
 * ad-hoc validation, or none at all.
 *
 * This pipeline is the single funnel. Every outbound message flows
 * through `hu_outbound_pipeline_run`, which dispatches to a chain of
 * stages each with a typed verdict:
 *
 *   SEND       → pass-through; next stage runs
 *   REWRITE    → stage rewrote text; re-enter at stage[0] ONCE
 *   REGENERATE → re-prompt LLM with stricter system prompt ONCE
 *   REJECT     → drop, log, mark followed-up
 *
 * Stages are composable. Six stages ship in Sprint 59:
 *
 *   strip       — character normalization (U+FFFC, RTL overrides, ZWJ)
 *   shape       — length + sentence-structure validation
 *   echo        — semantic directive-echo detection (not hardcoded list)
 *   crosstalk   — cross-contact content bleed (5-gram Jaccard)
 *   persona     — Seth-voice fidelity check
 *   moderation  — violence/hate/self-harm/PII (wraps hu_moderation_check)
 *
 * Different paths get different stage configs — proactive needs all
 * six, scheduled needs only strip+crosstalk+moderation. See
 * pipeline_configs.c for the per-path table.
 *
 * Design doc: docs/plans/2026-05-26-sprint-59-outbound-safety/design.md
 *
 * Per ~/.claude/rules/silent-config-gated-subsystems.md: the pipeline
 * emits a one-shot info log on first invocation listing active stages
 * and per-path configs.
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Forward declarations — opaque to the pipeline. */
struct hu_persona;
struct hu_memory;

/* Verdict kinds — what a stage tells the pipeline to do. */
typedef enum hu_outbound_verdict_kind {
    HU_OUTBOUND_SEND = 0,   /* Pass to next stage (or deliver if last). */
    HU_OUTBOUND_REWRITE,    /* Stage modified `replacement`; re-enter at stage[0]. */
    HU_OUTBOUND_REGENERATE, /* Re-prompt LLM using regenerate_hint, re-run pipeline. */
    HU_OUTBOUND_REJECT,     /* Drop the message; log reason. */
} hu_outbound_verdict_kind_t;

/* Verdict — returned by every stage; the pipeline acts on `kind`.
 *
 * `reason` and `regenerate_hint` are STATIC strings (the stage points
 * to a string literal or a const-static buffer). The pipeline does
 * not free them.
 *
 * `replacement` is heap-allocated by the stage using the pipeline's
 * allocator; the pipeline OWNS it once returned and frees it after
 * applying or discarding.
 */
typedef struct hu_outbound_verdict {
    hu_outbound_verdict_kind_t kind;
    const char *reason;
    char *replacement;
    size_t replacement_len;
    const char *regenerate_hint;
} hu_outbound_verdict_t;

/* Outbound paths — different paths get different stage configs. */
typedef enum hu_outbound_path {
    HU_OUTBOUND_PATH_REACTIVE = 0, /* response_guard handles most; pipeline adds crosstalk */
    HU_OUTBOUND_PATH_PROACTIVE,    /* full pipeline */
    HU_OUTBOUND_PATH_F25,          /* full pipeline + topic-shape pre-check upstream */
    HU_OUTBOUND_PATH_TEMPORAL,     /* strip + shape + crosstalk + persona + moderation(light) */
    HU_OUTBOUND_PATH_SCHEDULED,    /* strip + crosstalk + moderation(light) */
    HU_OUTBOUND_PATH_BURST,        /* inherits primary's verdict — pipeline skipped */
    HU_OUTBOUND_PATH_COUNT
} hu_outbound_path_t;

/* Outbound message — mutable; stages MAY rewrite via REWRITE verdict. */
typedef struct hu_outbound_message {
    char *content;
    size_t content_len;
    const char *prompt_used; /* what we asked the LLM — for echo detection */
    size_t prompt_used_len;
} hu_outbound_message_t;

/* Outbound context — read-only inputs the pipeline + stages consult. */
typedef struct hu_outbound_context {
    const char *recipient_contact_id;
    size_t recipient_contact_id_len;
    struct hu_persona *persona;
    struct hu_memory *memory; /* for crosstalk lookup; may be NULL in tests */
    const char *channel_name; /* "imessage", "slack", etc. */
    hu_outbound_path_t path;
    int regenerate_budget; /* pipeline decrements; default 1 */
    hu_allocator_t *alloc; /* for verdict.replacement allocation */
} hu_outbound_context_t;

/* Stage — single concern, returns a verdict. */
typedef struct hu_outbound_stage {
    const char *name;
    hu_outbound_verdict_t (*run)(struct hu_outbound_stage *self, hu_outbound_message_t *msg,
                                 hu_outbound_context_t *ctx);
    void *state; /* stage-private state if needed; pipeline does not touch */
} hu_outbound_stage_t;

/* Pipeline — ordered list of stages. Opaque to callers; build via
 * hu_outbound_pipeline_for_path. */
typedef struct hu_outbound_pipeline hu_outbound_pipeline_t;

/* Build a pipeline for a given path. The returned pipeline borrows
 * static stage definitions from pipeline_configs.c — caller frees
 * with hu_outbound_pipeline_destroy. */
hu_error_t hu_outbound_pipeline_for_path(hu_allocator_t *alloc, hu_outbound_path_t path,
                                         hu_outbound_pipeline_t **out);

void hu_outbound_pipeline_destroy(hu_outbound_pipeline_t *pipeline);

/* Run the pipeline. On return:
 *   - If verdict.kind == SEND, `msg->content` is the final delivery payload.
 *   - If verdict.kind == REWRITE, that was applied before the verdict
 *     reached the caller — the caller treats it as SEND with new content.
 *   - If verdict.kind == REGENERATE, the caller is expected to re-prompt
 *     the LLM using `verdict.regenerate_hint` and call the pipeline again
 *     with the new content.
 *   - If verdict.kind == REJECT, the caller drops the message.
 *
 * The pipeline transparently handles REWRITE (re-entering at stage[0]
 * once). REGENERATE bubbles up to the caller — the caller controls the
 * LLM call.
 *
 * `out_verdict.replacement` is owned by `out_verdict` and freed via
 * hu_outbound_verdict_clear when caller is done with it.
 */
hu_error_t hu_outbound_pipeline_run(hu_outbound_pipeline_t *pipeline, hu_outbound_message_t *msg,
                                    hu_outbound_context_t *ctx, hu_outbound_verdict_t *out_verdict);

/* Free heap-owned fields of a verdict. Safe to call on a SEND verdict
 * (no-op when replacement is NULL). */
void hu_outbound_verdict_clear(hu_outbound_verdict_t *verdict, hu_allocator_t *alloc);

/* Convenience constructors for stage authors — return a verdict by value. */
hu_outbound_verdict_t hu_outbound_verdict_send(void);
hu_outbound_verdict_t hu_outbound_verdict_reject(const char *reason);
hu_outbound_verdict_t hu_outbound_verdict_regenerate(const char *reason, const char *hint);
/* Caller transfers ownership of `replacement` to the verdict; the
 * pipeline allocator must match `alloc`. */
hu_outbound_verdict_t hu_outbound_verdict_rewrite(const char *reason, char *replacement,
                                                  size_t replacement_len);

/* Human-readable stage name → enum (for logging / configs). NULL if unknown. */
const char *hu_outbound_path_name(hu_outbound_path_t path);

/* ----------------------------------------------------------------- */
/* Crosstalk stage — exposed for testing and production wiring       */
/* ----------------------------------------------------------------- */

/* Pure Jaccard predicate over char-5-gram sets. Lowercases and
 * strips punctuation before n-gramming. Returns score in [0,1]. */
double hu_outbound_crosstalk_jaccard_5gram(hu_allocator_t *alloc, const char *a, size_t a_len,
                                           const char *b, size_t b_len);

/* Callback type for cross-contact corpus lookup. Implementations
 * MUST allocate `out_texts` (array of `out_count` heap strings) via
 * the provided allocator, and each string is NUL-terminated. The
 * stage frees both the strings and the array via the same allocator.
 *
 * Return 0 on success, -1 on error.
 *
 * Production wires this to a SQLite query over the `messages` table
 * filtered to contact_id != exclude_contact_id AND ts > now - 7d.
 * Tests inject a static fake corpus. */
typedef int (*hu_outbound_crosstalk_lookup_fn_t)(void *userdata, hu_allocator_t *alloc,
                                                 const char *exclude_contact_id,
                                                 size_t exclude_contact_id_len, char ***out_texts,
                                                 size_t *out_count);

/* Register the lookup callback. NULL clears it (then crosstalk
 * cross-contact check is a no-op; metadata-pattern check still runs).
 * Process-wide, not per-pipeline. */
void hu_outbound_crosstalk_set_lookup(hu_outbound_crosstalk_lookup_fn_t fn, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_OUTBOUND_PIPELINE_H */
