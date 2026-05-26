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

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "human/core/error.h"

/* Build the log directory path. Returns 0 on success, -1 on overflow.
 * Honors HU_IMESSAGE_ACTION_LOG_DIR env var for test isolation. */
static int resolve_log_dir(char *out, size_t out_cap) {
    const char *env_dir = getenv("HU_IMESSAGE_ACTION_LOG_DIR");
    if (env_dir && env_dir[0]) {
        int n = snprintf(out, out_cap, "%s", env_dir);
        return (n > 0 && (size_t)n < out_cap) ? 0 : -1;
    }
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return -1;
    int n = snprintf(out, out_cap, "%s/.human/logs", home);
    return (n > 0 && (size_t)n < out_cap) ? 0 : -1;
}

static const char *style_name(hu_reply_style_t s) {
    switch (s) {
    case HU_REPLY_STYLE_FLAT:
        return "FLAT";
    case HU_REPLY_STYLE_THREADED:
        return "THREADED";
    case HU_REPLY_STYLE_TAPBACK:
        return "TAPBACK";
    case HU_REPLY_STYLE_TAPBACK_PLUS_FLAT:
        return "TAPBACK_PLUS_FLAT";
    }
    return "UNKNOWN";
}

hu_error_t hu_imessage_action_log_jsonl(const hu_imessage_action_log_t *log) {
    if (!log)
        return HU_ERR_INVALID_ARGUMENT;

    char dir[512];
    if (resolve_log_dir(dir, sizeof(dir)) != 0)
        return HU_ERR_INVALID_ARGUMENT;

    /* mkdir -p equivalent. */
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        return HU_ERR_IO;
    }

    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/imessage_action.jsonl", dir);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return HU_ERR_INVALID_ARGUMENT;

    FILE *f = fopen(path, "a");
    if (!f)
        return HU_ERR_IO;

    fprintf(f,
            "{\"ts\":%lld,\"chat\":\"%s\","
            "\"facts\":{\"sec_since_parent\":%lld,\"pos\":%d,\"pq\":%d,"
            "\"o_th\":%d,\"u_th\":%d,\"density\":%.2f,\"q\":%s,"
            "\"formality\":%.2f,\"thread_aff\":%.2f,\"emo\":%d},"
            "\"style\":\"%s\",\"result\":%d,\"tier\":\"%s\",\"elapsed_ms\":%d}\n",
            (long long)log->ts_unix, log->target_chat_id_hash ? log->target_chat_id_hash : "",
            (long long)log->facts.seconds_since_parent, log->facts.parent_position_from_bottom,
            log->facts.pending_questions_in_window, log->facts.other_threaded_replies_recent,
            log->facts.our_threaded_replies_recent, (double)log->facts.conv_density_msgs_per_min,
            log->facts.parent_was_a_question ? "true" : "false",
            (double)log->facts.persona_formality, (double)log->facts.persona_thread_affinity,
            log->facts.parent_emotional_intensity, style_name(log->style_chosen), log->send_result,
            log->tier_used ? log->tier_used : "", log->elapsed_ms);
    fclose(f);
    return HU_OK;
}
