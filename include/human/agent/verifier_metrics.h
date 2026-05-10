#ifndef HU_AGENT_VERIFIER_METRICS_H
#define HU_AGENT_VERIFIER_METRICS_H

/* Persisted snapshot of the response-verifier counters that live on
 * hu_agent_t (verifier_runs / verifier_claims_total / verifier_claims_flagged).
 * The daemon flushes a snapshot to ~/.human/verifier_metrics.json once a
 * minute so `human doctor verifier` can show the last known hallucination
 * rate even when the daemon is offline.
 *
 * The model is intentionally tiny: the verifier is a process-lifetime
 * counter, and the file is a write-mostly heartbeat that any future
 * dashboard can scrape. */

#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hu_verifier_metrics {
    uint64_t total_runs;             /* hu_agent_t::verifier_runs */
    uint64_t total_claims_extracted; /* hu_agent_t::verifier_claims_total */
    uint64_t total_claims_flagged;   /* hu_agent_t::verifier_claims_flagged */
    int64_t last_update_epoch;       /* unix seconds, set by save() */
} hu_verifier_metrics_t;

/* Compose ~/.human/verifier_metrics.json into out (cap >= 256 recommended).
 * Returns true on success, false if HOME is unset or out is too small. */
bool hu_verifier_metrics_path(char *out, size_t cap);

/* Read the metrics file. Returns HU_ERR_NOT_FOUND if the file does not exist
 * (callers should treat this as "no data yet" and report zeroed counts). */
hu_error_t hu_verifier_metrics_load(hu_verifier_metrics_t *out);

/* Write the metrics file atomically-ish: ~/.human is mkdir'd if missing,
 * then the file is overwritten in place. Sets metrics->last_update_epoch to
 * the current time before serialising. */
hu_error_t hu_verifier_metrics_save(hu_verifier_metrics_t *metrics);

/* Compute a flagged-rate as flagged/extracted in the closed interval [0,1].
 * Returns 0.0 when extracted == 0. Pure helper used by doctor + tests. */
double hu_verifier_metrics_flagged_rate(const hu_verifier_metrics_t *m);

#endif /* HU_AGENT_VERIFIER_METRICS_H */
