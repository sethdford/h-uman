#include "human/cognition/metacognition.h"
#include "human/core/string.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

void hu_metacog_settings_default(hu_metacog_settings_t *out) {
    if (!out)
        return;
    memset(out, 0, sizeof(*out));
    out->enabled = true;
    out->confidence_threshold = 0.3f;
    out->coherence_threshold = 0.2f;
    out->repetition_threshold = 0.6f;
    out->max_reflects = 2;
    out->max_regen = 1;
    out->hysteresis_min = 2;
    out->use_calibrated_risk = true;
    out->risk_high_threshold = 0.62f;
    out->w_low_confidence = 0.2f;
    out->w_low_coherence = 0.22f;
    out->w_repetition = 0.18f;
    out->w_stuck = 0.15f;
    out->w_low_satisfaction = 0.12f;
    out->w_low_trajectory = 0.13f;
}

void hu_metacognition_init(hu_metacognition_t *mc) {
    if (!mc)
        return;
    memset(mc, 0, sizeof(*mc));
    hu_metacog_settings_default(&mc->cfg);
    mc->difficulty = HU_METACOG_DIFFICULTY_EASY;
}

void hu_metacognition_apply_config(hu_metacognition_t *mc, const hu_metacog_settings_t *settings) {
    if (!mc || !settings)
        return;
    mc->cfg = *settings;
}

static float clamp01(float x) {
    if (x < 0.0f)
        return 0.0f;
    if (x > 1.0f)
        return 1.0f;
    return x;
}

float hu_metacog_calibrated_risk(const hu_metacognition_t *mc,
                                 const hu_metacognition_signal_t *signal) {
    if (!mc || !signal)
        return 0.0f;
    const hu_metacog_settings_t *c = &mc->cfg;
    float f_conf = 1.0f - clamp01(signal->confidence);
    float f_coh = 1.0f - clamp01(signal->coherence);
    float f_rep = clamp01(signal->repetition);
    float f_stuck = clamp01(signal->stuck_score);
    float f_sat = 1.0f - clamp01(signal->satisfaction_proxy);
    float f_traj = 1.0f - clamp01(signal->trajectory_confidence);

    float wsum = c->w_low_confidence + c->w_low_coherence + c->w_repetition + c->w_stuck +
                 c->w_low_satisfaction + c->w_low_trajectory;
    if (wsum < 1e-6f)
        return 0.0f;

    float num = c->w_low_confidence * f_conf + c->w_low_coherence * f_coh +
                c->w_repetition * f_rep + c->w_stuck * f_stuck + c->w_low_satisfaction * f_sat +
                c->w_low_trajectory * f_traj;
    float r = num / wsum;
    if (mc->difficulty == HU_METACOG_DIFFICULTY_HARD)
        r += 0.04f;
    else if (mc->difficulty == HU_METACOG_DIFFICULTY_MEDIUM)
        r += 0.02f;
    return clamp01(r);
}

hu_metacog_difficulty_t hu_metacog_estimate_difficulty(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0)
        return HU_METACOG_DIFFICULTY_EASY;

    size_t qmarks = 0;
    bool has_fence = false;
    bool has_math = false;
    size_t words = 0;
    bool in_word = false;

    for (size_t i = 0; i < msg_len; i++) {
        unsigned char c = (unsigned char)msg[i];
        if (c == '?')
            qmarks++;
        if (i + 2 < msg_len && msg[i] == '`' && msg[i + 1] == '`' && msg[i + 2] == '`')
            has_fence = true;
        if (c == '=' || c == '^' || c == '\\')
            has_math = true;
        if (isalpha(c)) {
            if (!in_word) {
                words++;
                in_word = true;
            }
        } else {
            in_word = false;
        }
    }

    int score = 0;
    if (msg_len > 400)
        score += 2;
    else if (msg_len > 180)
        score += 1;
    if (qmarks >= 2)
        score += 2;
    else if (qmarks == 1)
        score += 1;
    if (words > 80)
        score += 2;
    else if (words > 40)
        score += 1;
    if (has_fence)
        score += 2;
    if (has_math)
        score += 1;

    if (score >= 5)
        return HU_METACOG_DIFFICULTY_HARD;
    if (score >= 2)
        return HU_METACOG_DIFFICULTY_MEDIUM;
    return HU_METACOG_DIFFICULTY_EASY;
}

