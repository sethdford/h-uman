#ifndef HU_BEHAVIOR_PROSOCIAL_MOMENT_H
#define HU_BEHAVIOR_PROSOCIAL_MOMENT_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Prosocial moments (B2/B4/B5) — the warm-response cluster beyond celebrating a
 * win (B1). One faculty, several opportunity KINDS, same proven shape as
 * win_detect/celebration:
 *
 *   ENCOURAGE (B2) — the user is working toward something / having a hard time
 *   AFFIRM    (B4) — the user showed real effort or character worth naming
 *   SAVOR     (B5) — the user shared a good moment to linger on
 *   GRATITUDE (B5) — the user expressed appreciation; receive it honestly
 *
 * Pure, conservative detection (behavior bounded context). The response is
 * always built through B0 (hu_prosocial_gate), so warmth stays honest and never
 * reinforces a dependency pattern. Crisis/distress is NOT handled here — that's
 * superhuman_emotional's job; this is the everyday-warmth surface.
 *
 * Spec: docs/plans/2026-05-29-prosocial-uplift/
 */

typedef enum hu_pmoment_kind {
    HU_PMOMENT_NONE = 0,
    HU_PMOMENT_ENCOURAGE,
    HU_PMOMENT_AFFIRM,
    HU_PMOMENT_SAVOR,
    HU_PMOMENT_GRATITUDE
} hu_pmoment_kind_t;

typedef struct hu_pmoment {
    bool present;
    hu_pmoment_kind_t kind;
    double confidence; /* [0,1] */
} hu_pmoment_t;

/* Detect a prosocial moment in a user message. Pure; NULL/empty -> none.
 * Conservative + first-match in a sensible precedence (gratitude, affirm,
 * savor, encourage). A crisis/setback negator suppresses the upbeat kinds. */
hu_pmoment_t hu_pmoment_detect(const char *msg, size_t len);

#endif /* HU_BEHAVIOR_PROSOCIAL_MOMENT_H */
