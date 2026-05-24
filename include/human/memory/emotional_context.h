/* include/human/memory/emotional_context.h
 *
 * Cross-conversation emotional memory — Sprint B Story 2 (2026-05-19).
 *
 * Why this exists: Alice mentions on iMessage that her mother is sick.
 * Tomorrow you reply on Slack. The persona prompt should surface "Alice
 * recently mentioned her mother is sick" BEFORE the persona body, so
 * the agent's reply is sensitive without needing the user to remember.
 *
 * Architecture: pure read-only scan over the personal model's fact
 * array. No new state, no separate store. Filters facts by:
 *
 *   1. Provenance contact_handle matches the requested contact
 *      (exact case-insensitive match — identity-resolver canonicalizes
 *       handles BEFORE personal-model ingest, so this works
 *       cross-channel for HIGH-confidence merged contacts).
 *
 *   2. last_seen_at within `lookback_seconds` of `now`
 *      (default 30 days; older tender facts are stale grief that the
 *       user has likely processed).
 *
 *   3. Subject, predicate, or object contains a token from the
 *      tender-emotional lexicon (word-boundary match — "sick of work"
 *      MUST NOT trigger; "her mother is sick" MUST).
 *
 *   4. Effective confidence (post-decay) >= 0.4 — drops noisy low-
 *      confidence extractions.
 *
 * Render: one or two sentences, prefixed with "EMOTIONAL CONTEXT:"
 * so the prompt block is grep-distinct from the persona body.
 *
 * Anti-goals (from the backlog):
 *   - Don't INFER emotions from message text directly (expensive,
 *     unreliable). Trigger only on explicit lexicon keyword + recent ts.
 *   - Don't autocomplete sensitivity ("you should ask about her").
 *     Surface the FACT, let the agent decide tone.
 *   - English-only lexicon for now (extension point left in the
 *     lexicon array — adding entries requires no API change).
 *   - Only the MOST RECENT matching fact is surfaced; we don't merge
 *     multi-event histories.
 */
#ifndef HU_MEMORY_EMOTIONAL_CONTEXT_H
#define HU_MEMORY_EMOTIONAL_CONTEXT_H

#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_personal_model;

/* Default lookback window. 30 days is the backlog spec; tender events
 * older than that are typically reframed or resolved, and surfacing
 * them risks awkwardness ("you mentioned the funeral 6 months ago"). */
#define HU_EMOTIONAL_CONTEXT_LOOKBACK_DEFAULT_SEC ((int64_t)(30LL * 24 * 60 * 60))

/* Effective-confidence floor. Below this, even a keyword-positive fact
 * is too noisy to surface. Tuned with fact_extract.h's typical
 * confidence range (0.5–0.9 for clean extractions). */
#define HU_EMOTIONAL_CONTEXT_MIN_CONFIDENCE 0.4f

/* Render the most-recent tender emotional fact about `contact_handle`
 * within the lookback window, formatted as a single-line summary
 * prefixed with "EMOTIONAL CONTEXT:". Pure function — no I/O, no
 * allocation beyond the caller's buffer.
 *
 * Inputs:
 *   model            — required. NULL or empty model writes nothing.
 *   contact_handle   — required. Case-insensitive match against fact
 *                       provenance contact_handle.
 *   now              — current unix time (seconds). Pass 0 to use the
 *                       wall clock via time(NULL) (HU_IS_TEST guard
 *                       writes deterministic value).
 *   lookback_seconds — 0 → DEFAULT (30 days).
 *   out              — caller-owned buffer; NUL-terminated when cap > 0.
 *   cap              — buffer capacity.
 *
 * Returns bytes written (excluding NUL). Returns 0 when:
 *   - any required pointer is NULL,
 *   - model has no facts matching the contact + lexicon + lookback,
 *   - matching fact's effective confidence < MIN_CONFIDENCE.
 *
 * Output shape (illustrative):
 *   "EMOTIONAL CONTEXT: Alice recently mentioned: her mother is sick."
 *
 * Deterministic across calls when inputs don't change. Safe to call
 * from the per-turn prompt-rendering path (no syscalls in fast path). */
size_t hu_emotional_context_for_contact(const struct hu_personal_model *model,
                                        const char *contact_handle, int64_t now,
                                        int64_t lookback_seconds, char *out, size_t cap);

/* Internal helper exposed for unit testing: word-boundary case-
 * insensitive substring match. Returns true when `needle` appears in
 * `haystack` bounded on both sides by start/end-of-string or a
 * non-alphanumeric character. Catches the classic "sick of work"
 * false-positive that a naive strstr/strncasecmp would miss. */
bool hu_emotional_context_lexicon_word_match(const char *haystack, const char *needle);

#ifdef __cplusplus
}
#endif
#endif /* HU_MEMORY_EMOTIONAL_CONTEXT_H */
