/*
 * Relationship depth tracker — session-based warmth and formality adaptation.
 */
#include "human/persona/relationship.h"
#include "human/core/string.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *STAGE_NAMES[] = {"new", "familiar", "trusted", "deep"};

/* Default guidance per stage. Persona-specific overrides via time_overlays; future config can
 * externalize. */
static const char *STAGE_GUIDANCE[] = {
    "This is a newer relationship. Be helpful, clear, and professional. Build trust through "
    "reliability.",
    "You know this user moderately well. Reference past conversations when relevant. Be warmer.",
    "This is a trusted relationship. Be candid and proactive. Share observations and insights "
    "freely.",
    "This is a deep, long-standing relationship. Be genuinely present. Anticipate needs. "
    "Celebrate growth.",
};

void hu_relationship_new_session(hu_relationship_state_t *state) {
    if (!state)
        return;
    state->session_count++;
    if (state->session_count >= 50)
        state->stage = HU_REL_DEEP;
    else if (state->session_count >= 20)
        state->stage = HU_REL_TRUSTED;
    else if (state->session_count >= 5)
        state->stage = HU_REL_FAMILIAR;
    else
        state->stage = HU_REL_NEW;
}

void hu_relationship_update(hu_relationship_state_t *state, uint32_t turn_count) {
    if (!state)
        return;
    state->total_turns += turn_count;
}

hu_error_t hu_relationship_build_prompt(hu_allocator_t *alloc, const hu_relationship_state_t *state,
                                        char **out, size_t *out_len) {
    if (!alloc || !state || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    const char *stage_name = STAGE_NAMES[(size_t)state->stage];
    const char *guidance = STAGE_GUIDANCE[(size_t)state->stage];

#define HU_REL_BUF_CAP 256
    char *buf = (char *)alloc->alloc(alloc->ctx, HU_REL_BUF_CAP);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    int n =
        snprintf(buf, HU_REL_BUF_CAP, "\n### Relationship Context\nStage: %s. Sessions: %u. %s\n",
                 stage_name, state->session_count, guidance);
    if (n <= 0 || (size_t)n >= HU_REL_BUF_CAP) {
        alloc->free(alloc->ctx, buf, HU_REL_BUF_CAP);
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t need = (size_t)n + 1;
    char *shrunk = (char *)alloc->realloc(alloc->ctx, buf, HU_REL_BUF_CAP, need);
    if (!shrunk) {
        alloc->free(alloc->ctx, buf, HU_REL_BUF_CAP);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = shrunk;
    *out_len = (size_t)n;
#undef HU_REL_BUF_CAP
    return HU_OK;
}
