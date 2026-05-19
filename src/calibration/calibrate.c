#include "human/calibration.h"
#include "human/core/json.h"
#include "human/core/string.h"
#include "human/json_util.h"
#include "human/memory/personal_model.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#if !(defined(HU_IS_TEST) && HU_IS_TEST)
static void hu_calib_weighted_median_reply(const hu_timing_report_t *t, double *out_med,
                                           uint32_t *out_samples) {
    double sum = 0.0;
    uint32_t n = 0;
    for (int b = 0; b < HU_CALIB_TOD_BUCKET_COUNT; b++) {
        sum += t->by_tod[b].p50_sec * (double)t->by_tod[b].sample_count;
        n += t->by_tod[b].sample_count;
    }
    if (n == 0) {
        *out_med = 300.0;
        *out_samples = 0;
        return;
    }
    *out_med = sum / (double)n;
    *out_samples = n;
}

static const char *hu_calib_tempo_label(double median_sec) {
    if (median_sec < 60.0)
        return "within_a_minute";
    if (median_sec < 900.0)
        return "within_minutes";
    if (median_sec < 7200.0)
        return "within_an_hour_or_two";
    return "often_delayed";
}

static const char *hu_calib_emoji_label(double per_msg) {
    if (per_msg < 0.05)
        return "low";
    if (per_msg < 0.28)
        return "moderate";
    return "high";
}

static const char *hu_calib_formality_label(const hu_style_report_t *s) {
    if (s->question_per_message > 0.22 && s->exclamation_per_message > 0.15)
        return "casual";
    if (s->avg_message_length > 90.0)
        return "formal";
    if (s->avg_message_length < 28.0)
        return "casual";
    return "adaptive";
}

static hu_error_t hu_calib_build_recommendations_json(hu_allocator_t *alloc,
                                                      const hu_timing_report_t *timing,
                                                      const hu_style_report_t *style,
                                                      const char *channel_name, char **out_json) {
    if (!alloc || !timing || !style || !out_json)
        return HU_ERR_INVALID_ARGUMENT;
    *out_json = NULL;
    const char *ch = channel_name ? channel_name : "auto";

    double wmed = 0.0;
    uint32_t tsamp = 0;
    hu_calib_weighted_median_reply(timing, &wmed, &tsamp);
    const char *tempo = hu_calib_tempo_label(wmed);
    const char *emoji = hu_calib_emoji_label(style->emoji_per_message);
    const char *formality = hu_calib_formality_label(style);

    char avg_len[32];
    int an = snprintf(avg_len, sizeof(avg_len), "%.0f", style->avg_message_length);
    if (an < 0 || (size_t)an >= sizeof(avg_len))
        return HU_ERR_INTERNAL;

    hu_json_buf_t buf;
    hu_error_t err = hu_json_buf_init(&buf, alloc);
    if (err != HU_OK)
        return err;

    err = hu_json_buf_append_raw(&buf, "{", 1);
    if (err != HU_OK)
        goto fail;

    err = hu_json_buf_append_raw(&buf, "\"recommended_overlay\":{", 23);
    if (err != HU_OK)
        goto fail;
    err = hu_json_util_append_key_value(&buf, "channel", ch);
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err != HU_OK)
        goto fail;
    err = hu_json_util_append_key_value(&buf, "formality", formality);
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err != HU_OK)
        goto fail;
    err = hu_json_util_append_key_value(&buf, "avg_length", avg_len);
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err != HU_OK)
        goto fail;
    err = hu_json_util_append_key_value(&buf, "emoji_usage", emoji);
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, "}", 1);
    if (err != HU_OK)
        goto fail;

    err = hu_json_buf_append_raw(&buf, ",\"recommended_voice_rhythm\":{", 29);
    if (err != HU_OK)
        goto fail;
    err = hu_json_util_append_key_value(&buf, "response_tempo", tempo);
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, "}", 1);
    if (err != HU_OK)
        goto fail;

    err = hu_json_buf_append_raw(&buf, ",\"calibration_meta\":{", 21);
    if (err != HU_OK)
        goto fail;
    err = hu_json_util_append_key_int(&buf, "timing_weighted_median_reply_sec",
                                      (int64_t)(wmed + 0.5));
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err != HU_OK)
        goto fail;
    err = hu_json_util_append_key_int(&buf, "timing_samples", (int64_t)tsamp);
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err != HU_OK)
        goto fail;
    err = hu_json_util_append_key_int(&buf, "style_messages_analyzed",
                                      (int64_t)style->messages_analyzed);
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err != HU_OK)
        goto fail;
    {
        char vr[24];
        int vn = snprintf(vr, sizeof(vr), "%.3f", style->vocabulary_richness);
        if (vn < 0 || (size_t)vn >= sizeof(vr)) {
            err = HU_ERR_INTERNAL;
            goto fail;
        }
        err = hu_json_util_append_key_value(&buf, "vocabulary_richness", vr);
        if (err != HU_OK)
            goto fail;
    }
    err = hu_json_buf_append_raw(&buf, "}", 1);
    if (err != HU_OK)
        goto fail;

    if (style->opening_count > 0) {
        err = hu_json_buf_append_raw(&buf, ",\"sample_opening_phrases\":[", 27);
        if (err != HU_OK)
            goto fail;
        for (size_t i = 0; i < style->opening_count; i++) {
            if (i > 0) {
                err = hu_json_buf_append_raw(&buf, ",", 1);
                if (err != HU_OK)
                    goto fail;
            }
            err = hu_json_util_append_string(&buf, style->opening_phrases[i].phrase);
            if (err != HU_OK)
                goto fail;
        }
        err = hu_json_buf_append_raw(&buf, "]", 1);
        if (err != HU_OK)
            goto fail;
    }

    if (style->closing_count > 0) {
        err = hu_json_buf_append_raw(&buf, ",\"sample_closing_phrases\":[", 27);
        if (err != HU_OK)
            goto fail;
        for (size_t i = 0; i < style->closing_count; i++) {
            if (i > 0) {
                err = hu_json_buf_append_raw(&buf, ",", 1);
                if (err != HU_OK)
                    goto fail;
            }
            err = hu_json_util_append_string(&buf, style->closing_phrases[i].phrase);
            if (err != HU_OK)
                goto fail;
        }
        err = hu_json_buf_append_raw(&buf, "]", 1);
        if (err != HU_OK)
            goto fail;
    }

    err = hu_json_buf_append_raw(&buf, "}", 1);
    if (err != HU_OK)
        goto fail;

    *out_json = hu_strdup(alloc, buf.ptr);
    if (!*out_json)
        err = HU_ERR_OUT_OF_MEMORY;