const char *hu_metacog_difficulty_name(hu_metacog_difficulty_t d) {
    switch (d) {
    case HU_METACOG_DIFFICULTY_EASY:
        return "easy";
    case HU_METACOG_DIFFICULTY_MEDIUM:
        return "medium";
    case HU_METACOG_DIFFICULTY_HARD:
        return "hard";
    default:
        return "unknown";
    }
}

static float ring_mean_repetition(const hu_metacognition_t *mc, size_t last_n) {
    if (!mc || mc->signal_count == 0 || last_n == 0)
        return 0.0f;
    size_t n = mc->signal_count < last_n ? mc->signal_count : last_n;
    size_t idx = (mc->signal_idx + HU_METACOG_SIGNAL_RING_SIZE - 1) % HU_METACOG_SIGNAL_RING_SIZE;
    float sum = 0.0f;
    for (size_t k = 0; k < n; k++) {
        sum += mc->signals[idx].repetition;
        idx = (idx + HU_METACOG_SIGNAL_RING_SIZE - 1) % HU_METACOG_SIGNAL_RING_SIZE;
    }
    return sum / (float)n;
}

static float estimate_satisfaction_proxy(size_t user_len, size_t response_len) {
    float ref = (float)user_len * 4.0f;
    if (ref < 32.0f)
        ref = 32.0f;
    if (ref > 8000.0f)
        ref = 8000.0f;
    float ratio = (float)response_len / ref;
    float score = 1.0f;
    if (ratio < 0.15f)
        score = ratio / 0.15f;
    else if (ratio < 0.3f)
        score = 0.5f + 0.5f * (ratio - 0.15f) / 0.15f;
    else if (ratio > 5.0f)
        score = 0.2f;
    else if (ratio > 3.0f)
        score = 1.0f - 0.4f * (ratio - 3.0f) / 2.0f;
    if (score < 0.0f)
        score = 0.0f;
    if (score > 1.0f)
        score = 1.0f;
    return score;
}

/* Detect sycophantic agreement patterns (arXiv:2509.16533).
 * Scans for agreement-bias markers: excessive validation without substance. */
static float estimate_sycophancy(const char *response, size_t response_len) {
    if (!response || response_len == 0)
        return 0.0f;
    static const char *const SYCOPHANCY_MARKERS[] = {
        "you're absolutely right",
        "you make a great point",
        "that's a great question",
        "i completely agree",
        "you're so right",
        "absolutely!",
        "great point!",
        "you're correct",
        "i couldn't agree more",
        "exactly right",
        "that's exactly right",
        "you nailed it",
    };
    static const size_t MARKER_COUNT = sizeof(SYCOPHANCY_MARKERS) / sizeof(SYCOPHANCY_MARKERS[0]);
    size_t hits = 0;
    for (size_t m = 0; m < MARKER_COUNT; m++) {
        const char *marker = SYCOPHANCY_MARKERS[m];
        size_t mlen = strlen(marker);
        for (size_t i = 0; i + mlen <= response_len; i++) {
            bool match = true;
            for (size_t j = 0; j < mlen; j++) {
                char rc = response[i + j];
                char mc_ch = marker[j];
                if (rc >= 'A' && rc <= 'Z')
                    rc += 32;
                if (rc != mc_ch) {
                    match = false;
                    break;
                }
            }
            if (match) {
                hits++;
                break;
            }
        }
    }
    float score = (float)hits / 3.0f;
    if (score > 1.0f)
        score = 1.0f;
    return score;
}

