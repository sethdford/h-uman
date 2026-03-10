#ifndef HU_AGENT_COMMITMENT_H
#define HU_AGENT_COMMITMENT_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

/* ──────────────────────────────────────────────────────────────────────────
 * Commitment detection — promises, intentions, reminders, goals from text
 * ────────────────────────────────────────────────────────────────────────── */

typedef enum hu_commitment_type {
    HU_COMMITMENT_PROMISE,
    HU_COMMITMENT_INTENTION,
    HU_COMMITMENT_REMINDER,
    HU_COMMITMENT_GOAL,
} hu_commitment_type_t;

typedef enum hu_commitment_status {
    HU_COMMITMENT_ACTIVE,
    HU_COMMITMENT_COMPLETED,
    HU_COMMITMENT_EXPIRED,
    HU_COMMITMENT_CANCELLED,
} hu_commitment_status_t;

typedef struct hu_commitment {
    char *id;           /* owned */
    char *statement;    /* owned; full text */
    size_t statement_len;
    char *summary;      /* owned; extracted clause */
    size_t summary_len;
    hu_commitment_type_t type;
    hu_commitment_status_t status;
    char *created_at;   /* owned; ISO8601 */
    char *emotional_weight; /* owned; optional, e.g. "0.5" */
    char *owner;        /* owned; "user" or "assistant" */
} hu_commitment_t;

#define HU_COMMITMENT_DETECT_MAX 5

typedef struct hu_commitment_detect_result {
    hu_commitment_t commitments[HU_COMMITMENT_DETECT_MAX];
    size_t count;
} hu_commitment_detect_result_t;

/* Detect commitments from text. role = "user" or "assistant". */
hu_error_t hu_commitment_detect(hu_allocator_t *alloc, const char *text, size_t text_len,
                               const char *role, size_t role_len,
                               hu_commitment_detect_result_t *result);

void hu_commitment_deinit(hu_commitment_t *c, hu_allocator_t *alloc);
void hu_commitment_detect_result_deinit(hu_commitment_detect_result_t *r, hu_allocator_t *alloc);

#endif /* HU_AGENT_COMMITMENT_H */
