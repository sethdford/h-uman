#ifndef HU_BEHAVIOR_WIN_DETECT_H
#define HU_BEHAVIOR_WIN_DETECT_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Win detection (B1a) — notice when the user shares something good worth
 * celebrating: an achievement, a milestone, or plain good news.
 *
 * Pure predicate (behavior bounded context). Deliberately CONSERVATIVE:
 * celebrating a non-win (or, worse, a setback) is far more damaging than
 * missing a real win, so detection is negation-guarded and requires an
 * explicit positive cue. The celebration itself is gated again by B0
 * (prosocial integrity) before anything is surfaced.
 *
 * Spec: docs/plans/2026-05-29-prosocial-uplift/
 */

typedef enum hu_win_kind {
    HU_WIN_NONE = 0,
    HU_WIN_ACHIEVEMENT, /* finished/shipped/passed/landed/got the job */
    HU_WIN_MILESTONE,   /* anniversary / N years / first time / big number */
    HU_WIN_GOOD_NEWS    /* "great news", "good news", "excited to share" */
} hu_win_kind_t;

typedef struct hu_win_signal {
    bool is_win;
    hu_win_kind_t kind;
    double confidence; /* [0,1] */
} hu_win_signal_t;

/* Detect a win in a user message. Pure; NULL/empty -> {false, NONE, 0}.
 * A negator near/in the message (didn't, not, failed, couldn't, no longer)
 * suppresses detection — "I didn't get the job" is never a win. */
hu_win_signal_t hu_win_detect(const char *msg, size_t len);

#endif /* HU_BEHAVIOR_WIN_DETECT_H */