float hu_metacog_trajectory_confidence(const hu_metacognition_t *mc) {
    if (!mc || mc->signal_count == 0)
        return 0.5f;
    const float decay = 0.7f;
    float wsum = 0.0f;
    float num = 0.0f;
    size_t n = mc->signal_count;
    size_t start = (mc->signal_idx + HU_METACOG_SIGNAL_RING_SIZE - n) % HU_METACOG_SIGNAL_RING_SIZE;
    for (size_t i = 0; i < n; i++) {
        size_t idx = (start + i) % HU_METACOG_SIGNAL_RING_SIZE;
        const hu_metacognition_signal_t *s = &mc->signals[idx];
        float comp = (s->confidence + s->coherence + s->emotional_alignment) / 3.0f;
        float w = powf(decay, (float)(n - 1 - i));
        num += comp * w;
        wsum += w;
    }
    if (wsum <= 0.0f)
        return 0.5f;
    float out = num / wsum;
    if (out < 0.0f)
        out = 0.0f;
    if (out > 1.0f)
        out = 1.0f;
    return out;
}

hu_metacog_trend_t hu_metacog_compute_trend(const hu_metacognition_t *mc) {
    hu_metacog_trend_t t;
    memset(&t, 0, sizeof(t));
    if (!mc || mc->signal_count < 2)
        return t;

    size_t n = mc->signal_count;
    size_t start = (mc->signal_idx + HU_METACOG_SIGNAL_RING_SIZE - n) % HU_METACOG_SIGNAL_RING_SIZE;
    float sum_x = 0.0f, sum_x2 = 0.0f, sum_y_c = 0.0f, sum_y_h = 0.0f, sum_xy_c = 0.0f,
          sum_xy_h = 0.0f;
    for (size_t i = 0; i < n; i++) {
        float x = (float)i;
        size_t idx = (start + i) % HU_METACOG_SIGNAL_RING_SIZE;
        float c = mc->signals[idx].confidence;
        float h = mc->signals[idx].coherence;
        sum_x += x;
        sum_x2 += x * x;
        sum_y_c += c;
        sum_y_h += h;
        sum_xy_c += x * c;
        sum_xy_h += x * h;
    }
    float denom = (float)n * sum_x2 - sum_x * sum_x;
    if (fabsf(denom) > 1e-6f) {
        t.confidence_slope = ((float)n * sum_xy_c - sum_x * sum_y_c) / denom;
        t.coherence_slope = ((float)n * sum_xy_h - sum_x * sum_y_h) / denom;
    }
    t.is_degrading = (t.confidence_slope < -0.04f || t.coherence_slope < -0.04f);
    return t;
}

static float estimate_confidence(const char *text, size_t len) {
    if (!text || len == 0)
        return 0.5f;

    static const char *hedges[] = {"maybe",    "perhaps",   "possibly", "might",     "could be",
                                   "i think",  "i believe", "not sure", "uncertain", "it seems",
                                   "arguably", "however",   "although", NULL};

    size_t hedge_count = 0;
    size_t word_count = 0;
    bool in_word = false;

    for (size_t i = 0; i < len; i++) {
        if (isalpha((unsigned char)text[i])) {
            if (!in_word) {
                word_count++;
                in_word = true;
            }
        } else {
            in_word = false;
        }
    }
    if (word_count == 0)
        return 0.5f;

    char lower[4096];
    size_t copy_len = len < sizeof(lower) - 1 ? len : sizeof(lower) - 1;
    for (size_t i = 0; i < copy_len; i++)
        lower[i] = (char)tolower((unsigned char)text[i]);
    lower[copy_len] = '\0';

    for (size_t i = 0; hedges[i]; i++) {
        const char *p = lower;
        while ((p = strstr(p, hedges[i])) != NULL) {
            hedge_count++;
            p += strlen(hedges[i]);
        }
    }

    float hedge_ratio = (float)hedge_count / (float)word_count;
    float conf = 1.0f - hedge_ratio * 10.0f;
    if (conf < 0.0f)
        conf = 0.0f;
    if (conf > 1.0f)
        conf = 1.0f;
    return conf;
}

