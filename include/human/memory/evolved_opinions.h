#ifndef HU_MEMORY_EVOLVED_OPINIONS_H
#define HU_MEMORY_EVOLVED_OPINIONS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/humanness.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Evolved opinion persistence — stores perspectives that develop through
 * repeated conversation, with conviction blending and interaction counting.
 *
 * Table: evolved_opinions (separate from F65 opinions in cognitive.h)
 */

#ifdef HU_ENABLE_SQLITE

#include <sqlite3.h>

hu_error_t hu_evolved_opinions_ensure_table(sqlite3 *db);

hu_error_t hu_evolved_opinion_upsert(sqlite3 *db, const char *topic, size_t topic_len,
                                     const char *stance, size_t stance_len, double conviction,
                                     int64_t now_ts);

/* Is an extracted (topic, stance) a position about the world, or the model
 * talking about itself? Measured 2026-09-13: in four months the extractor
 * stored exactly two rows, both from the twin's own replies and both
 * identity slips — "I think I just glitched for a second", "I think you
 * might be confusing me with someone else" — and both were then injected
 * into every live prompt as "positions you've arrived at through
 * experience". Rejects a topic that opens on a pronoun (the sentence is
 * about I/you, not a subject) and a stance carrying an AI/identity-slip
 * phrase. Pure; tests/test_opinions_persistence.c pins the two real rows. */
bool hu_evolved_opinion_stance_is_usable(const char *topic, size_t topic_len, const char *stance,
                                         size_t stance_len);

/* Extract opinionated statements from a response and upsert them.
 * Scans for "I think", "I believe", "I prefer", etc. and stores
 * the surrounding clause as topic+stance with moderate conviction.
 * Candidates that fail hu_evolved_opinion_stance_is_usable are skipped. */
hu_error_t hu_evolved_opinions_extract_and_store(sqlite3 *db, const char *response,
                                                 size_t response_len, int64_t now_ts);

hu_error_t hu_evolved_opinions_get(hu_allocator_t *alloc, sqlite3 *db, double min_conviction,
                                   size_t limit, hu_evolved_opinion_t **out, size_t *out_count);

void hu_evolved_opinions_free(hu_allocator_t *alloc, hu_evolved_opinion_t *opinions, size_t count);

/* ── Anti-sycophancy: check for existing opinion before agreeing ─────── */

/* Look up a single opinion by topic. Returns HU_OK and sets *found.
 * If found, fills *out (caller must free topic/stance via alloc). */
hu_error_t hu_evolved_opinion_find(hu_allocator_t *alloc, sqlite3 *db, const char *topic,
                                   size_t topic_len, hu_evolved_opinion_t *out, bool *found);

/* Build a directive string if there's an existing strong opinion on topic.
 * Returns allocated string or NULL if no relevant opinion exists.
 * Caller frees via alloc. */
char *hu_opinion_check_before_agree(hu_allocator_t *alloc, sqlite3 *db, const char *topic,
                                    size_t topic_len, size_t *out_len);

/* Decide whether to inject a contrarian prompt for a debatable topic.
 * Uses a simple hash-based ~15% budget. Returns allocated directive or NULL. */
char *hu_opinion_contrarian_prompt(hu_allocator_t *alloc, const char *topic, size_t topic_len,
                                   uint32_t turn_counter, size_t *out_len);

/* ── Opinion history tracking ──────────────────────────────────────────── */

typedef struct hu_opinion_history_entry {
    char *topic;
    size_t topic_len;
    char *old_stance;
    size_t old_stance_len;
    char *new_stance;
    size_t new_stance_len;
    char *change_reason;
    size_t change_reason_len;
    int64_t changed_at;
} hu_opinion_history_entry_t;

/* Ensure opinion_history table exists. */
hu_error_t hu_opinion_history_ensure_table(sqlite3 *db);

/* Record a stance change in the history table. */
hu_error_t hu_opinion_history_record(sqlite3 *db, const char *topic, size_t topic_len,
                                     const char *old_stance, size_t old_stance_len,
                                     const char *new_stance, size_t new_stance_len,
                                     const char *reason, size_t reason_len, int64_t changed_at);

/* Retrieve change history for a topic. Caller frees via hu_opinion_history_free. */
hu_error_t hu_evolved_opinion_history(hu_allocator_t *alloc, sqlite3 *db, const char *topic,
                                      size_t topic_len, hu_opinion_history_entry_t **out,
                                      size_t *out_count);

void hu_opinion_history_free(hu_allocator_t *alloc, hu_opinion_history_entry_t *entries,
                             size_t count);

/* Upsert with history tracking. If conviction shifts > threshold, records history.
 * Returns the shift narrative directive (allocated) or NULL. Caller frees.
 * opinion_changes_this_convo is checked against max 2 per conversation. */
char *hu_evolved_opinion_upsert_with_history(hu_allocator_t *alloc, sqlite3 *db, const char *topic,
                                             size_t topic_len, const char *stance,
                                             size_t stance_len, double conviction, int64_t now_ts,
                                             const char *reason, size_t reason_len,
                                             uint32_t opinion_changes_this_convo, size_t *out_len);

#endif /* HU_ENABLE_SQLITE */

#endif /* HU_MEMORY_EVOLVED_OPINIONS_H */
