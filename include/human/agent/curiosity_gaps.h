#ifndef HU_AGENT_CURIOSITY_GAPS_H
#define HU_AGENT_CURIOSITY_GAPS_H

/* Gap-driven curiosity: what the twin has heard nothing recent about for a
 * contact, so it can ask the way a friend would ("how's work been?").
 *
 * The topics are about someone's current state, never facts a close person is
 * expected to know (where they live, their kids' names): asking a sister
 * "where do you live?" because the notes happen not to say would be worse
 * than asking nothing.
 *
 * Gate HU_CURIOSITY_GAPS (off | shadow | live, default off). SHADOW logs the
 * topic it would offer; LIVE adds one line to the insight block. Activation
 * gated on a SHADOW week (offer rate per turn) and then a LIVE measurement of
 * the twin's question rate against Seth's own (17% of his texts, 30 days to
 * 2026-10-01): do not flip to default-ON without it. */

#include "human/core/gate_mode.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum hu_curiosity_topic {
    HU_CURIOSITY_NONE = 0,
    HU_CURIOSITY_PLANS,     /* what they have coming up */
    HU_CURIOSITY_WORK,      /* work or school */
    HU_CURIOSITY_PEOPLE,    /* the people in their life */
    HU_CURIOSITY_INTERESTS, /* what they've been into */
} hu_curiosity_topic_t;

/* Notes newer than this count as recent. */
#define HU_CURIOSITY_RECENT_DAYS 30
/* At most one offer per contact in this many hours (per daemon process). */
#define HU_CURIOSITY_COOLDOWN_HOURS 72

/* The first topic, in the order above, that no line of `recent` (newline-
 * separated notes about the contact from the last HU_CURIOSITY_RECENT_DAYS)
 * touches. NONE when every topic is covered. Whole-word matching. Pure. */
hu_curiosity_topic_t hu_curiosity_gap_pick(const char *recent, size_t len);

/* Prompt line for a topic ("- nothing recent on their work; ..."), or NULL. */
const char *hu_curiosity_gap_line(hu_curiosity_topic_t t);

/* Whether to offer a gap this turn: never when they asked us something
 * (answer first), and once per contact per HU_CURIOSITY_COOLDOWN_HOURS.
 * Consumes the contact's budget when it returns true. */
bool hu_curiosity_gap_offer_now(const char *contact, size_t contact_len, const char *inbound,
                                size_t inbound_len, int64_t now_s);

hu_gate_mode_t hu_curiosity_gaps_mode(void);

#ifdef HU_IS_TEST
void hu_curiosity_gaps_reset_for_test(void);
#endif

#endif /* HU_AGENT_CURIOSITY_GAPS_H */
