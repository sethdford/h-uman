#ifndef HU_PERSONA_TERSENESS_H
#define HU_PERSONA_TERSENESS_H

/*
 * Terseness calibration gate — OFF → SHADOW → LIVE.
 *
 * The blind-A/B measurement (scripts/blind_ab) showed h-uman reads as
 * DISTINGUISHABLE from the real person because it is too polished / verbose /
 * endearing vs the person's actual terseness (huuman_endearing ~5.7 vs ~4.0).
 * This gate injects a terseness directive into the persona system prompt that
 * suppresses the over-eager warmth + chirpy sign-offs driving that gap.
 *
 * Per .claude/rules/feature-gate-requires-measurement.md this ships OFF by
 * default and is promoted toward LIVE only by the blind A/B — never by a green
 * test suite. The activation site (hu_persona_build_prompt) carries the gate
 * comment naming the measurement.
 */

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HU_TERSE_OFF = 0,    /* no behavior change, zero prompt cost */
    HU_TERSE_SHADOW = 1, /* log what it WOULD inject; prompt unchanged */
    HU_TERSE_LIVE = 2    /* inject the terseness directive into the prompt */
} hu_terse_mode_t;

/* Pure parse of the gate from raw env inputs (testable without getenv).
 * Precedence LIVE > SHADOW > OFF:
 *   - hu_terseness == "live"/"2"/"on", OR live_env_set  -> LIVE
 *   - hu_terseness == "shadow"/"1",     OR shadow_env_set -> SHADOW
 *   - otherwise (NULL / "off" / "0" / unknown)            -> OFF
 */
hu_terse_mode_t hu_terse_mode_parse(const char *hu_terseness, bool live_env_set,
                                    bool shadow_env_set);

/* Thin wrapper: reads HU_TERSENESS / HU_TERSENESS_LIVE / HU_TERSENESS_SHADOW. */
hu_terse_mode_t hu_terse_mode_from_env(void);

/* The terseness directive appended to the persona prompt in LIVE mode.
 * Plain text (no markdown). Stable pointer; never NULL. */
const char *hu_terse_directive(void);

#ifdef __cplusplus
}
#endif

#endif /* HU_PERSONA_TERSENESS_H */
