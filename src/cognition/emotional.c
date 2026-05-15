#include "human/cognition/emotional.h"
#include "human/core/string.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* Copy of external dominant_emotion strings; perception inputs may not outlive ec. */
#define EMOTIONAL_DOM_LABEL_CAP 96
static _Thread_local char s_emotional_dom_label[EMOTIONAL_DOM_LABEL_CAP];

/* Heuristic thresholds for fusion, escalation, and prompt gating */
#define EMOTIONAL_CONFIDENCE_INTENSITY_BOOST_THRESHOLD 0.5f
#define EMOTIONAL_HIGH_INTENSITY_THRESHOLD             0.6f
#define EMOTIONAL_ESCALATION_VALENCE_THRESHOLD         (-0.5f)
#define EMOTIONAL_ESCALATION_INTENSITY_THRESHOLD       0.7f
#define EMOTIONAL_TRAJECTORY_SLOPE_EPSILON             0.05f
#define EMOTIONAL_MIN_PROMPT_INTENSITY                 0.1f

void hu_emotional_cognition_init(hu_emotional_cognition_t *ec) {
    if (!ec)
        return;
    memset(ec, 0, sizeof(*ec));
    ec->state.dominant_emotion = "neutral";
    ec->secondary_emotion = HU_EMOTION_NEUTRAL;
}

/* Map hu_emotion_tag_t to valence estimate */
static float emotion_tag_to_valence(hu_emotion_tag_t tag) {
    switch (tag) {
    case HU_EMOTION_JOY:
        return 0.8f;
    case HU_EMOTION_EXCITEMENT:
        return 0.7f;
    case HU_EMOTION_SURPRISE:
        return 0.1f;
    case HU_EMOTION_NEUTRAL:
        return 0.0f;
    case HU_EMOTION_SADNESS:
        return -0.6f;
    case HU_EMOTION_FRUSTRATION:
        return -0.5f;
    case HU_EMOTION_ANGER:
        return -0.7f;
    case HU_EMOTION_FEAR:
        return -0.6f;
    case HU_EMOTION_ANXIETY:
        return -0.4f;
    default:
        return 0.0f;
    }
}

static const char *emotion_tag_to_label(hu_emotion_tag_t tag) {
    switch (tag) {
    case HU_EMOTION_JOY:
        return "joy";
    case HU_EMOTION_EXCITEMENT:
        return "excitement";
    case HU_EMOTION_SURPRISE:
        return "surprise";
    case HU_EMOTION_NEUTRAL:
        return "neutral";
    case HU_EMOTION_SADNESS:
        return "sadness";
    case HU_EMOTION_FRUSTRATION:
        return "frustration";
    case HU_EMOTION_ANGER:
        return "anger";
    case HU_EMOTION_FEAR:
        return "fear";
    case HU_EMOTION_ANXIETY:
        return "anxiety";
    default:
        return "neutral";
    }
}

