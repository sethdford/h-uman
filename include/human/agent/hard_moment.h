#ifndef HU_AGENT_HARD_MOMENT_H
#define HU_AGENT_HARD_MOMENT_H

/*
 * Hard-moment note: when an inbound message reads as distressed or low, add a
 * short in-voice note to the persona prompt so the reply meets the moment
 * instead of answering it like any other text.
 *
 * Why (2026-09-24): replies were "cold in hard moments". Distress detection
 * (hu_affect_is_distress) and the retry path's sentiment note never ran on the
 * streaming path, which is what iMessage replies use, and the distress rule
 * needs arousal > 0.5, so low-arousal sadness ("sad", "lonely", "depressed")
 * never registered at all. LOW_MOOD closes that gap without changing the
 * distress predicate other callers rely on.
 *
 * Gated HU_HARD_MOMENT (off|shadow|live, default off) per
 * .claude/rules/feature-gate-requires-measurement.md. Crisis handling is
 * separate (the daemon's crisis directive fires regardless of this gate).
 */

#include "human/core/allocator.h"
#include "human/core/gate_mode.h"
#include <stddef.h>

typedef enum hu_hard_moment_kind {
    HU_HARD_MOMENT_NONE = 0,
    HU_HARD_MOMENT_DISTRESS, /* negative + agitated: stressed, scared, overwhelmed */
    HU_HARD_MOMENT_LOW_MOOD, /* negative + low energy: sad, lonely, hopeless */
} hu_hard_moment_kind_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Classify one inbound message with the behavior-layer affect estimator. */
hu_hard_moment_kind_t hu_hard_moment_classify(const char *msg, size_t msg_len);

/* The in-voice note for a kind; NULL for HU_HARD_MOMENT_NONE. Static string. */
const char *hu_hard_moment_note(hu_hard_moment_kind_t kind);

/* Stable name for logs ("none", "distress", "low_mood"). */
const char *hu_hard_moment_kind_name(hu_hard_moment_kind_t kind);

/* Gate mode from HU_HARD_MOMENT; default OFF. */
hu_gate_mode_t hu_hard_moment_mode(void);

/* Apply the gate to a persona prompt:
 *   OFF    — no classification, prompt untouched, returns NONE.
 *   SHADOW — classifies and logs the kind + message length (never the text);
 *            prompt untouched.
 *   LIVE   — classifies and, for a hard moment, appends the note to *prompt
 *            (reallocated; *prompt_len updated). On OOM the prompt is left
 *            unchanged.
 * Returns the classified kind (NONE when OFF). */
hu_hard_moment_kind_t hu_hard_moment_apply(hu_allocator_t *alloc, hu_gate_mode_t mode,
                                           const char *msg, size_t msg_len, char **prompt,
                                           size_t *prompt_len);

#ifdef __cplusplus
}
#endif

#endif /* HU_AGENT_HARD_MOMENT_H */
