#ifndef HU_PROACTIVE_H
#define HU_PROACTIVE_H

#include "human/agent/commitment.h"
#include "human/context/event_extract.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
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

#endif /* HU_PROACTIVE_H */