static float estimate_coherence(const char *query, size_t qlen, const char *response, size_t rlen) {
    if (!query || qlen == 0 || !response || rlen == 0)
        return 0.0f;

    char q_lower[2048];
    size_t ql = qlen < sizeof(q_lower) - 1 ? qlen : sizeof(q_lower) - 1;
    for (size_t i = 0; i < ql; i++)
        q_lower[i] = (char)tolower((unsigned char)query[i]);
    q_lower[ql] = '\0';

    char r_lower[4096];
    size_t rl = rlen < sizeof(r_lower) - 1 ? rlen : sizeof(r_lower) - 1;
    for (size_t i = 0; i < rl; i++)
        r_lower[i] = (char)tolower((unsigned char)response[i]);
    r_lower[rl] = '\0';

    size_t matches = 0, total = 0;
    const char *p = q_lower;
    while (*p) {
        while (*p && !isalpha((unsigned char)*p))
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && isalpha((unsigned char)*p))
            p++;
        size_t wlen = (size_t)(p - start);
        if (wlen < 3)
            continue;
        total++;

        char word[64];
        if (wlen >= sizeof(word))
            wlen = sizeof(word) - 1;
        memcpy(word, start, wlen);
        word[wlen] = '\0';

        if (strstr(r_lower, word))
            matches++;
    }

    if (total == 0)
        return 0.5f;
    return (float)matches / (float)total;
}

static float estimate_repetition(const char *current, size_t clen, const char *prev, size_t plen) {
    if (!current || clen == 0 || !prev || plen == 0)
        return 0.0f;

    if (clen < 4 || plen < 4)
        return 0.0f;

    size_t max_check = clen < 2000 ? clen : 2000;
    size_t matches = 0;
    size_t total = 0;

    for (size_t i = 0; i + 4 <= max_check; i += 4) {
        total++;
        for (size_t j = 0; j + 4 <= plen; j++) {
            if (memcmp(current + i, prev + j, 4) == 0) {
                matches++;
                break;
            }
        }
    }

    if (total == 0)
        return 0.0f;
    return (float)matches / (float)total;
}

hu_metacognition_signal_t
hu_metacognition_monitor(const char *user_query, size_t user_query_len, const char *response,
                         size_t response_len, const char *prev_response, size_t prev_response_len,
                         float emotional_confidence, uint64_t input_tokens, uint64_t output_tokens,
                         hu_metacognition_t *mc_opt) {

    hu_metacognition_signal_t s;
    memset(&s, 0, sizeof(s));

    s.confidence = estimate_confidence(response, response_len);
    s.coherence = estimate_coherence(user_query, user_query_len, response, response_len);
    s.repetition = estimate_repetition(response, response_len, prev_response, prev_response_len);
    s.emotional_alignment = emotional_confidence;

    if (input_tokens > 0)
        s.token_efficiency = (float)output_tokens / (float)input_tokens;
    else
        s.token_efficiency = 0.0f;

    float roll = mc_opt ? ring_mean_repetition(mc_opt, 3) : 0.0f;
    s.stuck_score = s.repetition > roll ? s.repetition : (s.repetition + roll) * 0.5f;
    if (s.stuck_score > 1.0f)
        s.stuck_score = 1.0f;

    s.satisfaction_proxy = estimate_satisfaction_proxy(user_query_len, response_len);
    s.trajectory_confidence = 0.0f;
    s.sycophancy_score = estimate_sycophancy(response, response_len);
    return s;
}

static float effective_confidence_threshold(const hu_metacognition_t *mc) {
    float t = mc->cfg.confidence_threshold;
    if (mc->difficulty == HU_METACOG_DIFFICULTY_HARD)
        t *= 0.67f;
    else if (mc->difficulty == HU_METACOG_DIFFICULTY_MEDIUM)
        t *= 0.85f;
    if (t < 0.05f)
        t = 0.05f;
    return t;
}

