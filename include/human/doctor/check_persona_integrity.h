/* include/human/doctor/check_persona_integrity.h
 *
 * Doctor check: the live persona file has not silently lost authored keys.
 *
 * On 2026-09-06 06:01 the live `~/.human/personas/seth.json` was rewritten
 * from 24 top-level keys to 15 — contacts, proactive, life_events, style_rules
 * and five more gone — and the daemon ran for two hours with zero contacts and
 * the proactive master switch parsed false. Doctor reported green the whole
 * time; the loss was found by hand. This check compares the live file against
 * its newest sibling backup (`<name>.json.*`) and FAILS when the live file has
 * lost the contacts block, or ≥ HU_DOCTOR_PERSONA_MAX_LOST_KEYS top-level keys,
 * that a backup still carries.
 *
 * ctx contract: same shape as check_prompt_budget — a borrowed `cfg` (the
 * persona name comes from cfg->agent.persona) plus an optional `persona_name`
 * override that wins when non-NULL, so tests never construct a hu_config.
 * NULL cfg and NULL override → NA. */
#ifndef HU_DOCTOR_CHECK_PERSONA_INTEGRITY_H
#define HU_DOCTOR_CHECK_PERSONA_INTEGRITY_H

#include "human/core/error.h"
#include "human/doctor/check.h"

struct hu_config;

/* Losing this many top-level keys (or more) versus the best backup is a
 * regression, not an edit. A deliberate hand edit removes one or two. */
#define HU_DOCTOR_PERSONA_MAX_LOST_KEYS 4

typedef struct hu_doctor_check_persona_integrity_ctx {
    const struct hu_config *cfg;
    const char *persona_name; /* optional override; wins when non-NULL */
} hu_doctor_check_persona_integrity_ctx_t;

extern hu_doctor_check_t hu_doctor_check_persona_integrity;

#endif /* HU_DOCTOR_CHECK_PERSONA_INTEGRITY_H */
