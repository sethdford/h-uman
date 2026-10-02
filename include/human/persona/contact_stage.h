#ifndef HU_PERSONA_CONTACT_STAGE_H
#define HU_PERSONA_CONTACT_STAGE_H

/* Per-contact relationship stage, derived from interaction data (DEF-16).
 *
 * Before: agent->relationship was ONE agent-wide state, loaded from whichever
 * contact spoke first after a restart and incremented on every turn for every
 * contact, so a contact's stage reflected the daemon's uptime, not the
 * relationship. The persisted per-contact rows (frontier_state.rel_stage,
 * 2026-10-02) were 7 NEW / 4 FAMILIAR / 3 TRUSTED, each a snapshot of the
 * shared counter when that contact last spoke.
 *
 * Now the stage of a contact is a function of that contact alone:
 *   volume      vol = 1 - 2^(-n / median_n)              messages exchanged
 *   regularity  reg = 1 - 2^(-active_days / median_days) distinct days talked
 *   reciprocity r   = 2 * min(in, out) / (in + out)
 *   evidence    e   = (vol + reg) / 2 * (0.75 + 0.25 * r)
 * Half-saturation is at the MEDIAN contact of this owner (norms below), so the
 * scale adapts to how much this person texts, not a hand-picked constant.
 * The persona's declared Dunbar layer is a prior whose weight shrinks as
 * evidence accumulates:  q = (1 - w) * prior + w * e,  w = n / (n + median_n).
 * q maps to a stage with hu_relationship_stage_from_quality's bands.
 *
 * Calendar span (first to last message) is deliberately not used: the
 * session store is ~205 days old, so 10 of 14 active contacts span 103-202
 * days and the span does not discriminate; active days does (1-70). */

#include "human/persona/relationship.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct hu_contact_stage_signals {
    uint32_t inbound;     /* messages from the contact */
    uint32_t outbound;    /* messages to the contact */
    uint32_t active_days; /* distinct days with any message */
} hu_contact_stage_signals_t;

/* Owner-level norms: the median contact's volume and active days. */
typedef struct hu_contact_stage_norms {
    double median_msgs;
    double median_days;
} hu_contact_stage_norms_t;

/* Norms used when fewer than HU_CONTACT_STAGE_MIN_NORM_CONTACTS contacts have
 * history (a new install): documented defaults, replaced by the medians as
 * soon as there is data. */
#define HU_CONTACT_STAGE_MIN_NORM_CONTACTS   5u
#define HU_CONTACT_STAGE_DEFAULT_MEDIAN_MSGS 50.0
#define HU_CONTACT_STAGE_DEFAULT_MEDIAN_DAYS 7.0

/* Prior from the persona's declared layer: the midpoint of the stage band the
 * layer maps to. Dunbar names or sizes ("intimate"/"support"/5 -> 0.90,
 * "close"/"sympathy"/15 -> 0.675, "active"/"affinity"/50 -> 0.40,
 * "acquaintance"/"casual"/150 -> 0.125), or the 1-4 layer index. Whole-word,
 * case-insensitive. Returns a negative value when there is no usable prior.
 * `relationship_stage` (a persona field like "intimate") is consulted only
 * when `dunbar_layer` gives nothing. */
float hu_contact_stage_prior(const char *dunbar_layer, const char *relationship_stage);

/* Evidence e in [0, 1] from signals and norms (formula above). */
float hu_contact_stage_evidence(const hu_contact_stage_signals_t *s,
                                const hu_contact_stage_norms_t *norms);

/* The stage: blends prior (ignored when < 0) and evidence, writes q to
 * *quality_out when non-NULL. NULL signals or norms -> HU_REL_NEW. */
hu_relationship_stage_t hu_contact_stage_derive(const hu_contact_stage_signals_t *s,
                                                const hu_contact_stage_norms_t *norms, float prior,
                                                float *quality_out);

#endif /* HU_PERSONA_CONTACT_STAGE_H */