fail:
    hu_json_buf_free(&buf);
    if (err != HU_OK)
        *out_json = NULL;
    return err;
}
#endif /* !(defined(HU_IS_TEST) && HU_IS_TEST) */

hu_error_t hu_calibrate(hu_allocator_t *alloc, const char *db_path, const char *contact_filter,
                        const char *channel_name, char **out_recommendations) {
    if (!alloc || !out_recommendations)
        return HU_ERR_INVALID_ARGUMENT;
    *out_recommendations = NULL;
    const char *ch = channel_name ? channel_name : "auto";

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)db_path;
    (void)contact_filter;
    char mock_buf[1024];
    int n = snprintf(
        mock_buf, sizeof(mock_buf),
        "{\"recommended_overlay\":{\"channel\":\"%s\",\"formality\":\"casual\","
        "\"avg_length\":\"42\",\"emoji_usage\":\"moderate\"},"
        "\"recommended_voice_rhythm\":{\"response_tempo\":\"within_minutes\"},"
        "\"calibration_meta\":{\"timing_weighted_median_reply_sec\":180,"
        "\"timing_samples\":40,\"style_messages_analyzed\":120,\"vocabulary_richness\":\"0.620\"}}",
        ch);
    if (n < 0 || (size_t)n >= sizeof(mock_buf))
        return HU_ERR_INVALID_ARGUMENT;
    *out_recommendations = hu_strdup(alloc, mock_buf);
    return *out_recommendations ? HU_OK : HU_ERR_OUT_OF_MEMORY;
#else

    hu_timing_report_t timing;
    hu_style_report_t style;
    memset(&timing, 0, sizeof(timing));
    memset(&style, 0, sizeof(style));

    hu_error_t err = hu_calibration_analyze_timing(alloc, db_path, contact_filter, &timing);
    if (err != HU_OK)
        return err;

    err = hu_calibration_analyze_style(alloc, db_path, contact_filter, &style);
    if (err != HU_OK) {
        hu_timing_report_deinit(alloc, &timing);
        return err;
    }

    err = hu_calib_build_recommendations_json(alloc, &timing, &style, ch, out_recommendations);
    hu_timing_report_deinit(alloc, &timing);
    hu_style_report_deinit(alloc, &style);
    return err;
