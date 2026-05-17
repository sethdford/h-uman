#ifndef HU_PROACTIVE_H
#define HU_PROACTIVE_H

#include "human/agent/commitment.h"
#include "human/context/event_extract.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include "human/persona.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HU_PROACTIVE_MAX_ACTIONS 32

typedef enum hu_proactive_action_type {
    HU_PROACTIVE_COMMITMENT_FOLLOW_UP,
    HU_PROACTIVE_MILESTONE,
    HU_PROACTIVE_CHECK_IN,
    HU_PROACTIVE_MORNING_BRIEFING,
    HU_PROACTIVE_PATTERN_INSIGHT,
    HU_PROACTIVE_REMINDER,
    HU_PROACTIVE_IMPORTANT_DATE,
    HU_PROACTIVE_INSIDE_JOKE,
    HU_PROACTIVE_TOPIC_ABSENCE,
    HU_PROACTIVE_GROWTH_CELEBRATION,
    HU_PROACTIVE_CURIOSITY,
    HU_PROACTIVE_CALLBACK,
} hu_proactive_action_type_t;

typedef struct hu_proactive_action {
    hu_proactive_action_type_t type;
    char *message;
    size_t message_len;
    double priority;
} hu_proactive_action_t;

typedef struct hu_proactive_result {
    hu_proactive_action_t actions[HU_PROACTIVE_MAX_ACTIONS];
    size_t count;
} hu_proactive_result_t;

typedef struct hu_silence_config {
    uint32_t threshold_hours;
    bool enabled;
} hu_silence_config_t;

#define HU_SILENCE_DEFAULTS {.threshold_hours = 72, .enabled = true}

hu_error_t hu_proactive_check_silence(hu_allocator_t *alloc, uint64_t last_contact_ms,
                                      uint64_t now_ms, const hu_silence_config_t *config,
                                      hu_proactive_result_t *out);
hu_error_t hu_proactive_check_reminder(hu_allocator_t *alloc, const char *contact_id,
                                       size_t contact_id_len, const char *interests,
                                       size_t interests_len, uint64_t now_ms,
                                       uint64_t last_reminder_ms, hu_proactive_result_t *out);
uint32_t hu_proactive_backoff_hours(uint32_t consecutive_unanswered);
hu_error_t hu_proactive_check(hu_allocator_t *alloc, uint32_t session_count, uint8_t hour,
                              hu_proactive_result_t *out);
hu_error_t hu_proactive_check_events(hu_allocator_t *alloc, const hu_extracted_event_t *events,
                                     size_t event_count, hu_proactive_result_t *out);
hu_error_t hu_proactive_check_extended(hu_allocator_t *alloc, uint32_t session_count, uint8_t hour,
                                       const hu_commitment_t *commitments, size_t commitment_count,
                                       const char *const *pattern_subjects,
                                       const uint32_t *pattern_counts, size_t pattern_count,
                                       hu_proactive_result_t *out);
hu_error_t hu_proactive_build_context(const hu_proactive_result_t *result, hu_allocator_t *alloc,
                                      size_t max_actions, char **out, size_t *out_len);
hu_error_t hu_proactive_build_starter(hu_allocator_t *alloc, hu_memory_t *memory,
                                      const char *contact_id, size_t contact_id_len, char **out,
                                      size_t *out_len);
void hu_proactive_result_deinit(hu_proactive_result_t *result, hu_allocator_t *alloc);

bool hu_proactive_check_important_dates(const hu_persona_t *persona, const char *contact_id,
                                        size_t contact_id_len, int month, int day,
                                        char *message_out, size_t msg_cap, char *type_out,
                                        size_t type_cap);

/* F30: Spontaneous curiosity — random genuine questions from memory (10–15% per proactive cycle) */
bool hu_proactive_check_curiosity(hu_allocator_t *alloc, hu_memory_t *memory,
                                  const char *contact_id, size_t contact_id_len, uint32_t seed,
                                  char *message_out, size_t msg_cap);

/* F31: Callback opportunities — reference previous conversations (25–35% per conversation start) */
bool hu_proactive_check_callbacks(hu_allocator_t *alloc, hu_memory_t *memory,
                                  const char *contact_id, size_t contact_id_len, uint32_t seed,
                                  char *message_out, size_t msg_cap);

/* Build the per-contact replay-insights memory key.
 *
 * Returns true and writes "replay:<contact_id>:latest" (NUL-terminated) into
 * out_buf, with the byte count (excluding NUL) at *out_len.  Returns false on
 * NULL/empty inputs or if out_buf_cap is too small.
 *
 * Originated from the 2026-05-16 incident: src/daemon.c stored replay
 * insights under the global key "replay:latest" with a process-global static
 * fallback buffer, so every contact's prompt received every other contact's
 * replay-analysis blob.  This helper makes the contact scope explicit and
 * unit-testable.  Pinned by tests/test_proactive.c. */
bool hu_proactive_build_replay_key(const char *contact_id, size_t contact_id_len, char *out_buf,
                                   size_t out_buf_cap, size_t *out_len);

/* Strict contact-match predicate for F25 emotional check-ins.
 *
 * Returns true iff the contact profile is the actual originator of the
 * emotional moment.  Match is by exact `contact_id` equality only — the
 * previous implementation at daemon.c:975-984 also tried matching the
 * moment's contact_id against `proactive_channel` (whole and after-colon
 * suffix), which on 2026-05-16 routed Mindy's emotional confession to
 * three contacts whose proactive_channel handles collided.
 *
 * Any non-null, non-empty argument is accepted; everything else returns
 * false.  Pinned by tests/test_proactive.c. */
bool hu_proactive_contact_matches_moment(const char *cp_contact_id, const char *moment_contact_id);

/* Outbound topic safety predicate. Returns true iff `topic` is acceptable to
 * interpolate into a user-visible proactive message.
 *
 * Rejects (returns false) when topic is null/empty/oversized or contains any of:
 *  - the recall debug-format substring "(last:"
 *  - newlines, carriage returns, tabs, or printf format specifiers
 *  - first-person pronouns (i, me, my, mine, i'm, i am)
 *  - emotion/affect keywords that indicate the "topic" is actually a confession
 *
 * Extracted as a pure predicate so it can be unit-tested without crossing the
 * F25 send boundary (see .claude/rules/security-predicate-extraction.md).
 *
 * Pinned by tests/test_proactive.c regression suite. Originated from the
 * 2026-05-16 incident: F25 shipped a relative's raw emotional confession to
 * three family contacts because `m->topic` was a raw 60–255-char window of
 * the user's own text. This predicate is the outbound safety net. */
bool hu_proactive_topic_is_safe(const char *topic, size_t topic_len);

#endif /* HU_PROACTIVE_H */
