#ifndef HUMAN_PERSONA_STICKER_H
#define HUMAN_PERSONA_STICKER_H

#include <stdbool.h>
#include <stddef.h>

/* Query specifying the conversational context for sticker selection.
 * Any field may be NULL/empty — picker will broaden the match
 * (e.g. NULL tone matches any tone). */
typedef struct {
    const char *context_tag; /* "casual"|"formal"|"intimate"|"playful" */
    const char *mood_tag;    /* "happy"|"acknowledgment"|"laugh"|"support"|"apology"|"gratitude" */
    const char *tone_tag;    /* "warm"|"dry"|"earnest" */
} hu_sticker_query_t;

/* Pick a sticker file matching the query from `sticker_dir`. On hit:
 * fills out_path (absolute), updates LRU state at
 * <sticker_dir>/../state/sticker_lru.txt, returns true.
 *
 * Pick rule:
 *  - Filter all files by context+mood+tone match (NULL fields match any)
 *  - Of the matched set, prefer files NOT in the recent LRU head
 *  - Among preferred-set, uniform random pick
 *  - Append picked file to LRU front; cap LRU file at 100 entries
 *
 * Returns false if dir missing/empty, no match found, or out_cap too
 * small for the picked path. */
bool hu_persona_pick_sticker(const char *sticker_dir, const hu_sticker_query_t *q, char *out_path,
                             size_t out_cap);

/* Test-only — override the LRU file location (so tests don't pollute
 * the real ~/.human/state). Pass NULL to restore default. */
void hu_persona_sticker_set_test_lru_path(const char *path);

#endif
