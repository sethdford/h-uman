#ifndef HU_CONTEXT_VOICE_TRIGGERS_H
#define HU_CONTEXT_VOICE_TRIGGERS_H

/* Voice triggers v2 (HU_VOICE_TRIGGERS_V2=off|shadow|live, default off).
 *
 * Voice-first memos (hu_voice_intent_decide) fire only on audio, a fixed list
 * of heartfelt phrases, or a question of 8+ words. Production 2026-09-17 to
 * 10-01: 77 of 79 decisions for real contacts were "no_trigger" — memos almost
 * never happened. These are four more moments when a person would rather talk.
 * They are considered only where the base decision is "no_trigger", so audio,
 * logistics and the per-contact spacing rule still come first, and a weekly
 * per-contact cap sits on top.
 *
 * Pure: facts in, reason out. Every threshold is named below. */

#include "human/persona.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* story_inbound: they told a story. Either a long message (a pasted link is not
 * a story), or a shorter one of two or more sentences with a past-tense or
 * feeling word ("we went", "I was so nervous"). */
#define HU_VOICE_V2_STORY_LONG_CHARS         140
#define HU_VOICE_V2_STORY_NARRATIVE_CHARS    80
#define HU_VOICE_V2_STORY_MIN_SENTENCES      2
#define HU_VOICE_V2_STORY_SENTENCE_MIN_WORDS 3

/* memo_length_reply: a close contact asked two or more real questions (each
 * 3+ words, not a short logistics question). Answering several questions is a
 * paragraph, which people talk rather than type. It reads their message, not
 * the planned reply budget: that budget sits at the channel ceiling for any
 * contact whose measured reply p90 reaches it, so a budget rule fired on "ok"
 * (review of #576). */
#define HU_VOICE_V2_MEMO_MIN_QUESTIONS 2

/* late_evening_warmth: 20:00-23:30 in the owner's local time (the machine the
 * daemon runs on), not the contact's; a close contact, and a message of
 * more than a couple of words (not "ok", not "night!"). */
#define HU_VOICE_V2_EVENING_START_MIN (20 * 60)
#define HU_VOICE_V2_EVENING_END_MIN   (23 * 60 + 30)
#define HU_VOICE_V2_EVENING_MIN_WORDS 4

/* long_gap_reconnect: the owner's first reply to a close contact after 3+ days. */
#define HU_VOICE_V2_RECONNECT_GAP_SEC (3 * 86400)

/* At most this many v2-chosen memos per contact per rolling week
 * (HU_VOICE_V2_WEEKLY_CAP overrides; 0 disables the v2 reasons). */
#define HU_VOICE_V2_WEEKLY_CAP_DEFAULT 2u
#define HU_VOICE_V2_WEEK_SEC           (7 * 86400)

typedef struct {
    const char *inbound; /* the inbound batch text */
    size_t inbound_len;
    int local_minute;               /* 0..1439 local time; -1 = unknown */
    bool close_contact;             /* hu_voice_v2_close_contact */
    int64_t secs_since_owner_reply; /* to this contact; -1 = unknown */
    uint32_t v2_memos_this_week;    /* v2 VOICE decisions to this contact */
    uint32_t weekly_cap;            /* hu_voice_v2_parse_weekly_cap */
} hu_voice_v2_facts_t;

bool hu_voice_v2_story_inbound(const char *s, size_t n);
bool hu_voice_v2_memo_length_reply(bool close_contact, const char *s, size_t n);
bool hu_voice_v2_late_evening_warmth(int local_minute, bool close_contact, const char *s, size_t n);
bool hu_voice_v2_long_gap_reconnect(int64_t secs_since_owner_reply, bool close_contact);

/* dunbar_layer "intimate"/"close", or relationship_type "family"/"romantic". */
bool hu_voice_v2_close_contact(const hu_contact_profile_t *cp);

/* HU_VOICE_V2_WEEKLY_CAP: a non-negative integer up to 14, else the default. */
uint32_t hu_voice_v2_parse_weekly_cap(const char *env);

/* The v2 reason that fires before the weekly cap, or NULL. */
const char *hu_voice_v2_trigger(const hu_voice_v2_facts_t *f);

/* Which v2 reason fires, if any, after the weekly cap. Returns true (VOICE)
 * with *out_reason one of "story_inbound", "long_gap_reconnect",
 * "late_evening_warmth", "memo_length_reply"; or false with *out_reason
 * "weekly_cap" or "none". Static strings, never NULL when out_reason is set. */
bool hu_voice_v2_decide(const hu_voice_v2_facts_t *f, const char **out_reason);

#endif