static hu_metacog_action_t raw_plan(const hu_metacognition_t *mc,
                                    const hu_metacognition_signal_t *signal) {
    if (signal->repetition > mc->cfg.repetition_threshold ||
        signal->stuck_score > mc->cfg.repetition_threshold)
        return HU_METACOG_ACTION_SIMPLIFY;

    if (signal->coherence < mc->cfg.coherence_threshold)
        return HU_METACOG_ACTION_SWITCH_STRATEGY;

    float ct = effective_confidence_threshold(mc);
    if (signal->confidence < ct) {
        if (mc->reflect_count < mc->cfg.max_reflects)
            return HU_METACOG_ACTION_REFLECT;
        return HU_METACOG_ACTION_CLARIFY;
    }

    if (signal->token_efficiency > 2.0f)
        return HU_METACOG_ACTION_SIMPLIFY;

    if (signal->sycophancy_score > 0.6f) {
        if (mc->reflect_count < mc->cfg.max_reflects)
            return HU_METACOG_ACTION_REFLECT;
    }

    if (signal->trajectory_confidence < 0.25f)
        return HU_METACOG_ACTION_SWITCH_STRATEGY;

    return HU_METACOG_ACTION_NONE;
}

static bool is_immediate_simplify(hu_metacog_action_t raw, const hu_metacognition_signal_t *signal,
                                  const hu_metacognition_t *mc) {
    if (raw != HU_METACOG_ACTION_SIMPLIFY)
        return false;
    if (signal->repetition > mc->cfg.repetition_threshold ||
        signal->stuck_score > mc->cfg.repetition_threshold)
        return true;
    if (signal->token_efficiency > 2.0f)
        return true;
    return false;
}

hu_metacog_action_t hu_metacognition_plan_action(hu_metacognition_t *mc,
                                                 hu_metacognition_signal_t *signal) {
    if (!mc || !signal)
        return HU_METACOG_ACTION_NONE;

    mc->last_suppressed_hysteresis = false;
    signal->trajectory_confidence = hu_metacog_trajectory_confidence(mc);
    hu_metacog_trend_t trend = hu_metacog_compute_trend(mc);
    uint32_t hyst_needed = mc->cfg.hysteresis_min;
    if (trend.is_degrading && signal->trajectory_confidence < 0.4f && hyst_needed > 1)
        hyst_needed--;

    float risk = hu_metacog_calibrated_risk(mc, signal);
    if (mc->cfg.use_calibrated_risk && risk >= mc->cfg.risk_high_threshold && hyst_needed > 1)
        hyst_needed--;

    hu_metacog_action_t raw = raw_plan(mc, signal);

    if (raw == HU_METACOG_ACTION_NONE) {
        mc->consecutive_bad_count = 0;
        hu_metacognition_record_signal(mc, signal);
        mc->last_action = HU_METACOG_ACTION_NONE;
        return HU_METACOG_ACTION_NONE;
    }

    if (is_immediate_simplify(raw, signal, mc)) {
        mc->consecutive_bad_count = 0;
        hu_metacognition_record_signal(mc, signal);
        mc->last_action = HU_METACOG_ACTION_SIMPLIFY;
        return HU_METACOG_ACTION_SIMPLIFY;
    }

    mc->consecutive_bad_count++;
    if (mc->consecutive_bad_count < hyst_needed) {
        hu_metacognition_record_signal(mc, signal);
        mc->last_action = HU_METACOG_ACTION_NONE;
        mc->last_suppressed_hysteresis = true;
        return HU_METACOG_ACTION_NONE;
    }

    mc->consecutive_bad_count = 0;
    hu_metacognition_record_signal(mc, signal);

    if (raw == HU_METACOG_ACTION_REFLECT && mc->reflect_count < mc->cfg.max_reflects) {
        mc->reflect_count++;
        mc->last_action = HU_METACOG_ACTION_REFLECT;
        return HU_METACOG_ACTION_REFLECT;
    }
    if (raw == HU_METACOG_ACTION_REFLECT) {
        mc->last_action = HU_METACOG_ACTION_CLARIFY;
        return HU_METACOG_ACTION_CLARIFY;
    }

    mc->last_action = raw;
    return raw;
}

