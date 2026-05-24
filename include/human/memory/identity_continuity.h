/* include/human/memory/identity_continuity.h
 *
 * Persistent identity continuity — Sprint B Story 8 (2026-05-19).
 *
 * When "Alice" appears under a NEW handle for the first time (she
 * switched phone numbers; she signed up for Discord; she added a
 * work email), the identity graph won't merge automatically — but
 * the agent should at least NOTICE and surface a "merge candidate"
 * for user confirmation.
 *
 * This module compares unknown handles seen in the personal model
 * against canonical_names already in the identity graph and emits a
 * single-line "IDENTITY:" prompt block when a plausible match is
 * found.
 *
 * Pure read-only — no graph mutation. The user runs hu_identity_save
 * themselves if they want to commit the merge. This module's job is
 * just to SURFACE the candidate.
 *
 * Anti-goals:
 *   - Don't auto-merge (HIGH-confidence-only is identity_resolver's
 *     job; this is the soft-suggestion layer)
 *   - Don't flood the prompt — surface at most ONE candidate per call
 *   - Only suggest when there's a clear lexical signal (display-name
 *     overlap, exact email-local-part overlap, etc.)
 */
#ifndef HU_MEMORY_IDENTITY_CONTINUITY_H
#define HU_MEMORY_IDENTITY_CONTINUITY_H

#include "human/core/error.h"
#include "human/memory/identity_resolver.h" /* hu_identity_graph_t is a typedef of an anonymous struct */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_personal_model;

/* Scan the personal model for handles NOT yet in the identity graph
 * and compare them against canonical_names in the graph. Returns the
 * single best candidate as a rendered "IDENTITY:" prompt block.
 *
 * Output shape:
 *   "IDENTITY: \"alice@new-phone\" may be same person as Alice
 *    (shared first-name token)."
 *
 * Returns 0 when no candidate is found (graph is empty, all handles
 * already merged, or no lexical signal exists).
 *
 * Pure — no I/O, no graph mutation. */
size_t hu_identity_continuity_suggest(const struct hu_personal_model *model,
                                      const hu_identity_graph_t *graph, char *out, size_t cap);

/* Internal helper exposed for testing: extract the first lowercase
 * word-token (alpha chars only) from `s` into `out`. Returns bytes
 * written. Used to compute first-name overlap. */
size_t hu_identity_continuity_first_token_lower(const char *s, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
#endif /* HU_MEMORY_IDENTITY_CONTINUITY_H */
