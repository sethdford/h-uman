#ifndef HU_BEHAVIOR_PRESSURE_H
#define HU_BEHAVIOR_PRESSURE_H

#include "human/behavior/trust.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* B-pressure: heuristic detection of user pressure signals from a single
 * message. Pure function. The output fields plug straight into
 * `hu_trust_input_t` so the trust policy can react.
 *
 * Detected signals (text-only; voice prosody hooks land in B12):
 *
 *   - `user_invoked_authority`     "everyone knows", "you said", "you should
 *                                   know", "as I told you", "you literally"
 *   - `user_emotional_pressure`    anger words, exclamations, all-caps shouting
 *                                   (caller decides a threshold for acting on it)
 *   - `user_reasserted`            same-message reassertion phrasing — "I told
 *                                   you", "again,", "still", "no, …", "you keep
 *                                   forgetting" — cumulative across turns is a
 *                                   follow-up.
 *
 * The detector deliberately errs toward UNDER-detection: false positives push
 * the assistant into uncertainty disclosure, which is acceptable; never into
 * agreement under pressure.
 */

typedef struct hu_pressure_signals {
    bool invoked_authority;
    bool emotional_pressure;
    bool reasserted_in_message;
    uint16_t exclamation_count;
    uint16_t caps_run_max;        /* longest run of consecutive uppercase */
    uint16_t hedging_phrases;     /* "I think", "maybe" — softens emotional read */
} hu_pressure_signals_t;

/* Detect pressure signals from a user message. NULL or empty input returns
 * a zeroed result. Always returns HU_OK (pure function). */
hu_error_t hu_pressure_detect(const char *user_message, size_t user_message_len,
                              hu_pressure_signals_t *out);

/* Convenience: copy detected signals into an `hu_trust_input_t`. Other fields
 * are left untouched so callers can preserve memory/tool evidence they have
 * gathered separately. */
void hu_pressure_apply_to_trust_input(const hu_pressure_signals_t *p,
                                      hu_trust_input_t *trust_in);

#endif /* HU_BEHAVIOR_PRESSURE_H */
