/*
 * Relationship depth tracker — session-based warmth and formality adaptation.
 */
#include "human/persona/relationship.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/data/loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Default fallback values */
static const char *DEFAULT_STAGE_NAMES[] = {"new", "familiar", "trusted", "deep"};

static const char *DEFAULT_STAGE_GUIDANCE[] = {
    "This is a newer relationship. Be helpful, clear, and professional. Build trust through "
    "reliability.",
    "You know this user moderately well. Reference past conversations when relevant. Be warmer.",
    "This is a trusted relationship. Be candid and proactive. Share observations and insights "
    "freely.",
    "This is a deep, long-standing relationship. Be genuinely present. Anticipate needs. "
    "Celebrate growth.",
};

/* Runtime loaded data */
static const char **s_stage_names = (const char **)DEFAULT_STAGE_NAMES;
static const char **s_stage_guidance = (const char **)DEFAULT_STAGE_GUIDANCE;
static size_t s_stage_count = 4;

hu_error_t hu_relationship_data_init(hu_allocator_t *alloc) {
    if (!alloc)
        return HU_ERR_INVALID_ARGUMENT;

    char *json_data = NULL;
    size_t json_len = 0;
    hu_error_t err = hu_data_load(alloc, "persona/relationship_stages.json", &json_data, &json_len);
    if (err != HU_OK)
        return HU_OK; /* Fail gracefully, keep defaults */

    hu_json_value_t *root = NULL;
    err = hu_json_parse(alloc, json_data, json_len, &root);
    alloc->free(alloc->ctx, json_data, json_len);
    if (err != HU_OK || !root)
        return HU_OK; /* Fail gracefully, keep defaults */

    hu_json_value_t *stages_arr = hu_json_object_get(root, "stages");
    if (!stages_arr || stages_arr->type != HU_JSON_ARRAY) {
        hu_json_free(alloc, root);
        return HU_OK;
    }

    size_t stage_count = stages_arr->data.array.len;
    if (stage_count == 0 || stage_count > 4) {
        hu_json_free(alloc, root);
        return HU_OK;
    }

    const char **names =
        (const char **)alloc->alloc(alloc->ctx, stage_count * sizeof(const char *));
    const char **guidance =
        (const char **)alloc->alloc(alloc->ctx, stage_count * sizeof(const char *));
    if (!names || !guidance) {
        if (names)
            alloc->free(alloc->ctx, names, stage_count * sizeof(const char *));
        if (guidance)
            alloc->free(alloc->ctx, guidance, stage_count * sizeof(const char *));
        hu_json_free(alloc, root);
        return HU_OK;
    }

    memset(names, 0, stage_count * sizeof(const char *));
    memset(guidance, 0, stage_count * sizeof(const char *));

    for (size_t i = 0; i < stage_count; i++) {
        hu_json_value_t *stage_obj = stages_arr->data.array.items[i];
        if (!stage_obj || stage_obj->type != HU_JSON_OBJECT)
            continue;

        const char *name = hu_json_get_string(stage_obj, "name");
        const char *guide = hu_json_get_string(stage_obj, "guidance");

        if (name) {
            names[i] = hu_strndup(alloc, name, strlen(name));
            if (!names[i])
                goto relationship_stages_load_fail;
        }
        if (guide) {
            guidance[i] = hu_strndup(alloc, guide, strlen(guide));
            if (!guidance[i])
                goto relationship_stages_load_fail;
        }
    }

    /* Atomically swap in new data */
    s_stage_names = names;
    s_stage_guidance = guidance;
    s_stage_count = stage_count;

    hu_json_free(alloc, root);
    return HU_OK;

relationship_stages_load_fail:
    for (size_t j = 0; j < stage_count; j++) {
        if (names[j])
            alloc->free(alloc->ctx, (char *)names[j], strlen(names[j]) + 1);
        if (guidance[j])
            alloc->free(alloc->ctx, (char *)guidance[j], strlen(guidance[j]) + 1);
    }
    alloc->free(alloc->ctx, names, stage_count * sizeof(const char *));
    alloc->free(alloc->ctx, guidance, stage_count * sizeof(const char *));
    hu_json_free(alloc, root);
    return HU_OK;
}

