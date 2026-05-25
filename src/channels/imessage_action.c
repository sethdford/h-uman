#include "human/channels/imessage_action.h"
#include <math.h>
#include <stddef.h>

/* Per-fact threading nudges, expressed as additive log-odds.
 * Tuned for a default thread_affinity=0.3 to land the global thread-rate
 * near 25-30% on representative fact distributions (AC-2). */
static float thread_logodds(const hu_reply_style_facts_t *f) {
    float l = logf(f->persona_thread_affinity / (1.0f - f->persona_thread_affinity + 1e-6f));

    /* Staleness — older parents pull stronger thread. */
    if (f->seconds_since_parent > 600)
        l += 1.2f;
    else if (f->seconds_since_parent > 120)
        l += 0.4f;
    else if (f->seconds_since_parent < 10)
        l -= 0.8f; /* fresh, no need */

    /* Position — message scrolled off the active view. */
    if (f->parent_position_from_bottom >= 5)
        l += 0.8f;
    if (f->parent_position_from_bottom >= 10)
        l += 0.6f;

    /* Pending questions — threading disambiguates which one we answer. */
    if (f->pending_questions_in_window >= 2)
        l += 0.7f;
    if (f->pending_questions_in_window >= 4)
        l += 0.5f;

    /* Soft mirror — they thread, we thread (but never always). */
    if (f->other_threaded_replies_recent >= 2)
        l += 0.6f;
    if (f->other_threaded_replies_recent >= 5)
        l += 0.4f;

    /* Density — rapid-fire suppresses threading, but softly.
     * Seth still occasionally threads in fast chat when content warrants. */
    if (f->conv_density_msgs_per_min > 6.0f)
        l -= 0.5f;
    if (f->conv_density_msgs_per_min > 12.0f)
        l -= 0.8f;

    /* Question handling. */
    if (f->parent_was_a_question)
        l += 0.3f;

    /* Formality — more formal personas thread more. */
    l += (f->persona_formality - 0.5f) * 0.6f;

    return l;
}

static float sigmoid(float x) {
    return 1.0f / (1.0f + expf(-x));
}

hu_reply_style_scores_t hu_imessage_score_reply_style(const hu_reply_style_facts_t *f) {
    hu_reply_style_scores_t s = {0};
    float p_thread = sigmoid(thread_logodds(f));

    /* Tapback-only probability: low for substantive replies, higher for
     * casual confirmations. Hard-zeroed by emotional protection (AC-3). */
    float p_tap = 0.15f;
    if (f->parent_was_a_question)
        p_tap = 0.02f; /* questions deserve words */
    if (f->conv_density_msgs_per_min > 8.0f)
        p_tap += 0.10f; /* casual chat */
    if (f->parent_emotional_intensity >= HU_EMOTION_THRESHOLD_MEDIUM)
        p_tap = 0.0f;

    float p_tap_plus = 0.05f; /* rare; used for emotional acknowledgment */
    if (f->parent_emotional_intensity >= HU_EMOTION_THRESHOLD_MEDIUM)
        p_tap_plus = 0.20f;

    /* Normalize tapback-bearing masses out of (1 - p_thread). */
    float p_remaining = 1.0f - p_thread;
    float tap_share = p_tap + p_tap_plus;
    if (tap_share > p_remaining)
        tap_share = p_remaining;
    float p_flat = p_remaining - tap_share;
    if (tap_share > 0) {
        float scale =
            (p_remaining > 0 && (p_tap + p_tap_plus) > 0) ? tap_share / (p_tap + p_tap_plus) : 0;
        p_tap *= scale;
        p_tap_plus *= scale;
    }

    s.p_thread = p_thread;
    s.p_tapback = p_tap;
    s.p_flat = p_flat;
    s.p_tapback_plus_flat = p_tap_plus;
    return s;
}

hu_reply_style_t hu_imessage_choose_reply_style(const hu_reply_style_facts_t *facts,
                                                uint64_t rng_seed) {
    hu_reply_style_scores_t s = hu_imessage_score_reply_style(facts);
    /* Convert seed to [0,1) via xorshift64*. */
    uint64_t x = rng_seed ? rng_seed : 0x9E3779B97F4A7C15ULL;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    float r = (float)((x * 0x2545F4914F6CDD1DULL) >> 11) / (float)(1ULL << 53);

    /* Cumulative draw. Order: THREAD, TAPBACK, TAPBACK_PLUS_FLAT, FLAT. */
    float c = 0;
    c += s.p_thread;
    if (r < c)
        return HU_REPLY_STYLE_THREADED;
    c += s.p_tapback;
    if (r < c)
        return HU_REPLY_STYLE_TAPBACK;
    c += s.p_tapback_plus_flat;
    if (r < c)
        return HU_REPLY_STYLE_TAPBACK_PLUS_FLAT;
    return HU_REPLY_STYLE_FLAT;
}
