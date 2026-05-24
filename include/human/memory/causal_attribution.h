/* include/human/memory/causal_attribution.h
 *
 * Causal attribution — Sprint B Story 6 (2026-05-19).
 *
 * "What works with Alice?" Scan the personal model for reaction facts
 * tagged to a contact, count positive vs negative, and render a
 * single line operators can read at a glance.
 *
 * Pure read-only — no I/O, no allocation beyond the caller's buffer.
 *
 * Why this is in scope: the reaction-ingest pipeline (Sprint A) has
 * been storing per-fact provenance + source_hint for months; we have
 * the raw signal. Until now it lived only in the prompt as topic
 * summaries. This module surfaces the OUTCOME shape distinctly,
 * which is what the user actually wants to know ("is what I'm doing
 * working?"). */
#ifndef HU_MEMORY_CAUSAL_ATTRIBUTION_H
#define HU_MEMORY_CAUSAL_ATTRIBUTION_H

#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_personal_model;

typedef struct hu_causal_attribution_summary {
    int32_t total_reactions; /* count of reaction-derived facts for the contact */
    int32_t positive_count;  /* fact->predicate contains "loves" / "likes" / etc. */
    int32_t negative_count;  /* fact->predicate contains "hates" / "dislikes" / etc. */
    int32_t neutral_count;   /* total - (positive + negative) */
    int64_t earliest_seen;   /* unix; 0 when none */
    int64_t latest_seen;     /* unix; 0 when none */
} hu_causal_attribution_summary_t;

/* Scan personal model facts where source_hint == "reaction_ingest" and
 * provenance.contact_handle == contact_handle. Returns aggregated
 * counts. Pure; safe to call from prompt-rendering paths. */
size_t hu_causal_attribution_summarize(const struct hu_personal_model *model,
                                       const char *contact_handle,
                                       hu_causal_attribution_summary_t *out);

/* Render the summary as a one-line prompt block prefixed
 * "WHAT WORKS:". Returns bytes written, or 0 when total_reactions==0
 * (nothing to surface). Output shape:
 *
 *   "WHAT WORKS: alice — 5 positive / 1 negative across 6 reactions (last 7d)."
 */
size_t hu_causal_attribution_render(const char *contact_handle,
                                    const hu_causal_attribution_summary_t *summary, int64_t now,
                                    char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* HU_MEMORY_CAUSAL_ATTRIBUTION_H */
