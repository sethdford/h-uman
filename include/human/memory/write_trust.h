#ifndef HU_MEMORY_WRITE_TRUST_H
#define HU_MEMORY_WRITE_TRUST_H

#include "human/core/error.h"
#include "human/memory/graph.h"
#include <stdbool.h>
#include <stdint.h>

/* W1 — Write-time trust scoring.
 *
 * Every memory candidate (graph relation, fact, episode, etc.) is scored on a
 * 0.0-1.0 trust scale before it is written. Low-trust candidates are diverted
 * into the quarantine_relations table instead of the live graph. AutoDream
 * (W2) reviews quarantine entries during idle cycles and either promotes or
 * deletes them.
 *
 * Trust formula (deterministic, no LLM at write time):
 *
 *   score = w_source   * source_score
 *         + w_recency  * recency_score
 *         + w_consist  * consistency_score
 *         + w_anomaly  * anomaly_score
 *
 * Default weights (sum to 1.0): source 0.40, recency 0.10, consistency 0.30,
 * anomaly 0.20. Tunable via hu_write_trust_set_weights for tests.
 *
 * Sub-scores:
 *   - source_score:    1.00 user-typed, 0.85 trusted channel, 0.50 untrusted
 *                      web/feed, 0.30 unknown.
 *   - recency_score:   exponential decay vs now; 1.0 at t=now, ~0.6 at 24h.
 *   - consistency_score: 1.0 if no contradictions, 0.5 on FLAG, 0.7 on
 *                      SUPERSEDE (older fact may be wrong).
 *   - anomaly_score:   1.0 by default, decreased to 0.0 when the source has
 *                      tripped the rate-limit ring (>= N writes / window).
 *
 * Threshold defaults: live >= 0.6, quarantine 0.3-0.6, drop < 0.3.
 */

typedef enum hu_write_source {
    HU_WRITE_SOURCE_USER = 0,        /* directly typed by the user */
    HU_WRITE_SOURCE_CHANNEL_TRUSTED, /* iMessage, Slack, etc. paired channels */
    HU_WRITE_SOURCE_CHANNEL_OPEN,    /* discord public, anonymous webhook */
    HU_WRITE_SOURCE_FEED_FILE,       /* PDF, markdown, file ingest */
    HU_WRITE_SOURCE_FEED_WEB,        /* RSS / scraped web */
    HU_WRITE_SOURCE_AGENT,           /* extracted by the agent itself (lower) */
    HU_WRITE_SOURCE_UNKNOWN,         /* default */
} hu_write_source_t;

typedef enum hu_write_outcome {
    HU_WRITE_OUTCOME_LIVE,       /* score >= live threshold; commit normally */
    HU_WRITE_OUTCOME_QUARANTINE, /* in quarantine band; divert for review */
    HU_WRITE_OUTCOME_DROP,       /* score below drop floor; reject silently */
} hu_write_outcome_t;

typedef struct hu_write_trust_input {
    hu_write_source_t source;
    int64_t observed_at;       /* unix ms when fact was observed */
    int64_t now;               /* unix ms; for recency decay */
    bool contradiction_flag;   /* set when conflict_resolver returned FLAG */
    bool supersession;         /* set when conflict_resolver returned SUPERSEDE */
    uint32_t recent_writes;    /* count from the source in the rate window */
    uint32_t rate_limit;       /* trip when recent_writes > rate_limit */
} hu_write_trust_input_t;

typedef struct hu_write_trust_decision {
    float score;                 /* 0.0-1.0 */
    hu_write_outcome_t outcome;
    char reason[128];            /* short human-readable label */
} hu_write_trust_decision_t;

/* Pure scorer: no DB access. Always succeeds; never returns HU_ERR. */
hu_write_trust_decision_t hu_write_trust_score(const hu_write_trust_input_t *in);

/* Convenience: source label for logs / quarantine. */
const char *hu_write_source_str(hu_write_source_t s);
const char *hu_write_outcome_str(hu_write_outcome_t o);

/* Insert a relation into quarantine_relations instead of the live graph.
 * The caller passes the same fields it would have given to upsert_ex, plus
 * the trust decision. Returns HU_OK on success, HU_ERR_IO on failure. */
hu_error_t hu_write_trust_quarantine_relation(hu_graph_t *g, const char *contact_id,
                                              size_t contact_id_len, int64_t source_id,
                                              int64_t target_id, hu_relation_type_t type,
                                              float weight, int64_t event_start, int64_t event_end,
                                              float confidence, const char *context,
                                              size_t context_len, const char *provenance,
                                              size_t provenance_len,
                                              const hu_write_trust_decision_t *decision);

/* Counts entries currently in quarantine for a contact. */
hu_error_t hu_write_trust_quarantine_count(hu_graph_t *g, const char *contact_id,
                                           size_t contact_id_len, size_t *out_count);

#endif /* HU_MEMORY_WRITE_TRUST_H */
