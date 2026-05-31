#ifndef HU_AGENT_INTENT_H
#define HU_AGENT_INTENT_H

/* Intent-aware response-type classifier (Tier B #1 from the voiceai port-map,
 * docs/research/2026-05-31-voiceai-speech-behavior-port-map.md).
 *
 * Classifies an inbound message into ONE conversational intent + a confidence,
 * then (when confident) builds a terse directive that steers the reply strategy
 * — listen vs advise vs validate vs keep-it-short. Wired as a new directive
 * source in agent_turn.c, gated by env HU_INTENT_DIRECTIVE (off|shadow|on,
 * default off). Activation OFF->SHADOW->ON is gated on the blind A/B.
 *
 * Deterministic: keyword (word-boundary) + message-shape scoring, NO LLM call,
 * no allocation in analyze. Keyword matching uses hu_str_contains_word_ci, never
 * substring — see ~/.claude/rules/substring-classifier-pitfalls.md ("informal"
 * must not match "formal"; "won" must not match "wonder").
 */

#include "human/core/allocator.h"
#include "human/core/error.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum hu_intent {
    HU_INTENT_PROCESSING_ALOUD = 0, /* default: nothing dominates; reflect back   */
    HU_INTENT_LOGISTICS,            /* scheduling / yes-no — answer short          */
    HU_INTENT_SEEKING_ADVICE,       /* explicit ask — brief take                   */
    HU_INTENT_NEEDS_TO_BE_HEARD,    /* long + emotional — listen, don't fix        */
    HU_INTENT_JUST_VENTING,         /* frustration — validate, don't redirect      */
    HU_INTENT_VULNERABLE_SHARE,     /* disclosure — hold space, no advice          */
    HU_INTENT_GOOD_NEWS,            /* celebration — match the energy              */
    HU_INTENT_SMALL_TALK,           /* light social — warm but brief               */
    HU_INTENT_COUNT
} hu_intent_t;

typedef struct hu_intent_analysis {
    hu_intent_t intent;
    double confidence;       /* 0..1, = min(1, winning score)                      */
    double emotional_weight; /* 0..1, feeds arbitrator directive priority          */
} hu_intent_analysis_t;

/* Deterministic classification. No allocation, no LLM. Safe on NULL/empty msg
 * (yields PROCESSING_ALOUD, confidence 0). `out` must be non-NULL. */
void hu_intent_analyze(const char *msg, size_t len, hu_intent_analysis_t *out);

/* Build the terse reply-strategy directive for `a`. On success sets *dir (NUL-
 * terminated, allocator-owned, free with alloc->free(ctx, *dir, *dir_len + 1))
 * and *dir_len. When confidence is below the injection threshold, OR the intent
 * is the PROCESSING_ALOUD default, sets *dir=NULL / *dir_len=0 and returns HU_OK
 * (caller injects nothing). Returns HU_ERR_OUT_OF_MEMORY on allocation failure. */
hu_error_t hu_intent_build_directive(const hu_allocator_t *alloc, const hu_intent_analysis_t *a,
                                     char **dir, size_t *dir_len);

/* Stable lowercase name for logging / shadow mode (e.g. "logistics"). */
const char *hu_intent_name(hu_intent_t intent);

/* Injection threshold: below this confidence, build_directive emits nothing.
 * Keeps the (already ~16 KB) system prompt from growing on low-signal turns. */
#define HU_INTENT_CONFIDENCE_THRESHOLD 0.45

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_INTENT_H */
