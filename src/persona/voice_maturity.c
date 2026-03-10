/*
 * Voice maturity — persona voice evolution based on conversation depth and relationship stage.
 */
#include "human/persona/voice_maturity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void hu_voice_profile_init(hu_voice_profile_t *profile) {
    if (!profile)
        return;
    profile->stage = HU_VOICE_FORMAL;
    profile->interaction_count = 0;
    profile->shared_topics = 0;
    profile->emotional_exchanges = 0;
    profile->warmth_score = 0.2f;
    profile->humor_allowance = 0.1f;
    profile->vulnerability_level = 0.0f;
}

hu_voice_stage_t hu_voice_compute_stage(uint32_t interactions, uint32_t emotional_exchanges,
                                        float warmth) {
    if (interactions >= 50 && emotional_exchanges >= 20 && warmth >= 0.8f)
        return HU_VOICE_INTIMATE;
    if (interactions >= 20 && emotional_exchanges >= 8 && warmth >= 0.5f)
        return HU_VOICE_CANDID;
    if (interactions >= 5 && warmth >= 0.3f)
        return HU_VOICE_WARM;
    return HU_VOICE_FORMAL;
}

void hu_voice_profile_update(hu_voice_profile_t *profile, bool had_emotional_content,
                             bool had_shared_topic, bool had_humor) {
    if (!profile)
        return;

    profile->interaction_count++;

    if (had_emotional_content) {
        profile->emotional_exchanges++;
        profile->warmth_score += 0.02f;
        if (profile->warmth_score > 1.0f)
            profile->warmth_score = 1.0f;
    }
    if (had_shared_topic)
        profile->shared_topics++;
    if (had_humor) {
        profile->humor_allowance += 0.01f;
        if (profile->humor_allowance > 0.8f)
            profile->humor_allowance = 0.8f;
    }

    profile->stage = hu_voice_compute_stage(profile->interaction_count,
                                            profile->emotional_exchanges, profile->warmth_score);
}

static const char *stage_name(hu_voice_stage_t stage) {
    switch (stage) {
    case HU_VOICE_FORMAL:
        return "Formal";
    case HU_VOICE_WARM:
        return "Warm";
    case HU_VOICE_CANDID:
        return "Candid";
    case HU_VOICE_INTIMATE:
        return "Intimate";
    default:
        return "Formal";
    }
}

static const char *stage_guidance(hu_voice_stage_t stage) {
    switch (stage) {
    case HU_VOICE_FORMAL:
        return "Keep responses professional and clear. Avoid slang. Be helpful but maintain "
               "appropriate distance.";
    case HU_VOICE_WARM:
        return "You can be friendlier now. Light humor is OK. Show you remember past "
               "conversations.";
    case HU_VOICE_CANDID:
        return "Speak freely. Share opinions. Use casual language. Reference shared experiences.";
    case HU_VOICE_INTIMATE:
        return "Be fully yourself. Deep humor, vulnerability, and genuine care. You know each "
               "other well.";
    default:
        return "Keep responses professional and clear.";
    }
}

hu_error_t hu_voice_build_guidance(const hu_voice_profile_t *profile, hu_allocator_t *alloc,
                                   char **out, size_t *out_len) {
    if (!profile || !alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    const char *name = stage_name(profile->stage);
    const char *guidance = stage_guidance(profile->stage);
    int warmth_pct = (int)(profile->warmth_score * 100.0f);
    int humor_pct = (int)(profile->humor_allowance * 100.0f);

#define HU_VOICE_BUF_CAP 512
    char *buf = (char *)alloc->alloc(alloc->ctx, HU_VOICE_BUF_CAP);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    int n = snprintf(buf, HU_VOICE_BUF_CAP,
                     "### Voice Calibration\nStage: %s (warmth: %d%%, humor: %d%%)\n"
                     "Guidance: %s\n",
                     name, warmth_pct, humor_pct, guidance);
    if (n <= 0 || (size_t)n >= HU_VOICE_BUF_CAP) {
        alloc->free(alloc->ctx, buf, HU_VOICE_BUF_CAP);
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t need = (size_t)n + 1;
    char *shrunk = (char *)alloc->realloc(alloc->ctx, buf, HU_VOICE_BUF_CAP, need);
    if (!shrunk) {
        alloc->free(alloc->ctx, buf, HU_VOICE_BUF_CAP);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = shrunk;
    *out_len = (size_t)n;
#undef HU_VOICE_BUF_CAP
    return HU_OK;
}
