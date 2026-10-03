#ifndef HU_DAEMON_EXPRESSIVE_H
#define HU_DAEMON_EXPRESSIVE_H

/* Deterministic guards around the director's expressive choices
 * (docs/superpowers/specs/2026-09-28-expressive-imessage-design.md): the
 * scene director proposes an effect, a GIF, a threaded reply; these decide
 * whether it is appropriate right now. Pure: facts in, verdict out. */

#include "human/channel.h"
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

/* Who decides a song/video share this turn: the director's share, only when
 * the director is LIVE (then nothing else shares), else today's dice
 * (dice_hit). Never when the inbound is somber (hu_expressive_somber).
 * kind_out is the director's kind, or NONE for a dice share (the medium is
 * picked as today). Saved links are not shared here. */
bool hu_expressive_share_should_go(const hu_director_result_t *director, bool forms_live,
                                   bool dice_hit, bool somber, hu_share_kind_t *kind_out);

/* The director result to hand the sharer, or NULL: only a valid SHARE choice,
 * only with HU_DIRECTOR_FORMS LIVE, only past the guards (not somber, not a
 * group, once per contact per day — remembered for this process). Marks the
 * contact when it lets a share through. */
const hu_director_result_t *hu_expressive_share_gate(const hu_director_result_t *d, bool valid,
                                                     bool forms_live, const char *inbound,
                                                     size_t inbound_len, bool is_group,
                                                     const char *key, size_t key_len, int64_t now);

/* Self-test commands from Seth's own number: "#voice", "#share <song|video|short|
 * saved> [words]", "#effect <id> [text]", "#tapback <love|like|laugh|emphasize|
 * question|dislike>", "#gif [words]", "#text [words]" (a normal text reply).
 * Only at the start of the message.
 * consumed = bytes of the command (and its space) to strip before the turn. */
typedef struct {
    hu_director_form_t form;
    hu_share_kind_t share;
    hu_reaction_type_t reaction;
    char effect[16];
    char query[96];
    size_t consumed;
} hu_selftest_t;

bool hu_selftest_parse(const char *text, size_t len, hu_selftest_t *out);

/* The command becomes the director's choice for the turn. */
void hu_expressive_selftest_apply(const hu_selftest_t *t, hu_director_result_t *d);

struct hu_persona;
/* A #command from one of Seth's own numbers: answered at any hour. */
bool hu_selftest_from_owner(const struct hu_persona *p, const char *key, size_t key_len,
                            const char *text, size_t len);

/* The director's effect, LIVE only, past hu_expressive_effect_allowed and a
 * once-a-week-per-contact budget (remembered for this process). On true,
 * effect_out holds the id to mark the reply with. */
bool hu_expressive_effect_gate(const hu_director_result_t *d, bool valid, bool forms_live,
                               const char *inbound, size_t inbound_len, bool is_group,
                               const char *key, size_t key_len, int64_t now, char *effect_out,
                               size_t cap);

/* Unknown-event guard (2026-09-30). True when `msg` asks how an event in
 * Seth's life went, whether he went to one, or how someone is doing ("how'd
 * the big meeting go", "did you ever go to that concert", "how's ryan
 * settling in") and no recent `history` entry mentions it; `topic` gets the
 * event ("big meeting"). Generic time periods ("your day", "the weekend") and
 * pronouns ("how'd it go") are not events. Pure. */
bool hu_expressive_unknown_event(const char *msg, size_t msg_len,
                                 const hu_channel_history_entry_t *history, size_t history_count,
                                 char *topic, size_t topic_cap);

/* The director direction that replaces an invented outcome: don't say how it
 * went, ask which one or say not sure yet. */
void hu_expressive_unknown_event_direction(const char *topic, char *out, size_t cap);

/* The daemon's call: under HU_UNKNOWN_EVENT_GUARD=off|shadow|live (default
 * off), log a hit and, when live, replace `d`'s direction. The gate is the
 * measurement contract: live only after the same-question probe shows the
 * invented outcomes gone and known events still answered. */
void hu_expressive_unknown_event_guard(hu_director_result_t *d, const char *msg, size_t msg_len,
                                       const hu_channel_history_entry_t *history,
                                       size_t history_count);

#endif
