#ifndef HU_INSPIRATION_H
#define HU_INSPIRATION_H

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

#endif
