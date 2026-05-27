/* include/human/doctor/check_reflection_loop.h
 *
 * T12 of docs/plans/2026-05-26-reflection-loop. Reports the
 * operator-facing state of the M2 reflection loop:
 *
 *   - Is `reflection.enabled` in config.json?
 *   - If yes, has a run actually completed yet?
 *   - If runs exist, what's the health (recent OK vs recent errors)?
 *
 * Verdict semantics:
 *   PASS — enabled AND ≥1 'ok' run in the last 7 days
 *   FAIL — enabled AND last 5 runs are ALL non-ok (provider_error /
 *          schema_invalid / abandoned). Subsystem is broken.
 *   NA   — every other case, with the reason string distinguishing:
 *            (a) "disabled in config" (opt-out is fine)
 *            (b) "enabled but no runs yet" (cold start — first run
 *                fires when min_interval + idle threshold hit)
 *            (c) "no config provided" (doctor called without cfg)
 *
 * ctx contract: `const struct hu_config *` via hu_doctor_check_reflection_loop_ctx_t.
 * NULL ctx → NA "no config given to doctor".
 *
 * The check does NOT need a live SQLite handle — it reads the
 * persisted reflection_runs rows via the operator-config-supplied
 * memory backend. For Phase 1, when SQLite isn't available, the
 * check returns NA. */
#ifndef HU_DOCTOR_CHECK_REFLECTION_LOOP_H
#define HU_DOCTOR_CHECK_REFLECTION_LOOP_H

#include "human/core/error.h"
#include "human/doctor/check.h"

struct hu_config;
struct sqlite3;

typedef struct hu_doctor_check_reflection_loop_ctx {
    const struct hu_config *cfg;
    /* db is OPTIONAL — when NULL the check returns NA "no db available
     * to read run history". When non-NULL the check inspects the
     * reflection_runs table for terminal status counts. */
    struct sqlite3 *db;
} hu_doctor_check_reflection_loop_ctx_t;

/* Public vtable — registered by registry.c::register_defaults. */
extern hu_doctor_check_t hu_doctor_check_reflection_loop;

#endif /* HU_DOCTOR_CHECK_REFLECTION_LOOP_H */