#endif
}

/* ──────────────────────────────────────────────────────────────────────────
 * Reaction signature — aggregates reaction-ingest facts in the personal
 * model into per-contact valence counts and salient topic tokens.
 * ────────────────────────────────────────────────────────────────────────── */

/* Classify a reaction predicate into a polarity. Positive predicates are
 * `reacted_with_love_to`, `reacted_with_like_to`, `laughed_at`,
 * `emphasized`, `reacted_with_emoji_to`. Negative are
 * `reacted_with_dislike_to` and `questioned`. Anything else returns 0
 * so unknown verbs (future-proofing) are silently ignored. */
static int hu_calib_reaction_polarity(const char *pred) {
    if (!pred || !*pred)
        return 0;
    if (strncmp(pred, "reacted_with_love", 17) == 0)
        return 1;
    if (strncmp(pred, "reacted_with_like", 17) == 0)
        return 1;
    if (strncmp(pred, "laughed", 7) == 0)
        return 1;
    if (strncmp(pred, "emphasized", 10) == 0)
        return 1;
    if (strncmp(pred, "reacted_with_emoji", 18) == 0)
        return 1;
    if (strncmp(pred, "reacted_with_dislike", 20) == 0)
        return -1;
    if (strncmp(pred, "questioned", 10) == 0)
        return -1;
    return 0;
}

/* Common English/chat stopwords filtered out of salient-topic extraction.
 * Kept tiny on purpose — large stopword lists are easy to over-tune. */
static bool hu_calib_is_stopword(const char *tok) {
    static const char *const STOP[] = {"the",  "and",  "for", "that", "with", "this", "from",
                                       "have", "has",  "had", "but",  "not",  "are",  "was",
                                       "were", "will", "you", "your", "they", "them", "their",
                                       "what", "when", "who", "how",  "why",  NULL};
    for (size_t i = 0; STOP[i]; i++) {
        if (strcmp(tok, STOP[i]) == 0)
            return true;
    }
    return false;
}

/* Lowercase `src` into `dst` (cap-1 chars + NUL). Returns the length
 * written (excluding the NUL). Non-alnum characters are stripped so
 * "Hiking!" → "hiking" and "well-being" → "wellbeing". */
static size_t hu_calib_lower_strip(const char *src, char *dst, size_t cap) {
    if (!src || !dst || cap == 0)
        return 0;
    size_t w = 0;
    for (size_t i = 0; src[i] && w + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        if (isalnum(c))
            dst[w++] = (char)tolower(c);
    }
    dst[w] = '\0';
    return w;
}

