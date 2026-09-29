#ifndef HU_DAEMON_EXPRESSIVE_H
#define HU_DAEMON_EXPRESSIVE_H

/* Deterministic guards around the director's expressive choices
 * (docs/superpowers/specs/2026-09-28-expressive-imessage-design.md): the
 * scene director proposes an effect, a GIF, a threaded reply; these decide
 * whether it is appropriate right now. Pure: facts in, verdict out. */

#include "human/daemon/director.h"
#include "human/inspiration.h"

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
                               bool is_group, bool saved_link);

/* One shadow log line: the director's form, and each flourish with the guards'
 * verdict ("effect=confetti(blocked)"). Length written, 0 if it did not fit. */
size_t hu_expressive_shadow_line(const hu_director_result_t *r, const char *inbound,
                                 size_t inbound_len, bool is_group, const char *relationship,
                                 char *buf, size_t cap);

/* Phase 5.1: at most one share per contact per day; never somber, never groups. */
bool hu_expressive_share_allowed(bool somber, bool is_group, int64_t secs_since_last);

/* Which sender carries a share: songs via music search, videos and Shorts via
 * YouTube (needs a key), saved links via the share queue (NONE here). */
hu_inspiration_medium_t hu_expressive_share_medium(hu_share_kind_t kind, bool have_youtube_key);

/* Who decides a song/video share this turn: the director's share (LIVE), else
 * nothing when the director is LIVE, else today's dice (dice_hit). kind_out is
 * the director's kind, or NONE for a dice share (the medium is picked as today).
 * Saved links are not shared here. */
bool hu_expressive_share_should_go(const hu_director_result_t *director, bool forms_live,
                                   bool dice_hit, hu_share_kind_t *kind_out);

/* The director result to hand the sharer, or NULL: only a valid SHARE choice,
 * only with HU_DIRECTOR_FORMS LIVE, only past the guards (not somber, not a
 * group, once per contact per day — remembered for this process). Marks the
 * contact when it lets a share through. */
const hu_director_result_t *hu_expressive_share_gate(const hu_director_result_t *d, bool valid,
                                                     bool forms_live, const char *inbound,
                                                     size_t inbound_len, bool is_group,
                                                     const char *key, size_t key_len, int64_t now);

#endif
