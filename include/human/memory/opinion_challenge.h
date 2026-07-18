#ifndef HU_MEMORY_OPINION_CHALLENGE_H
#define HU_MEMORY_OPINION_CHALLENGE_H

#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/gate_mode.h"
#include <stdbool.h>
#include <stddef.h>

/* ── Opinion-challenge detection (roadmap #14: stances persist under pushback).
 * Pure text predicates over (inbound, stored-opinion) strings — no persistence
 * and no hu_opinion_t/hu_evolved_opinion_t dependency, so any opinion store can
 * feed it. Available in every build variant. Implemented in
 * src/memory/opinions.c. ── */

/* True when `inbound` references the stored opinion's topic keywords AND
 * contains a disagreement marker ("nah", "disagree", "wrong", "no way",
 * "really?", "you think?"). Word-boundary matching throughout, so
 * "informal" never matches topic word "formal" and "wrongfully" never
 * fires the "wrong" marker. Precision-first: both halves must match —
 * a false positive makes the persona stubborn about nothing. */
bool hu_opinion_challenge_detect(const char *inbound, size_t inbound_len, const char *topic,
                                 size_t topic_len);

/* Gate-aware directive assembly for the director/context path.
 *   OFF    -> no work, *out NULL, *would_fire false
 *   SHADOW -> runs the detector, sets *would_fire, *out stays NULL
 *   LIVE   -> when the detector fires, allocates the hold-your-position
 *             directive into *out (*out_len bytes, buffer *out_len+1)
 * Caller frees *out via alloc. Returns HU_OK unless allocation fails. */
hu_error_t hu_opinion_challenge_directive(hu_allocator_t *alloc, hu_gate_mode_t mode,
                                          const char *inbound, size_t inbound_len,
                                          const char *topic, size_t topic_len, const char *stance,
                                          size_t stance_len, char **out, size_t *out_len,
                                          bool *would_fire);

#endif /* HU_MEMORY_OPINION_CHALLENGE_H */