size_t hu_calib_reaction_signature_from_model(const struct hu_personal_model *model,
                                              hu_calib_reaction_signature_t *out) {
    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    if (!model)
        return 0;

    /* Use an internal scratch buffer of all reactors so we can sort later. */
    enum { SCRATCH_CAP = 64 };
    hu_calib_top_reactor_t scratch[SCRATCH_CAP];
    memset(scratch, 0, sizeof(scratch));
    size_t scratch_count = 0;

    /* Topic frequency table — bounded; collisions overwrite the lowest count. */
    enum { TOPIC_CAP = 64 };
    char topic_buf[TOPIC_CAP][HU_CALIB_REACTION_TOPIC_MAX];
    uint32_t topic_count[TOPIC_CAP];
    memset(topic_buf, 0, sizeof(topic_buf));
    memset(topic_count, 0, sizeof(topic_count));
    size_t topics_used = 0;

    for (size_t i = 0; i < model->fact_count; i++) {
        const hu_heuristic_fact_t *f = &model->facts[i];
        if (strcmp(f->source_hint, "reaction_ingest") != 0)
            continue;
        if (!f->subject[0])
            continue;
        int pol = hu_calib_reaction_polarity(f->predicate);
        if (pol == 0)
            continue;

        /* Find or insert reactor by subject (case-sensitive — handles are stable IDs). */
        size_t slot = scratch_count;
        for (size_t s = 0; s < scratch_count; s++) {
            if (strcmp(scratch[s].handle, f->subject) == 0) {
                slot = s;
                break;
            }
        }
        if (slot == scratch_count) {
            if (scratch_count >= SCRATCH_CAP)
                continue; /* overflow — drop rare reactors */
            strncpy(scratch[slot].handle, f->subject, sizeof(scratch[slot].handle) - 1);
            scratch[slot].handle[sizeof(scratch[slot].handle) - 1] = '\0';
            scratch_count++;
        }
        if (pol > 0)
            scratch[slot].positive_count++;
        else
            scratch[slot].negative_count++;
        if (f->last_seen_at > scratch[slot].last_observed)
            scratch[slot].last_observed = f->last_seen_at;

        /* Tokenize object string into topic candidates. */
        const char *obj = f->object;
        size_t obj_len = strlen(obj);
        char tok[HU_CALIB_REACTION_TOPIC_MAX];
        size_t tok_w = 0;
        for (size_t c = 0; c <= obj_len; c++) {
            unsigned char ch = (unsigned char)obj[c];
            bool boundary = (c == obj_len) || !isalnum(ch);
            if (!boundary) {
                if (tok_w + 1 < sizeof(tok))
                    tok[tok_w++] = (char)tolower(ch);
                continue;
            }
            tok[tok_w] = '\0';
            if (tok_w >= 4 && !hu_calib_is_stopword(tok)) {
                /* Look up token in topic_buf. */
                size_t found = topics_used;
                for (size_t t = 0; t < topics_used; t++) {
                    if (strcmp(topic_buf[t], tok) == 0) {
                        found = t;
                        break;
                    }
                }
                if (found == topics_used && topics_used < TOPIC_CAP) {
                    strncpy(topic_buf[topics_used], tok, HU_CALIB_REACTION_TOPIC_MAX - 1);
                    topic_buf[topics_used][HU_CALIB_REACTION_TOPIC_MAX - 1] = '\0';
                    topic_count[topics_used] = 0;
                    topics_used++;
                }
                if (found < TOPIC_CAP)
                    topic_count[found]++;
            }
            tok_w = 0;
        }
    }

    /* Sort scratch by total count (positive + negative) descending,
     * tie-break by last_observed descending. Simple insertion sort —
     * SCRATCH_CAP is small. */
    for (size_t i = 1; i < scratch_count; i++) {
        hu_calib_top_reactor_t key = scratch[i];
        size_t j = i;
        while (j > 0) {
            uint32_t lhs = scratch[j - 1].positive_count + scratch[j - 1].negative_count;
            uint32_t rhs = key.positive_count + key.negative_count;
            bool greater =
                (rhs > lhs) || (rhs == lhs && key.last_observed > scratch[j - 1].last_observed);
            if (!greater)
                break;
            scratch[j] = scratch[j - 1];
            j--;
        }
        scratch[j] = key;
    }

    size_t out_n = scratch_count < HU_CALIB_REACTION_TOP_REACTORS ? scratch_count
                                                                  : HU_CALIB_REACTION_TOP_REACTORS;
    for (size_t i = 0; i < out_n; i++)
        out->top_reactors[i] = scratch[i];
    out->reactor_count = out_n;

    /* Select top-N topics by frequency. Insertion sort on a parallel
     * index array since we want stable output. */
    size_t order[TOPIC_CAP];
    for (size_t i = 0; i < topics_used; i++)
        order[i] = i;
    for (size_t i = 1; i < topics_used; i++) {
        size_t key = order[i];
        size_t j = i;
        while (j > 0 && topic_count[order[j - 1]] < topic_count[key]) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = key;
    }
    size_t topic_out_n =
        topics_used < HU_CALIB_REACTION_TOPICS ? topics_used : HU_CALIB_REACTION_TOPICS;
    for (size_t i = 0; i < topic_out_n; i++) {
        strncpy(out->salient_topics[i], topic_buf[order[i]], HU_CALIB_REACTION_TOPIC_MAX - 1);
        out->salient_topics[i][HU_CALIB_REACTION_TOPIC_MAX - 1] = '\0';
    }
    out->salient_topic_count = topic_out_n;
    /* Suppress unused warning when hu_calib_lower_strip is only needed in tests. */
    (void)hu_calib_lower_strip;
    return out->reactor_count;
}