void hu_relationship_data_cleanup(hu_allocator_t *alloc) {
    if (!alloc)
        return;
    /* Only free if they're not the defaults */
    if (s_stage_names != (const char **)DEFAULT_STAGE_NAMES) {
        for (size_t i = 0; i < s_stage_count; i++) {
            if (s_stage_names[i])
                alloc->free(alloc->ctx, (char *)s_stage_names[i], strlen(s_stage_names[i]) + 1);
        }
        alloc->free(alloc->ctx, s_stage_names, s_stage_count * sizeof(const char *));
    }
    if (s_stage_guidance != (const char **)DEFAULT_STAGE_GUIDANCE) {
        for (size_t i = 0; i < s_stage_count; i++) {
            if (s_stage_guidance[i])
                alloc->free(alloc->ctx, (char *)s_stage_guidance[i],
                            strlen(s_stage_guidance[i]) + 1);
        }
        alloc->free(alloc->ctx, s_stage_guidance, s_stage_count * sizeof(const char *));
    }
    s_stage_names = (const char **)DEFAULT_STAGE_NAMES;
    s_stage_guidance = (const char **)DEFAULT_STAGE_GUIDANCE;
    s_stage_count = 4;
}

/* Compute weighted quality from a single session's signals */
float hu_session_quality_score(const hu_session_quality_t *q) {
    if (!q)
        return 0.0f;
    float score = q->emotional_exchanges * 0.3f + q->topic_diversity * 0.2f +
                  q->vulnerability_events * 0.3f + q->humor_shared * 0.1f +
                  q->repair_survived * 0.1f;
    if (score < 0.0f)
        score = 0.0f;
    if (score > 1.0f)
        score = 1.0f;
    return score;
}

/* Map cumulative quality to relationship stage */
static hu_relationship_stage_t stage_from_quality(float quality) {
    if (quality >= 0.80f)
        return HU_REL_DEEP;
    if (quality >= 0.55f)
        return HU_REL_TRUSTED;
    if (quality >= 0.25f)
        return HU_REL_FAMILIAR;
    return HU_REL_NEW;
}

/* Legacy: session-count-only progression */
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

/* Quality-weighted session progression with regression support */
void hu_relationship_new_session_quality(hu_relationship_state_t *state,
                                         const hu_session_quality_t *quality,
                                         float velocity_factor) {
    if (!state || !quality)
        return;

    state->session_count++;
    float session_score = hu_session_quality_score(quality);

    /* Apply velocity factor as acceleration/deceleration */
    if (velocity_factor <= 0.0f)
        velocity_factor = 1.0f;
    session_score *= velocity_factor;
    if (session_score > 1.0f)
        session_score = 1.0f;

    /* Update cumulative quality: exponential moving average.
     * session_count contributes a small baseline (max 0.1 at 50 sessions)
     * so pure session count alone never reaches DEEP. */
    float session_baseline = (float)state->session_count / 500.0f;
    if (session_baseline > 0.1f)
        session_baseline = 0.1f;

    hu_relationship_quality_score_t *qs = &state->quality;
    if (qs->quality_sessions == 0) {
        qs->cumulative_quality = session_score + session_baseline;
    } else {
        /* EMA with alpha = 0.3 (recent sessions matter more) */
        qs->cumulative_quality =
            qs->cumulative_quality * 0.7f + (session_score + session_baseline) * 0.3f;
    }
    if (qs->cumulative_quality > 1.0f)
        qs->cumulative_quality = 1.0f;
    if (qs->cumulative_quality < 0.0f)
        qs->cumulative_quality = 0.0f;

    qs->recent_quality = session_score;
    qs->quality_sessions++;

    /* Stage can both advance AND regress based on quality */
    state->stage = stage_from_quality(qs->cumulative_quality);
}

void hu_relationship_update(hu_relationship_state_t *state, uint32_t turn_count) {
    if (!state)
        return;
    state->total_turns += turn_count;

    /* Gradually advance stage based on accumulated turns when no quality
     * scoring is available (e.g. CLI mode without the daemon). This is a
     * conservative fallback — quality-based progression in
     * hu_relationship_new_session_quality is preferred. */
    hu_relationship_stage_t floor;
    if (state->total_turns >= 200)
        floor = HU_REL_DEEP;
    else if (state->total_turns >= 80)
        floor = HU_REL_TRUSTED;
    else if (state->total_turns >= 20)
        floor = HU_REL_FAMILIAR;
    else
        floor = HU_REL_NEW;
    if (floor > state->stage)
        state->stage = floor;
}

hu_error_t hu_relationship_build_prompt(hu_allocator_t *alloc, const hu_relationship_state_t *state,
                                        char **out, size_t *out_len) {
    if (!alloc || !state || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    size_t stage_ix = (size_t)state->stage;
    const char *stage_name;
    const char *guidance;
    if (stage_ix >= s_stage_count) {
        stage_name = "unknown";
        guidance = "Relationship stage is indeterminate; be clear, professional, and avoid "
                   "assuming depth.";
    } else {
        stage_name = s_stage_names[stage_ix];
        guidance = s_stage_guidance[stage_ix];
    }

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
