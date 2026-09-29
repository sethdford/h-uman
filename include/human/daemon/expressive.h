#ifndef HU_DAEMON_EXPRESSIVE_H
#define HU_DAEMON_EXPRESSIVE_H

/* Deterministic guards around the director's expressive choices
 * (docs/superpowers/specs/2026-09-28-expressive-imessage-design.md): the
 * scene director proposes an effect, a GIF, a threaded reply; these decide
 * whether it is appropriate right now. Pure: facts in, verdict out. */

#include "human/daemon/director.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Grief, illness, a breakup: no flourish is ever right here. */
bool hu_expressive_somber(const char *text, size_t len);

/* One of the effect ids imsg 0.15.9 documents; at most one per contact per
 * 7 days (secs_since_last: -1 = never); never somber, never in groups. */
bool hu_expressive_effect_allowed(const char *effect, bool somber, bool is_group,
                                  int64_t secs_since_last);

/* At most one GIF per contact per day, for close casual relationships only;
 * never somber, never in groups. */
bool hu_expressive_gif_allowed(bool somber, bool is_group, const char *relationship,
                               int64_t secs_since_last);

/* One line for the director: which forms are possible this turn. Returns the
 * length written, 0 if it did not fit. */
size_t hu_expressive_situation(char *buf, size_t cap, bool voice_available, bool bridge_up,
                               bool is_group);

/* One shadow log line: the director's form, and each flourish with the guards'
 * verdict ("effect=confetti(blocked)"). Length written, 0 if it did not fit. */
size_t hu_expressive_shadow_line(const hu_director_result_t *r, const char *inbound,
                                 size_t inbound_len, bool is_group, const char *relationship,
                                 char *buf, size_t cap);

#endif
