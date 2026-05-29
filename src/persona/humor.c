#include "human/persona/humor.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/* ── Context analysis for humor appropriateness ──────────────────── */

static const char *serious_markers[] = {
    "died", "death", "funeral", "cancer", "suicide",
    "depressed", "heartbroken", "divorce", "fired", "laid off",
    "lost my", "grieving", "struggling", "scared", "terrified",
};
static const size_t serious_count = sizeof(serious_markers) / sizeof(serious_markers[0]);

static const char *light_markers[] = {
    "haha", "lol", "lmao", "funny", "joke", "hilarious",
    "😂", "🤣", "just kidding", "laughing",
};
static const size_t light_count = sizeof(light_markers) / sizeof(light_markers[0]);

hu_error_t hu_humor_fw_evaluate_context(const char *conversation, size_t conv_len,
                                     const hu_humor_context_t *ctx,
                                     hu_humor_evaluation_t *eval) {
    if (!conversation || !eval)
        return HU_ERR_INVALID_ARGUMENT;
    memset(eval, 0, sizeof(*eval));

    /* Check for serious context markers */
    size_t serious_hits = 0;
    for (size_t i = 0; i < serious_count; i++) {
        if (hu_str_contains_ci_cstr(conversation, conv_len, serious_markers[i]))
            serious_hits++;
    }

    /* Check for light/humor-receptive markers */
    size_t light_hits = 0;
    for (size_t i = 0; i < light_count; i++) {
        if (hu_str_contains_ci_cstr(conversation, conv_len, light_markers[i]))
            light_hits++;
    }

    if (ctx && ctx->in_serious_context)
        serious_hits += 3;

    /* Appropriateness: high when light, low when serious */
    if (serious_hits > 0 && light_hits == 0)
        eval->appropriateness = 0.1f;
    else if (serious_hits > 0)
        eval->appropriateness = 0.3f;
    else if (light_hits > 0)
        eval->appropriateness = 0.9f;
    else
        eval->appropriateness = 0.5f;

    /* Persona fit: check whether preferred styles match the suggested theory */
    eval->persona_fit = 0.4f;
    if (ctx && ctx->preferred_count > 0) {
        for (size_t si = 0; si < ctx->preferred_count; si++) {
            hu_humor_fw_style_t s = ctx->preferred_styles[si];
            if ((s == HU_HUMOR_FW_DRY && light_hits == 0) ||
                (s == HU_HUMOR_FW_SELF_DEPRECATING) ||
                (s == HU_HUMOR_FW_OBSERVATIONAL && light_hits > 0) ||
                (s == HU_HUMOR_FW_WORDPLAY) ||
                (s == HU_HUMOR_FW_ABSURDIST && serious_hits == 0)) {
                eval->persona_fit = 0.85f;
                break;
            }
        }
        if (eval->persona_fit < 0.85f)
            eval->persona_fit = 0.6f;
    }

    /* Audience fit: real risk_tolerance and channel context */
    eval->audience_fit = 0.5f;
    if (ctx) {
        float risk_factor = ctx->risk_tolerance;
        if (ctx->contact_id && ctx->contact_id_len > 0)
            eval->audience_fit = 0.65f + risk_factor * 0.2f;
        if (ctx->channel && ctx->channel_len > 0) {
            if (ctx->channel_len == 8 &&
                (memcmp(ctx->channel, "imessage", 8) == 0 ||
                 memcmp(ctx->channel, "whatsapp", 8) == 0))
                eval->audience_fit += 0.1f;
        }
        if (eval->audience_fit > 1.0f) eval->audience_fit = 1.0f;
    }

    /* Novelty: penalize if conversation is short (less context = less setup) */
    eval->novelty = conv_len > 200 ? 0.7f : (conv_len > 50 ? 0.5f : 0.3f);

    eval->composite = eval->appropriateness * 0.4f +
                      eval->persona_fit * 0.2f +
                      eval->audience_fit * 0.2f +
                      eval->novelty * 0.2f;

    eval->should_attempt = (eval->composite >= 0.5f && eval->appropriateness >= 0.3f);

    /* Suggest theory based on context */
    if (light_hits > 0)
        eval->suggested_theory = HU_HUMOR_INCONGRUITY;
    else
        eval->suggested_theory = HU_HUMOR_BENIGN_VIOLATION;

    return HU_OK;
}

/* ── Humor directive for prompt ──────────────────────────────────── */

static const char *theory_guidance[] = {
    [HU_HUMOR_INCONGRUITY] =
        "Use incongruity humor: set up a reasonable expectation, then subvert "
        "it with something unexpected but coherent. The surprise should be "
        "logical in hindsight.",
    [HU_HUMOR_BENIGN_VIOLATION] =
        "Use benign violation humor: reference something that feels slightly "
        "wrong or threatening, but is ultimately safe and playful.",
    [HU_HUMOR_SUPERIORITY] =
        "Use gentle self-deprecating humor: poke fun at yourself or shared "
        "human foibles. Never punch down.",
    [HU_HUMOR_RELIEF] =
        "Use relief humor: acknowledge tension or awkwardness, then release "
        "it with a light observation.",
};

hu_error_t hu_humor_fw_build_directive(hu_allocator_t *alloc,
                                    const hu_humor_evaluation_t *eval,
                                    const hu_humor_context_t *ctx,
                                    char **out, size_t *out_len) {
    if (!alloc || !eval || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    (void)ctx;

    if (!eval->should_attempt) {
        *out = NULL;
        *out_len = 0;
        return HU_OK;
    }

    int theory = (int)eval->suggested_theory;
    if (theory < 0 || theory >= HU_HUMOR_THEORY_COUNT) theory = 0;
    const char *guidance = theory_guidance[theory];
    size_t glen = strlen(guidance);
    size_t cap = glen + 64;
    char tmp[512];
    int n = snprintf(tmp, sizeof(tmp), "[HUMOR OPPORTUNITY (%.0f%% appropriate)] %s",
                     (double)(eval->appropriateness * 100.0f), guidance);
    if (n <= 0 || (size_t)n >= sizeof(tmp) || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;

    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)n + 1);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, tmp, (size_t)n + 1);
    *out = buf;
    *out_len = (size_t)n;
    return HU_OK;
}

/* ── Score a response's humor quality ────────────────────────────── */

hu_error_t hu_humor_fw_score_response(const char *response, size_t response_len,
                                   const hu_humor_context_t *ctx,
                                   float *score) {
    if (!response || !score)
        return HU_ERR_INVALID_ARGUMENT;
    (void)ctx;

    float s = 0.0f;

    /* Positive: contains humor signals */
    if (hu_str_contains_ci_cstr(response, response_len, "haha") ||
        hu_str_contains_ci_cstr(response, response_len, "😂") ||
        hu_str_contains_ci_cstr(response, response_len, "lol"))
        s += 0.3f;

    /* Positive: self-deprecation signals */
    if (hu_str_contains_ci_cstr(response, response_len, "i'm terrible at") ||
        hu_str_contains_ci_cstr(response, response_len, "my bad"))
        s += 0.2f;

    /* Moderate length suggests effort (not just a one-liner) */
    if (response_len > 50 && response_len < 300)
        s += 0.2f;

    /* Negative: too short for humor to land */
    if (response_len < 20)
        s -= 0.1f;

    if (s < 0.0f)
        s = 0.0f;
    if (s > 1.0f)
        s = 1.0f;

    *score = s;
    return HU_OK;
}
