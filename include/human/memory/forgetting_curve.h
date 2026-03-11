#ifndef HU_MEMORY_FORGETTING_CURVE_H
#define HU_MEMORY_FORGETTING_CURVE_H

#include "human/core/error.h"
#include <stdbool.h>
#include <stdint.h>

/* --- F73 Forgetting Curve ---
 * Salience score decay with reinforcement. Emotional anchors resist decay.
 */

/**
 * Compute decayed salience from initial value using exponential forgetting curve.
 *
 * Formula: salience = initial * exp(-decay_rate * days)
 * If is_emotional_anchor: effective_decay = decay_rate * 0.3 (3x slower decay)
 */
double hu_forgetting_decayed_salience(double initial_salience, double decay_rate,
                                      int64_t created_at, int64_t now_ts,
                                      bool is_emotional_anchor);

#ifdef HU_ENABLE_SQLITE
/**
 * Apply batch decay to episodes in the database.
 * Episodes with impact_score > 0.8 use decay_rate * 0.3 (emotional anchor).
 * Requires episodes table with: salience_score, impact_score, created_at.
 */
hu_error_t hu_forgetting_apply_batch_decay(void *db, int64_t now_ts, double rate);
#endif

#endif /* HU_MEMORY_FORGETTING_CURVE_H */
