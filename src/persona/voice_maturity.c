/*
 * Voice maturity — persona voice evolution based on conversation depth and relationship stage.
 */
#include "human/persona/voice_maturity.h"
#include "human/core/string.h"
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

/* ── Vulnerability content scoring ────────────────────────────────── */

float hu_voice_vulnerability_from_content(const char *text, size_t text_len) {
    if (!text || text_len == 0)
        return 0.0f;

    static const char *const emotional_markers[] = {
        "i feel",        "i'm feeling",   "i'm scared",  "i'm afraid", "i'm worried",
        "i'm sad",       "i'm lonely",    "i'm hurt",    "i miss",     "i love",
        "i'm grateful",  "thank you for", "means a lot", "opened up",  "vulnerable",
        "trust you",     "honest with",   "never told",  "secret",     "struggling",
        "going through", "hard time",     "emotional",   "crying",     "broke down",
        "overwhelmed",   "anxious",       "depressed",
    };
    static const size_t marker_count = sizeof(emotional_markers) / sizeof(emotional_markers[0]);

    size_t hits = 0;
    for (size_t i = 0; i < marker_count; i++) {
        if (hu_str_contains_ci_cstr(text, text_len, emotional_markers[i]))
            hits++;
    }

    /* Score: 1 hit = 0.15, 2 hits = 0.3, saturates at ~0.8 */
    float score = (float)hits * 0.15f;
    if (score > 1.0f)
        score = 1.0f;
    return score;
}

void hu_voice_vulnerability_decay(hu_voice_profile_t *profile, float hours_elapsed) {
    if (!profile || hours_elapsed <= 0.0f)
        return;
    /* Decay rate: lose ~10% per hour of inactivity */
    float decay = hours_elapsed * 0.1f;
    profile->vulnerability_level -= decay;
    if (profile->vulnerability_level < 0.0f)
        profile->vulnerability_level = 0.0f;
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

        /* Increase vulnerability based on emotional exchanges */
        profile->vulnerability_level += 0.05f;
        if (profile->vulnerability_level > 1.0f)
            profile->vulnerability_level = 1.0f;
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

size_t hu_voice_maturity_build_directive(hu_voice_stage_t stage, char *out_buf, size_t buf_len) {
    if (!out_buf || buf_len == 0)
        return 0;

    static const char *const directives[] = {
        /* HU_VOICE_FORMAL */
        "[VOICE STAGE: Early formal — keep replies measured. Don't presume familiarity.]",
        /* HU_VOICE_WARM */
        "[VOICE STAGE: Warm — friendly, attentive, gently personal. You're past pleasantries.]",
        /* HU_VOICE_CANDID */
        "[VOICE STAGE: Candid — speak directly. Trade pleasantries for honesty.]",
        /* HU_VOICE_INTIMATE — clang-format off keeps the single-line literal that
           -Wstring-concatenation requires */
        /* clang-format off */
        "[VOICE STAGE: Intimate — you've earned vulnerability. Reference shared history; don't perform warmth.]",
        /* clang-format on */
    };

    size_t idx = (size_t)stage;
    if (idx >= sizeof(directives) / sizeof(directives[0]))
        idx = 0;

    const char *src = directives[idx];
    size_t src_len = strlen(src);
    size_t copy_len = src_len < buf_len - 1 ? src_len : buf_len - 1;
    memcpy(out_buf, src, copy_len);
    out_buf[copy_len] = '\0';
    return copy_len;
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

    int vuln_pct = (int)(profile->vulnerability_level * 100.0f);

    int n = snprintf(buf, HU_VOICE_BUF_CAP,
                     "### Voice Calibration\nStage: %s (warmth: %d%%, humor: %d%%, "
                     "vulnerability: %d%%)\nGuidance: %s\n",
                     name, warmth_pct, humor_pct, vuln_pct, guidance);
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
