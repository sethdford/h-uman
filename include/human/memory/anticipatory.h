/* include/human/memory/anticipatory.h
 *
 * Anticipatory memory surfacing — Sprint B Story 7 (2026-05-19).
 *
 * Before the agent composes a reply to Alice, surface upcoming events
 * about her that are mentioned in the personal model:
 *
 *   "Alice's birthday is coming up (mentioned 8 days ago)."
 *   "Alice mentioned a trip to Japan next week."
 *
 * Architecture: pure read-only scan over personal_model facts, similar
 * to emotional_context but with the OPPOSITE temporal orientation —
 * we surface facts about FUTURE events the user should be primed for,
 * not past events that need sensitivity.
 *
 * Lexicon (English-only, extensible): birthday, anniversary, wedding,
 * trip, vacation, holiday, graduation, interview, exam, conference,
 * deadline, due, surgery, appointment, moving, baby, retirement.
 *
 * Anti-goals (extension of the B2 anti-goals):
 *   - Don't INFER events from message text; only trigger on explicit
 *     lexicon keywords + a recent mention
 *   - Don't suggest what to say — surface the fact, let the agent
 *     decide tone
 *   - One fact per contact (most-recently-mentioned wins; no aggregation)
 */
#ifndef HU_MEMORY_ANTICIPATORY_H
#define HU_MEMORY_ANTICIPATORY_H

#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct hu_personal_model;

/* Default lookback. Upcoming-event mentions are surfaced for 14 days
 * after they were observed. The event itself may be further out, but
 * the MENTION half-life is shorter than emotional context (30d) —
 * stale event mentions get awkward fast ("you mentioned your
 * graduation? — that was 60 days ago"). */
#define HU_ANTICIPATORY_LOOKBACK_DEFAULT_SEC ((int64_t)(14LL * 24 * 60 * 60))

/* Effective-confidence floor. Same threshold as emotional_context. */
#define HU_ANTICIPATORY_MIN_CONFIDENCE 0.4f

/* Render the most-recent upcoming-event fact about `contact_handle`,
 * formatted as a single line prefixed with "UPCOMING:". Pure function.
 *
 * Inputs:
 *   model            — required
 *   contact_handle   — required (case-insensitive provenance match)
 *   now              — current unix time (0 → wall clock; returns 0
 *                       under HU_IS_TEST for determinism)
 *   lookback_seconds — 0 → DEFAULT (14 days)
 *   out, cap         — caller-owned buffer
 *
 * Returns bytes written (excluding NUL). 0 when no match.
 *
 * Output shape:
 *   "UPCOMING: <handle> mentioned: <s> <p> <o>."
 */
size_t hu_anticipatory_for_contact(const struct hu_personal_model *model,
                                   const char *contact_handle, int64_t now,
                                   int64_t lookback_seconds, char *out, size_t cap);

/* Word-boundary lexicon match; reuses the same predicate logic as
 * emotional_context but with the anticipatory lexicon. Pure; exposed
 * for unit testing. */
bool hu_anticipatory_lexicon_word_match(const char *haystack, const char *needle);

#ifdef __cplusplus
}
#endif
#endif /* HU_MEMORY_ANTICIPATORY_H */
