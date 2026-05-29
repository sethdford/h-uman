#ifndef HU_INSPIRATION_H
#define HU_INSPIRATION_H

#include "human/channel.h"
#include <stdbool.h>
#include <stddef.h>

/* Which kind of media inspiration to share this turn. */
typedef enum hu_inspiration_medium {
    HU_INSPIRATION_NONE,
    HU_INSPIRATION_MUSIC,
    HU_INSPIRATION_YOUTUBE,
    HU_INSPIRATION_TIKTOK
} hu_inspiration_medium_t;

/* Pick the medium from conversation cues (word-boundary CI match).
 * A youtube cue with youtube_available=false falls back to music.
 * Always returns a concrete medium (defaults to MUSIC).
 * Checks TikTok cues before YouTube before Music (more specific first);
 * youtube_available=false demotes a YouTube match to Music. */
hu_inspiration_medium_t hu_inspiration_pick_medium(const char *incoming, size_t incoming_len,
                                                   bool youtube_available);

/* Static system-prompt instruction for the generation call, per medium.
 * Each asks for "<search intent> | <persona-voiced human line>". */
const char *hu_inspiration_system_prompt(hu_inspiration_medium_t medium);

/* Build a persona voice hint to append to the generation prompt.
 * Either arg may be NULL/empty; returns bytes written (0 if nothing to say). */
size_t hu_inspiration_build_voice_hint(const char *formality, const char *traits_csv, char *out,
                                       size_t cap);

/* Build a guaranteed-valid TikTok discovery URL: https://www.tiktok.com/tag/<kw>
 * Strips a leading '#', URL-encodes non-alnum, drops separators. Returns bytes
 * written or 0 on bad input/overflow. */
size_t hu_tiktok_tag_url(const char *keyword, size_t keyword_len, char *out, size_t cap);

/* Send an inspiration as two bubbles on an unfurling channel: the persona-voiced
 * human line first, an optional human-pacing gap (gap_us microseconds; pass 0 in
 * tests), then the bare URL alone so the platform renders its rich card. The URL
 * bubble body is exactly the URL bytes — never a caption inline, which suppresses
 * the unfurl. casual_msg may be NULL/empty (URL-only then). Validates url via
 * hu_tool_validate_url; returns false (sends nothing) on invalid url or bad args. */
bool hu_inspiration_send_two_bubble(hu_channel_t *channel, const char *target, size_t target_len,
                                    const char *casual_msg, const char *url, unsigned gap_us);

#endif