void hu_emotional_cognition_perceive(hu_emotional_cognition_t *ec,
                                     const hu_emotional_perception_t *p) {
    if (!ec || !p)
        return;

    float valence_sum = 0.0f;
    float intensity_sum = 0.0f;
    float weight_sum = 0.0f;
    uint8_t mask = 0;
    bool concerning = false;
    hu_emotion_tag_t dominant = HU_EMOTION_NEUTRAL;
    float dominant_intensity = 0.0f;
    hu_emotion_tag_t secondary = HU_EMOTION_NEUTRAL;
    float secondary_intensity = 0.0f;

    /* Fast capture (highest trust weight) */
    if (p->fast_capture) {
        float w = 2.0f;
        valence_sum += p->fast_capture->valence * w;
        intensity_sum += p->fast_capture->intensity * w;
        weight_sum += w;
        mask |= HU_EMOTION_SRC_FAST_CAPTURE;
        if (p->fast_capture->concerning)
            concerning = true;
        if (p->fast_capture->intensity > dominant_intensity) {
            dominant_intensity = p->fast_capture->intensity;
        }
    }

    /* Conversation-level detection */
    if (p->conversation) {
        float w = 1.5f;
        valence_sum += p->conversation->valence * w;
        intensity_sum += p->conversation->intensity * w;
        weight_sum += w;
        mask |= HU_EMOTION_SRC_CONVERSATION;
        if (p->conversation->concerning)
            concerning = true;
    }

    /* STM emotion history */
    if (p->stm_emotions && p->stm_emotion_count > 0) {
        float w = 1.0f;
        float stm_valence = 0.0f;
        float stm_intensity = 0.0f;
        for (size_t i = 0; i < p->stm_emotion_count; i++) {
            stm_valence += emotion_tag_to_valence(p->stm_emotions[i].tag);
            stm_intensity += (float)p->stm_emotions[i].intensity;
            /* Track top-2 emotions */
            float this_int = (float)p->stm_emotions[i].intensity;
            if (this_int > dominant_intensity) {
                secondary = dominant;
                secondary_intensity = dominant_intensity;
                dominant = p->stm_emotions[i].tag;
                dominant_intensity = this_int;
            } else if (this_int > secondary_intensity) {
                secondary = p->stm_emotions[i].tag;
                secondary_intensity = this_int;
            }
        }
        stm_valence /= (float)p->stm_emotion_count;
        stm_intensity /= (float)p->stm_emotion_count;
        valence_sum += stm_valence * w;
        intensity_sum += stm_intensity * w;
        weight_sum += w;
        mask |= HU_EMOTION_SRC_STM_HISTORY;
    }

    /* Emotional graph */
    if (p->egraph_dominant != HU_EMOTION_NEUTRAL) {
        float w = 0.8f;
        valence_sum += emotion_tag_to_valence(p->egraph_dominant) * w;
        intensity_sum += p->egraph_intensity * w;
        weight_sum += w;
        mask |= HU_EMOTION_SRC_EGRAPH;
    }

    /* Voice channel prosody */
    if (!isnan(p->voice_valence)) {
        float w = 1.2f;
        valence_sum += p->voice_valence * w;
        intensity_sum += fabsf(p->voice_valence) * w;
        weight_sum += w;
        mask |= HU_EMOTION_SRC_VOICE;
    }

    /* Fuse */
    float fused_valence = 0.0f;
    float fused_intensity = 0.0f;
    if (weight_sum > 0.0f) {
        fused_valence = valence_sum / weight_sum;
        fused_intensity = intensity_sum / weight_sum;
    }

    /* Clamp */
    if (fused_valence > 1.0f)
        fused_valence = 1.0f;
    if (fused_valence < -1.0f)
        fused_valence = -1.0f;
    if (fused_intensity > 1.0f)
        fused_intensity = 1.0f;
    if (fused_intensity < 0.0f)
        fused_intensity = 0.0f;

    /* Confidence: more sources and stronger agreement = higher confidence */
    int source_count = 0;
    for (int i = 0; i < HU_EMOTION_SRC_BIT_COUNT; i++) {
        if (mask & (1 << i))
            source_count++;
    }
    float conf = (float)source_count / (float)HU_EMOTION_SRC_BIT_COUNT;
    if (fused_intensity > EMOTIONAL_CONFIDENCE_INTENSITY_BOOST_THRESHOLD)
        conf += 0.1f;
    if (conf > 1.0f)
        conf = 1.0f;

    /* Determine dominant label from tag or fast_capture */
    const char *dom_label = "neutral";
    if (dominant != HU_EMOTION_NEUTRAL) {
        dom_label = emotion_tag_to_label(dominant);
    } else if (p->fast_capture && p->fast_capture->dominant_emotion) {
        (void)snprintf(s_emotional_dom_label, sizeof(s_emotional_dom_label), "%s",
                       p->fast_capture->dominant_emotion);
        dom_label = s_emotional_dom_label;
    } else if (p->conversation && p->conversation->dominant_emotion) {
        (void)snprintf(s_emotional_dom_label, sizeof(s_emotional_dom_label), "%s",
                       p->conversation->dominant_emotion);
        dom_label = s_emotional_dom_label;
    }

    ec->state.valence = fused_valence;
    ec->state.intensity = fused_intensity;
    ec->state.concerning = concerning;
    ec->state.dominant_emotion = dom_label;
    ec->source_mask = mask;
    ec->confidence = conf;
    ec->secondary_emotion = secondary;
    ec->needs_empathy_boost = (fused_intensity > EMOTIONAL_HIGH_INTENSITY_THRESHOLD && concerning);
    ec->escalation_detected = (fused_valence < EMOTIONAL_ESCALATION_VALENCE_THRESHOLD &&
                               fused_intensity > EMOTIONAL_ESCALATION_INTENSITY_THRESHOLD);

    hu_emotional_cognition_update_trajectory(ec, fused_valence);
}

void hu_emotional_cognition_update_trajectory(hu_emotional_cognition_t *ec, float valence) {
    if (!ec)
        return;

    ec->valence_history[ec->valence_idx] = valence;
    ec->valence_idx = (ec->valence_idx + 1) % HU_EMOTIONAL_TRAJECTORY_LEN;
    if (ec->valence_count < HU_EMOTIONAL_TRAJECTORY_LEN)
        ec->valence_count++;

    /* Simple linear regression slope over the buffer */
    if (ec->valence_count < 2) {
        ec->trajectory_slope = 0.0f;
        return;
    }
    size_t n = ec->valence_count;
    float sum_x = 0.0f, sum_y = 0.0f, sum_xy = 0.0f, sum_x2 = 0.0f;
    for (size_t i = 0; i < n; i++) {
        size_t buf_idx;
        if (ec->valence_count < HU_EMOTIONAL_TRAJECTORY_LEN)
            buf_idx = i;
        else
            buf_idx = (ec->valence_idx + i) % HU_EMOTIONAL_TRAJECTORY_LEN;
        float x = (float)i;
        float y = ec->valence_history[buf_idx];
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }
    float denom = (float)n * sum_x2 - sum_x * sum_x;
    if (fabsf(denom) < 1e-9f) {
        ec->trajectory_slope = 0.0f;
    } else {
        ec->trajectory_slope = ((float)n * sum_xy - sum_x * sum_y) / denom;
    }
}

