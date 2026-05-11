#ifndef HU_INTELLIGENCE_TRUST_H
#define HU_INTELLIGENCE_TRUST_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Per-contact trust state tracking */
typedef struct hu_trust_state {
    double trust_level;        /* 0.0-1.0, starts at 0.5 (neutral) */
    double erosion_rate;       /* accumulated erosion velocity */
    int64_t last_error_at;     /* timestamp of last error/correction */
    uint32_t corrections_count;/* total corrections received */
    uint32_t fabrications_count;/* detected fabrications */
    uint32_t accurate_recalls; /* verified accurate memory recalls */
    uint32_t helpful_actions;  /* positive outcomes */
    int64_t last_updated_at;   /* timestamp of last state change */
} hu_trust_state_t;

/* Trust modification events */
typedef enum hu_trust_event {
    HU_TRUST_ACCURATE_RECALL = 0,  /* verified memory claim */
    HU_TRUST_HELPFUL_ACTION,       /* positive user feedback */
    HU_TRUST_CORRECTION,           /* user corrected the agent */
    HU_TRUST_FABRICATION,          /* detected fabrication/hallucination */
    HU_TRUST_ERROR,                /* factual error */
    HU_TRUST_VERIFIED_CLAIM,       /* claim cross-referenced and confirmed */
} hu_trust_event_t;

/* Uncertainty language levels (7 gradations) */
typedef enum hu_uncertainty_level {
    HU_UNCERTAINTY_CERTAIN = 0,    /* "I know..." */
    HU_UNCERTAINTY_CONFIDENT,      /* "I'm confident that..." */
    HU_UNCERTAINTY_FAIRLY_SURE,    /* "I'm fairly sure..." */
    HU_UNCERTAINTY_THINK,          /* "I think..." */
    HU_UNCERTAINTY_BELIEVE,        /* "I believe..." */
    HU_UNCERTAINTY_NOT_SURE,       /* "I'm not sure, but..." */
    HU_UNCERTAINTY_SPECULATING,    /* "I may be wrong, but..." */
} hu_uncertainty_level_t;

#define HU_TRUST_DEFAULT_LEVEL 0.5
#define HU_TRUST_EROSION_THRESHOLD 0.3

/* Initialize a trust state to neutral defaults */
void hu_trust_init(hu_trust_state_t *state);

/* Update trust based on an event. Modifies state in place.
 * now_ts: current unix timestamp. */
hu_error_t hu_trust_update(hu_trust_state_t *state, hu_trust_event_t event, int64_t now_ts);

/* Check if trust has eroded below the alert threshold.
 * Returns true if trust_level < HU_TRUST_EROSION_THRESHOLD. */
bool hu_trust_detect_erosion(const hu_trust_state_t *state);

/* Map an internal confidence (0.0-1.0) to an uncertainty level,
 * adjusted by the current trust state. Low trust shifts toward
 * more uncertain language. */
hu_uncertainty_level_t hu_trust_calibrate_language(const hu_trust_state_t *state,
                                                    double internal_confidence);

/* Get the hedge prefix string for an uncertainty level.
 * Returns a static string like "I think " or "I'm fairly sure ". */
const char *hu_uncertainty_prefix(hu_uncertainty_level_t level);

/* Build a trust-aware context directive string from per-contact trust state.
 * When trust is low, adds hedging instructions.
 * Allocates *out. Caller frees with alloc.
 *
 * Named `hu_trust_state_*` to avoid a C linker collision with
 * `human/behavior/trust.h` (`hu_trust_build_directive` for decisions). */
hu_error_t hu_trust_state_build_directive(hu_allocator_t *alloc, const hu_trust_state_t *state,
                                          char **out, size_t *out_len);

#endif /* HU_INTELLIGENCE_TRUST_H */