hu_error_t hu_calib_reactions_append_json(hu_json_buf_t *buf,
                                          const hu_calib_reaction_signature_t *sig) {
    if (!buf || !sig)
        return HU_ERR_INVALID_ARGUMENT;
    hu_error_t err = hu_json_util_append_key(buf, "reactions");
    if (err != HU_OK)
        return err;
    err = hu_json_buf_append_raw(buf, "{\"top_reactors\":[", 17);
    if (err != HU_OK)
        return err;
    for (size_t i = 0; i < sig->reactor_count; i++) {
        if (i > 0) {
            err = hu_json_buf_append_raw(buf, ",", 1);
            if (err != HU_OK)
                return err;
        }
        err = hu_json_buf_append_raw(buf, "{", 1);
        if (err != HU_OK)
            return err;
        err = hu_json_util_append_key_value(buf, "contact", sig->top_reactors[i].handle);
        if (err != HU_OK)
            return err;
        err = hu_json_buf_append_raw(buf, ",", 1);
        if (err != HU_OK)
            return err;
        err = hu_json_util_append_key_int(buf, "positive",
                                          (int64_t)sig->top_reactors[i].positive_count);
        if (err != HU_OK)
            return err;
        err = hu_json_buf_append_raw(buf, ",", 1);
        if (err != HU_OK)
            return err;
        err = hu_json_util_append_key_int(buf, "negative",
                                          (int64_t)sig->top_reactors[i].negative_count);
        if (err != HU_OK)
            return err;
        err = hu_json_buf_append_raw(buf, ",", 1);
        if (err != HU_OK)
            return err;
        err = hu_json_util_append_key_int(buf, "last_seen_unix",
                                          (int64_t)sig->top_reactors[i].last_observed);
        if (err != HU_OK)
            return err;
        err = hu_json_buf_append_raw(buf, "}", 1);
        if (err != HU_OK)
            return err;
    }
    err = hu_json_buf_append_raw(buf, "],\"salient_topics\":[", 20);
    if (err != HU_OK)
        return err;
    for (size_t i = 0; i < sig->salient_topic_count; i++) {
        if (i > 0) {
            err = hu_json_buf_append_raw(buf, ",", 1);
            if (err != HU_OK)
                return err;
        }
        err = hu_json_util_append_string(buf, sig->salient_topics[i]);
        if (err != HU_OK)
            return err;
    }
    err = hu_json_buf_append_raw(buf, "]}", 2);
    return err;
}

hu_error_t hu_calibrate_with_model(hu_allocator_t *alloc, const char *db_path,
                                   const char *contact_filter, const char *channel_name,
                                   const struct hu_personal_model *model,
                                   char **out_recommendations) {
    if (!alloc || !out_recommendations)
        return HU_ERR_INVALID_ARGUMENT;
    *out_recommendations = NULL;

    /* Get the base calibration JSON first, then splice in the reactions
     * object before the trailing '}'. Cheaper than re-implementing the
     * whole builder for the test-mock case. */
    char *base = NULL;
    hu_error_t err = hu_calibrate(alloc, db_path, contact_filter, channel_name, &base);
    if (err != HU_OK)
        return err;
    if (!base)
        return HU_ERR_INTERNAL;

    hu_calib_reaction_signature_t sig;
    size_t reactor_n = hu_calib_reaction_signature_from_model(model, &sig);
    if (reactor_n == 0 && sig.salient_topic_count == 0) {
        *out_recommendations = base;
        return HU_OK;
    }

    /* Strip trailing '}' from base, append ",reactions":{...}, close. */
    size_t base_len = strlen(base);
    if (base_len == 0 || base[base_len - 1] != '}') {
        *out_recommendations = base;
        return HU_OK;
    }

    hu_json_buf_t buf;
    err = hu_json_buf_init(&buf, alloc);
    if (err != HU_OK) {
        hu_str_free(alloc, base);
        return err;
    }
    /* Write everything except the trailing '}'. */
    err = hu_json_buf_append_raw(&buf, base, base_len - 1);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err == HU_OK)
        err = hu_calib_reactions_append_json(&buf, &sig);
    if (err == HU_OK)
        err = hu_json_buf_append_raw(&buf, "}", 1);
    if (err == HU_OK) {
        char *result = hu_strdup(alloc, buf.ptr);
        if (!result)
            err = HU_ERR_OUT_OF_MEMORY;
        else
            *out_recommendations = result;
    }
    hu_json_buf_free(&buf);
    hu_str_free(alloc, base);
    return err;
}