/* Sprint 6 US-17: Emotional contagion implementation. */
hu_error_t hu_emotional_apply_contagion(hu_emotional_cognition_t *self,
                                        hu_emotion_tag_t partner_emotion, float partner_intensity,
                                        float contagion_fraction) {
    if (!self)
        return HU_ERR_INVALID_ARGUMENT;

    /* Default fraction if caller passes 0 or negative */
    if (contagion_fraction <= 0.0f)
        contagion_fraction = 0.3f;

    /* Map partner emotion to a valence influence */
    float partner_valence;
    switch (partner_emotion) {
    case HU_EMOTION_SADNESS:
        partner_valence = -0.7f;
        break;
    case HU_EMOTION_ANGER:
        partner_valence = -0.5f;
        break;
    case HU_EMOTION_FEAR:
        partner_valence = -0.5f;
        break;
    case HU_EMOTION_ANXIETY:
        partner_valence = -0.5f;
        break;
    case HU_EMOTION_FRUSTRATION:
        partner_valence = -0.5f;
        break;
    case HU_EMOTION_JOY:
        partner_valence = 0.7f;
        break;
    case HU_EMOTION_EXCITEMENT:
        partner_valence = 0.7f;
        break;
    case HU_EMOTION_SURPRISE:
        partner_valence = 0.3f;
        break;
    case HU_EMOTION_NEUTRAL:
    default:
        partner_valence = 0.0f;
        break;
    }

    /* Apply contagion delta to valence */
    float delta_valence = partner_valence * partner_intensity * contagion_fraction;
    float new_valence = self->state.valence + delta_valence;
    if (new_valence > 1.0f)
        new_valence = 1.0f;
    if (new_valence < -1.0f)
        new_valence = -1.0f;
    self->state.valence = new_valence;

    /* Arousal (intensity proxy) rises with partner energy */
    float delta_arousal = partner_intensity * contagion_fraction * 0.5f;
    float new_arousal = self->state.intensity + delta_arousal;
    if (new_arousal > 1.0f)
        new_arousal = 1.0f;
    if (new_arousal < 0.0f)
        new_arousal = 0.0f;
    self->state.intensity = new_arousal;

    return HU_OK;
}

hu_error_t hu_emotional_cognition_build_prompt(hu_allocator_t *alloc,
                                               const hu_emotional_cognition_t *ec, char **out,
                                               size_t *out_len) {
    if (!alloc || !ec || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    /* Skip if no meaningful emotional signal */
    if (ec->source_mask == 0 || ec->state.intensity < EMOTIONAL_MIN_PROMPT_INTENSITY)
        return HU_OK;

    const char *dominant = ec->state.dominant_emotion ? ec->state.dominant_emotion : "neutral";

    char buf[2048];
    size_t pos = 0;
    pos = hu_buf_appendf(buf, sizeof(buf), pos, "## Emotional Context\n\n");
    pos = hu_buf_appendf(buf, sizeof(buf), pos,
                         "- Dominant emotion: **%s** (valence: %.2f, intensity: %.2f)\n", dominant,
                         (double)ec->state.valence, (double)ec->state.intensity);

    if (ec->secondary_emotion != HU_EMOTION_NEUTRAL) {
        pos = hu_buf_appendf(buf, sizeof(buf), pos, "- Secondary: %s\n",
                             emotion_tag_to_label(ec->secondary_emotion));
    }

    if (ec->confidence > 0.0f) {
        pos = hu_buf_appendf(buf, sizeof(buf), pos, "- Confidence: %.0f%%\n",
                             (double)(ec->confidence * 100.0f));
    }

    if (ec->valence_count >= 3) {
        const char *trend = "stable";
        if (ec->trajectory_slope > EMOTIONAL_TRAJECTORY_SLOPE_EPSILON)
            trend = "improving";
        else if (ec->trajectory_slope < -EMOTIONAL_TRAJECTORY_SLOPE_EPSILON)
            trend = "declining";
        pos = hu_buf_appendf(buf, sizeof(buf), pos, "- Trajectory: %s\n", trend);
    }

    if (ec->needs_empathy_boost) {
        pos = hu_buf_appendf(buf, sizeof(buf), pos,
                             "\n**Priority: Respond with empathy and active listening. "
                             "Acknowledge their emotional state before addressing content.**\n");
    }
    if (ec->escalation_detected) {
        pos = hu_buf_appendf(
            buf, sizeof(buf), pos,
            "\n**Warning: Emotional escalation detected. "
            "Use de-escalation techniques: validate, lower intensity, offer support.**\n");
    }

    pos = hu_buf_appendf(buf, sizeof(buf), pos, "\n");

    size_t len = (size_t)pos;
    char *result = alloc->alloc(alloc->ctx, len + 1);
    if (!result)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(result, buf, len);
    result[len] = '\0';
    *out = result;
    *out_len = len;
    return HU_OK;
}