void hu_metacognition_record_signal(hu_metacognition_t *mc,
                                    const hu_metacognition_signal_t *signal) {
    if (!mc || !signal)
        return;
    mc->signals[mc->signal_idx] = *signal;
    mc->signal_idx = (mc->signal_idx + 1) % HU_METACOG_SIGNAL_RING_SIZE;
    if (mc->signal_count < HU_METACOG_SIGNAL_RING_SIZE)
        mc->signal_count++;
}

hu_error_t hu_metacognition_apply(hu_metacog_action_t action, char *prompt_buf, size_t prompt_cap,
                                  size_t *prompt_len_out) {
    if (!prompt_buf || prompt_cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (prompt_len_out)
        *prompt_len_out = 0;

    const char *text = NULL;
    switch (action) {
    case HU_METACOG_ACTION_REFLECT:
        text = "Reconsider your response carefully. Be more specific and direct.";
        break;
    case HU_METACOG_ACTION_CLARIFY:
        text = "Ask the user a clarifying question before answering.";
        break;
    case HU_METACOG_ACTION_SWITCH_STRATEGY:
        text = "Try a completely different approach to this problem.";
        break;
    case HU_METACOG_ACTION_SIMPLIFY:
        text = "Be concise. Remove unnecessary detail.";
        break;
    case HU_METACOG_ACTION_DEEPEN:
        text = "Provide a deeper, more thorough answer with explicit reasoning steps.";
        break;
    default:
        return HU_ERR_INVALID_ARGUMENT;
    }

    size_t tlen = strlen(text);
    if (tlen + 1 > prompt_cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(prompt_buf, text, tlen + 1);
    if (prompt_len_out)
        *prompt_len_out = tlen;
    return HU_OK;
}

float hu_metacog_label_from_followup(const char *followup, size_t followup_len) {
    if (!followup || followup_len == 0)
        return 0.0f;

    static const char *neg[] = {
        "not what i", "that's wrong",   "that is wrong", "incorrect",   "try again", "no that's",
        "nope",       "doesn't answer", "didn't answer", "not helpful", NULL};
    static const char *pos[] = {"thanks",  "thank you",    "perfect",     "great",
                                "exactly", "got it",       "makes sense", "appreciate",
                                "helpful", "works for me", NULL};

    int neg_hits = 0, pos_hits = 0;
    for (size_t i = 0; neg[i]; i++) {
        if (hu_str_contains_ci_cstr(followup, followup_len, neg[i]))
            neg_hits++;
    }
    for (size_t i = 0; pos[i]; i++) {
        if (hu_str_contains_ci_cstr(followup, followup_len, pos[i]))
            pos_hits++;
    }

    if (neg_hits > 0 && pos_hits > 0)
        return 0.0f;
    if (neg_hits > 0)
        return -0.8f;
    if (pos_hits > 0)
        return 0.8f;
    return 0.0f;
}

const char *hu_metacog_action_name(hu_metacog_action_t action) {
    switch (action) {
    case HU_METACOG_ACTION_NONE:
        return "none";
    case HU_METACOG_ACTION_REFLECT:
        return "reflect";
    case HU_METACOG_ACTION_DEEPEN:
        return "deepen";
    case HU_METACOG_ACTION_SIMPLIFY:
        return "simplify";
    case HU_METACOG_ACTION_CLARIFY:
        return "clarify";
    case HU_METACOG_ACTION_SWITCH_STRATEGY:
        return "switch_strategy";
    default:
        return "unknown";
    }
}
