#ifndef HU_PERSONA_DELTAS_H
#define HU_PERSONA_DELTAS_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory/memory.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* W5 — Agent-writable persona deltas + evolver.
 *
 * The agent observes evidence about the user (e.g. "they consistently prefer
 * a shorter response style on slack", "they're using more formal greetings
 * lately") and proposes deltas. Deltas accumulate in a per-contact table and
 * the evolver applies stable ones during AutoDream cycles, with safety rails:
 *
 *   - Each delta is bounded by kind/key (no arbitrary persona overwrites)
 *   - confidence < 0.5  -> dropped
 *   - confidence < 0.75 -> needs at least 3 corroborating proposals
 *   - confidence >= 0.75 with stable evidence -> applied
 *   - Adversarial floods (>10 proposals/hour) are rate-limited
 *
 * The evolver reports every applied/dropped delta so the channel layer can
 * surface "I noticed you tend to ... is that right?" prompts. */

typedef enum hu_persona_delta_kind {
    HU_PERSONA_DELTA_TONE = 0,         /* tone adjustment, e.g. "warmer", "more formal" */
    HU_PERSONA_DELTA_LENGTH = 1,       /* preferred avg length for a channel */
    HU_PERSONA_DELTA_VOCAB_ADD = 2,    /* token to add to preferred vocabulary */
    HU_PERSONA_DELTA_VOCAB_AVOID = 3,  /* token to add to avoid list */
    HU_PERSONA_DELTA_VALUE = 4,        /* value tag, e.g. "honesty", "discretion" */
    HU_PERSONA_DELTA_INTEREST = 5,     /* interest topic */
    HU_PERSONA_DELTA_BOUNDARY = 6,     /* "do not discuss X" */
    HU_PERSONA_DELTA_FORMALITY = 7,    /* per-channel formality override */
    HU_PERSONA_DELTA_MAX
} hu_persona_delta_kind_t;

typedef enum hu_persona_delta_status {
    HU_DELTA_STATUS_PENDING = 0,
    HU_DELTA_STATUS_APPLIED = 1,
    HU_DELTA_STATUS_DROPPED = 2,
    HU_DELTA_STATUS_QUARANTINED = 3,
} hu_persona_delta_status_t;

typedef struct hu_persona_delta {
    int64_t id;
    hu_persona_delta_kind_t kind;
    char key[64];      /* e.g. channel name "slack", or empty */
    char value[160];   /* the proposed new value/string */
    float confidence;  /* 0..1 */
    int64_t proposed_at_ms;
    char source[64];   /* e.g. "telegram", "user-explicit", "agent-inference" */
    hu_persona_delta_status_t status;
    char status_reason[120];
} hu_persona_delta_t;

typedef struct hu_persona_evolver_config {
    int64_t now_ms;            /* 0 = OS clock */
    float apply_threshold;     /* default 0.75 */
    float drop_threshold;      /* default 0.50 */
    size_t corroboration_min;  /* default 3 */
    size_t rate_limit_per_hour;/* default 10 */
    size_t max_apply;          /* default 32 */
} hu_persona_evolver_config_t;

typedef struct hu_persona_evolver_report {
    size_t proposed_total;
    size_t applied;
    size_t dropped;
    size_t quarantined;
    size_t still_pending;
} hu_persona_evolver_report_t;

hu_persona_evolver_config_t hu_persona_evolver_default_config(void);

hu_error_t hu_persona_delta_propose(hu_graph_t *graph, const char *contact_id,
                                    size_t contact_id_len, hu_persona_delta_kind_t kind,
                                    const char *key, const char *value, float confidence,
                                    const char *source, int64_t proposed_at_ms,
                                    int64_t *out_delta_id);

/* Same insert as `hu_persona_delta_propose` using `hu_memory_facade_sqlite_db`. */
hu_error_t hu_persona_delta_propose_facade(hu_memory_facade_t *m, const char *contact_id,
                                           size_t contact_id_len, hu_persona_delta_kind_t kind,
                                           const char *key, const char *value, float confidence,
                                           const char *source, int64_t proposed_at_ms,
                                           int64_t *out_delta_id);

hu_error_t hu_persona_delta_list(hu_graph_t *graph, hu_allocator_t *alloc, const char *contact_id,
                                 size_t contact_id_len, hu_persona_delta_status_t status_filter,
                                 size_t limit, hu_persona_delta_t **out, size_t *out_count);

hu_error_t hu_persona_delta_list_facade(hu_memory_facade_t *m, hu_allocator_t *alloc,
                                         const char *contact_id, size_t contact_id_len,
                                         hu_persona_delta_status_t status_filter, size_t limit,
                                         hu_persona_delta_t **out, size_t *out_count);

void hu_persona_delta_free(hu_allocator_t *alloc, hu_persona_delta_t *deltas, size_t count);

hu_error_t hu_persona_evolver_run(hu_graph_t *graph, const char *contact_id,
                                  size_t contact_id_len,
                                  const hu_persona_evolver_config_t *cfg,
                                  hu_persona_evolver_report_t *out_report);

/* Same logic as `hu_persona_evolver_run` via `hu_memory_facade_sqlite_db`. */
hu_error_t hu_persona_evolver_run_facade(hu_memory_facade_t *m, const char *contact_id,
                                         size_t contact_id_len,
                                         const hu_persona_evolver_config_t *cfg,
                                         hu_persona_evolver_report_t *out_report);

#endif /* HU_PERSONA_DELTAS_H */
