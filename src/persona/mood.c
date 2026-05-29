/*
 * F60 — Mood Persistence Across Conversations (DOMAIN module).
 * Global mood state carrying across all contacts. Decays toward neutral.
 *
 * Only the pure mood-name mapping + the prompt-directive builder live here.
 * The persistence (get_current / set SQL + the mood_log table + the raw sqlite3
 * handle + the in-memory cache) was relocated to
 * src/memory/repos/mood_repo_sqlite.c so this module no longer includes
 * <sqlite3.h> (memory repository pattern; sqlite-includer ratchet).
 */
#include "human/persona/mood.h"
#include "human/core/allocator.h"
#include <stdio.h>

static const char *MOOD_NAMES[] = {
    "neutral",   "happy",         "stressed", "tired", "energized",
    "irritable", "contemplative", "excited",  "sad",
};

const char *hu_mood_name(hu_mood_enum_t mood) {
    if (mood >= 0 && mood < (int)HU_MOOD_COUNT)
        return MOOD_NAMES[mood];
    return "neutral";
}

char *hu_mood_build_directive(hu_allocator_t *alloc, const hu_mood_state_t *state,
                              size_t *out_len) {
    if (!alloc || !state || !out_len)
        return NULL;

    if (state->intensity < 0.2f)
        return NULL;

    const char *qualifier = "Slightly";
    if (state->intensity >= 0.7f)
        qualifier = "Quite";
    else if (state->intensity >= 0.5f)
        qualifier = "Moderately";

    const char *mood_name = hu_mood_name(state->mood);

    size_t cap = 256;
    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return NULL;

    int n;
    if (state->cause[0] != '\0') {
        n = snprintf(buf, cap, "[CURRENT MOOD: %s %s (%s). Affects warmth and patience.]",
                     qualifier, mood_name, state->cause);
    } else {
        n = snprintf(buf, cap, "[CURRENT MOOD: %s %s. Affects warmth and patience.]", qualifier,
                     mood_name);
    }

    if (n <= 0 || (size_t)n >= cap) {
        alloc->free(alloc->ctx, buf, cap);
        return NULL;
    }

    size_t need = (size_t)n + 1;
    char *shrunk = (char *)alloc->realloc(alloc->ctx, buf, cap, need);
    if (!shrunk) {
        alloc->free(alloc->ctx, buf, cap);
        return NULL;
    }
    *out_len = (size_t)n;
    return shrunk;
}
