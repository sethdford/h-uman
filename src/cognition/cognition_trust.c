#include "human/cognition/trust.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* ── EMA smoothing for trust updates ─────────────────────────────── */

static const float TCAL_ALPHA = 0.15f;
static const float TCAL_EROSION_THRESHOLD = 0.1f;

static float ema(float old, float signal) {
    return old * (1.0f - TCAL_ALPHA) + signal * TCAL_ALPHA;
}

/* ── Trust initialization ────────────────────────────────────────── */

void hu_tcal_init(hu_tcal_state_t *state) {
    if (!state)
        return;
    memset(state, 0, sizeof(*state));
    state->dimensions.competence = 0.5f;
    state->dimensions.benevolence = 0.5f;
    state->dimensions.integrity = 0.5f;
    state->dimensions.predictability = 0.5f;
    state->dimensions.transparency = 0.5f;
    state->level = HU_TCAL_UNKNOWN;
    state->composite = 0.5f;
}

/* ── Trust update ────────────────────────────────────────────────── */

void hu_tcal_update(hu_tcal_state_t *state,
                    float competence_signal,
                    float benevolence_signal,
                    float integrity_signal) {
    if (!state)
        return;

    float prev_composite = state->composite;

    state->dimensions.competence = ema(state->dimensions.competence, competence_signal);
    state->dimensions.benevolence = ema(state->dimensions.benevolence, benevolence_signal);
    state->dimensions.integrity = ema(state->dimensions.integrity, integrity_signal);

    float consistency = 1.0f - fabsf(competence_signal - state->dimensions.competence);
    state->dimensions.predictability = ema(state->dimensions.predictability, consistency);

    /* Transparency: grows when integrity and benevolence signals are both positive
     * (open, honest behavior). Erodes when integrity drops (evasive/contradictory). */
    float transparency_sig = (integrity_signal + benevolence_signal) * 0.5f;
    state->dimensions.transparency = ema(state->dimensions.transparency, transparency_sig);

    state->interaction_count++;
    state->level = hu_tcal_compute_level(&state->dimensions);
    state->composite = state->dimensions.competence * 0.3f +
                       state->dimensions.benevolence * 0.25f +
                       state->dimensions.integrity * 0.25f +
                       state->dimensions.predictability * 0.1f +
                       state->dimensions.transparency * 0.1f;

    float delta = state->composite - prev_composite;
    if (delta < -TCAL_EROSION_THRESHOLD) {
        state->erosion_detected = true;
        state->erosion_rate = -delta;
    } else {
        state->erosion_detected = false;
        state->erosion_rate = 0.0f;
    }
}

/* ── Trust level computation ─────────────────────────────────────── */

hu_tcal_level_t hu_tcal_compute_level(const hu_tcal_dimensions_t *dims) {
    if (!dims)
        return HU_TCAL_UNKNOWN;
    float avg = (dims->competence + dims->benevolence + dims->integrity +
                 dims->predictability + dims->transparency) / 5.0f;
    if (avg >= 0.85f)
        return HU_TCAL_DEEP;
    if (avg >= 0.7f)
        return HU_TCAL_ESTABLISHED;
    if (avg >= 0.5f)
        return HU_TCAL_DEVELOPING;
    if (avg >= 0.3f)
        return HU_TCAL_CAUTIOUS;
    return HU_TCAL_UNKNOWN;
}

/* ── Confidence-to-language mapping ──────────────────────────────── */

const char *hu_tcal_confidence_language(float confidence,
                                        hu_tcal_level_t contact_trust) {
    if (contact_trust >= HU_TCAL_ESTABLISHED) {
        if (confidence >= 0.9f) return "I'm sure";
        if (confidence >= 0.7f) return "I'm pretty sure";
        if (confidence >= 0.5f) return "I think";
        if (confidence >= 0.3f) return "hmm, I'm not totally sure but";
        return "honestly I'm not sure, but";
    }
    if (contact_trust >= HU_TCAL_DEVELOPING) {
        if (confidence >= 0.9f) return "I'm confident that";
        if (confidence >= 0.7f) return "I'm fairly sure that";
        if (confidence >= 0.5f) return "I think";
        if (confidence >= 0.3f) return "I believe, though I'm not certain, that";
        return "I'm not sure, but";
    }
    /* UNKNOWN or CAUTIOUS — formal hedging */
    if (confidence >= 0.9f) return "I'm confident that";
    if (confidence >= 0.75f) return "I'm fairly sure that";
    if (confidence >= 0.6f) return "I think";
    if (confidence >= 0.4f) return "I believe, though I'm not certain, that";
    if (confidence >= 0.2f) return "I'm not sure, but";
    return "I really don't know, but my best guess is";
}

/* ── Build trust context for prompt ──────────────────────────────── */

hu_error_t hu_tcal_build_context(hu_allocator_t *alloc,
                                 const hu_tcal_state_t *state,
                                 char **out, size_t *out_len) {
    if (!alloc || !state || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    static const char *level_names[] = {
        [HU_TCAL_UNKNOWN] = "unknown",
        [HU_TCAL_CAUTIOUS] = "cautious",
        [HU_TCAL_DEVELOPING] = "developing",
        [HU_TCAL_ESTABLISHED] = "established",
        [HU_TCAL_DEEP] = "deep",
    };

    int sl = (int)state->level;
    int n_level_names = (int)(sizeof(level_names) / sizeof(level_names[0]));
    int lvl = (sl >= 0 && sl < n_level_names) ? sl : 0;
    const char *level_name = level_names[lvl];

    const char *high_conf = hu_tcal_confidence_language(0.9f, state->level);
    const char *mid_conf = hu_tcal_confidence_language(0.5f, state->level);
    const char *low_conf = hu_tcal_confidence_language(0.2f, state->level);

    char buf[768];
    int n = snprintf(buf, sizeof(buf),
        "[TRUST CALIBRATION] Trust level: %s (%.2f). "
        "Interactions: %zu. %s"
        "Calibrate uncertainty language to your actual confidence: "
        "high=\"%s\", medium=\"%s\", low=\"%s\".",
        level_name, state->composite,
        state->interaction_count,
        state->erosion_detected ? "WARNING: Trust erosion detected. Be extra careful with claims. " : "",
        high_conf, mid_conf, low_conf);

    if (n <= 0 || (size_t)n >= sizeof(buf))
        return HU_ERR_INVALID_ARGUMENT;

    char *result = (char *)alloc->alloc(alloc->ctx, (size_t)n + 1);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(result, buf, (size_t)n + 1);
    *out = result;
    *out_len = (size_t)n;
    return HU_OK;
}

/* ── Erosion detection ───────────────────────────────────────────── */

bool hu_tcal_check_erosion(const hu_tcal_state_t *state) {
    if (!state)
        return false;
    return state->erosion_detected;
}
