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
 * Gate HU_CURIOSITY_GAPS (off | shadow | live, default off). The line rides
 * the insight block, so it needs HU_INSIGHT_STREAM=live, and only the load for
 * the contact's own message offers it (hu_memory_loader_set_offer_gap), never
 * the tool-loop loads. SHADOW logs every turn a gap would be offered without
 * spending the cooldown; LIVE adds one line and spends it. Activation gated on
 * a SHADOW week (offer rate per turn) and then a LIVE measurement of the
 * twin's question rate against Seth's own (17% of his texts, 30 days to
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

/* The first topic after `after` (cyclic, in the order above; NONE starts at
 * PLANS) that no line of `recent` touches — `recent` being newline-separated
 * notes about the contact from the last HU_CURIOSITY_RECENT_DAYS. NONE when
 * every topic is covered. Whole-word matching. Pure. */
hu_curiosity_topic_t hu_curiosity_gap_pick(const char *recent, size_t len,
                                           hu_curiosity_topic_t after);

/* Prompt line for a topic ("- nothing recent on their work; ..."), or NULL. */
const char *hu_curiosity_gap_line(hu_curiosity_topic_t t);

/* The topic to offer this contact now, or NONE: never when they asked us
 * something (answer first), nothing within HU_CURIOSITY_COOLDOWN_HOURS of the
 * last offer, and rotating past the topic offered last. `commit` records the
 * offer (LIVE); without it nothing changes (SHADOW). */
hu_curiosity_topic_t hu_curiosity_gap_offer(const char *contact, size_t contact_len,
                                            const char *inbound, size_t inbound_len,
                                            const char *recent, size_t recent_len, int64_t now_s,
                                            bool commit);

hu_gate_mode_t hu_curiosity_gaps_mode(void);

#ifdef HU_IS_TEST
void hu_curiosity_gaps_reset_for_test(void);
#endif

#endif /* HU_AGENT_CURIOSITY_GAPS_H */
