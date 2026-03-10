#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#include "human/persona/replay.h"
#include "human/context/conversation.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define HU_REPLAY_LONG_PAUSE_SEC        1800 /* 30 min */
#define HU_REPLAY_RAPID_WINDOW_SEC      120  /* 2 min for rapid reply = engaged */
#define HU_REPLAY_HIGH_ENGAGEMENT_RATIO 2.0  /* their reply >= 2x our length */
#define HU_REPLAY_LOW_ENGAGEMENT_RATIO  0.33 /* their reply < 1/3 our length */
#define HU_REPLAY_ONE_WORD_MAX          15   /* reply <= 15 chars = one-word territory */
#define HU_REPLAY_ENERGY_EXCLAM_MIN     1    /* 1+ exclamation marks = energy */
#define HU_REPLAY_GENERIC_RESPONSE_MAX                         \
    70 /* our msg < 70 chars to long theirs = possible generic \
        */

/* Parse timestamp to time_t. Returns (time_t)-1 on failure. */
static time_t parse_timestamp(const char *ts) {
    if (!ts || !ts[0])
        return (time_t)-1;
    struct tm tm = {0};
    char *p = strptime(ts, "%Y-%m-%d %H:%M", &tm);
    if (p && *p == '\0') {
        return mktime(&tm);
    }
    tm = (struct tm){0};
    p = strptime(ts, "%H:%M", &tm);
    if (p && *p == '\0') {
        /* Assume today for time-only format */
        time_t now = time(NULL);
        struct tm *now_tm = localtime(&now);
        if (now_tm) {
            tm.tm_year = now_tm->tm_year;
            tm.tm_mon = now_tm->tm_mon;
            tm.tm_mday = now_tm->tm_mday;
            return mktime(&tm);
        }
    }
    return (time_t)-1;
}

/* Count exclamation marks in text (energy marker) */
static int count_exclamations(const char *text, size_t len) {
    int n = 0;
    for (size_t i = 0; i < len && text[i]; i++)
        if (text[i] == '!')
            n++;
    return n;
}

/* Check if text contains a question (continuation signal) */
static bool contains_question(const char *text, size_t len) {
    for (size_t i = 0; i < len && text[i]; i++)
        if (text[i] == '?')
            return true;
    return false;
}

/* Check for lengthened words (yesss, loool) — 3+ repeated same char */
static bool has_lengthened_word(const char *text, size_t len) {
    int run = 0;
    char prev = '\0';
    for (size_t i = 0; i < len && text[i]; i++) {
        char c = (char)tolower((unsigned char)text[i]);
        if (c == prev && isalpha((unsigned char)c)) {
            run++;
            if (run >= 3)
                return true;
        } else {
            run = 1;
            prev = c;
        }
    }
    return false;
}

/* Generic validation heuristic: short our-msg to substantial their-msg, no question */
static bool looks_generic_response(size_t my_len, size_t their_prev_len, const char *my_text,
                                   size_t my_text_len) {
    if (my_len >= HU_REPLAY_GENERIC_RESPONSE_MAX || their_prev_len < 25)
        return false;
    if (contains_question(my_text, my_text_len))
        return false;
    return true;
}

hu_error_t hu_replay_analyze(hu_allocator_t *alloc, const hu_channel_history_entry_t *entries,
                             size_t entry_count, uint32_t max_chars, hu_replay_result_t *out) {
    if (!alloc || !out)
        return HU_ERR_INVALID_ARGUMENT;
    if (!entries && entry_count > 0)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    if (entry_count == 0)
        return HU_OK;

    int total_quality = 0;
    size_t our_response_count = 0;
    int high_engagement_count = 0;
    int low_engagement_count = 0;
    size_t total_my_len = 0;
    size_t best_engaged_my_len = 0;
    size_t worst_engaged_my_len = 0;

    for (size_t i = 0; i < entry_count && out->insight_count < HU_REPLAY_MAX_INSIGHTS; i++) {
        if (!entries[i].from_me)
            continue;

        size_t my_len = strlen(entries[i].text);
        if (my_len == 0)
            continue;

        hu_quality_score_t q =
            hu_conversation_evaluate_quality(entries[i].text, my_len, entries, i + 1, max_chars);
        total_quality += q.total;
        our_response_count++;
        total_my_len += my_len;

        /* Compute engagement from their reply (next message) */
        if (i + 1 < entry_count && !entries[i + 1].from_me) {
            const char *their = entries[i + 1].text;
            size_t their_len = strlen(their);
            double ratio = (my_len > 0) ? (double)their_len / (double)my_len : 0.0;
            int exclam = count_exclamations(their, their_len);
            bool has_q = contains_question(their, their_len);
            bool has_lengthened = has_lengthened_word(their, their_len);

            /* Response speed from timestamps */
            double gap_sec = -1.0;
            time_t my_ts = parse_timestamp(entries[i].timestamp);
            time_t their_ts = parse_timestamp(entries[i + 1].timestamp);
            if (my_ts != (time_t)-1 && their_ts != (time_t)-1)
                gap_sec = difftime(their_ts, my_ts);
            bool rapid_reply = (gap_sec >= 0 && gap_sec < (double)HU_REPLAY_RAPID_WINDOW_SEC);

            /* High engagement: ratio >= 2, or 2+ exclamations, or question, or rapid, or lengthened
             */
            bool high = (ratio >= HU_REPLAY_HIGH_ENGAGEMENT_RATIO) ||
                        (exclam >= HU_REPLAY_ENERGY_EXCLAM_MIN) || has_q || rapid_reply ||
                        has_lengthened;

            /* Low engagement: one-word reply (<=15 chars) to our longer msg, or ratio < 1/3 */
            bool low = (their_len <= HU_REPLAY_ONE_WORD_MAX && my_len > 50) ||
                       (my_len > 0 && ratio < HU_REPLAY_LOW_ENGAGEMENT_RATIO);

            if (high && !low && out->insight_count < HU_REPLAY_MAX_INSIGHTS) {
                high_engagement_count++;
                if (best_engaged_my_len == 0 || my_len < best_engaged_my_len)
                    best_engaged_my_len = my_len;
                char obs[160];
                char rec[120];
                int no;
                if (exclam >= 1)
                    no = snprintf(obs, sizeof(obs),
                                  "Message %zu: your %zu-char message got a %.1fx reply with %d "
                                  "exclamation mark(s) — high engagement",
                                  (size_t)(i + 1), my_len, ratio, exclam);
                else
                    no = snprintf(obs, sizeof(obs),
                                  "Message %zu: your %zu-char message got a %.1fx longer reply — "
                                  "high engagement",
                                  (size_t)(i + 1), my_len, ratio);
                int nr = snprintf(rec, sizeof(rec),
                                  "Short messages (like your %zu-char one) got engaged responses — "
                                  "keep this tone and brevity",
                                  my_len);
                if (no > 0 && (size_t)no < sizeof(obs) && nr > 0 && (size_t)nr < sizeof(rec)) {
                    hu_replay_insight_t *ins = &out->insights[out->insight_count];
                    ins->observation = hu_strndup(alloc, obs, (size_t)no);
                    ins->observation_len = ins->observation ? (size_t)no : 0;
                    ins->recommendation = hu_strndup(alloc, rec, (size_t)nr);
                    ins->recommendation_len = ins->recommendation ? (size_t)nr : 0;
                    ins->score_delta = 10;
                    if (ins->observation && ins->recommendation)
                        out->insight_count++;
                    else {
                        if (ins->observation)
                            alloc->free(alloc->ctx, ins->observation, ins->observation_len + 1);
                        if (ins->recommendation)
                            alloc->free(alloc->ctx, ins->recommendation,
                                        ins->recommendation_len + 1);
                    }
                }
            } else if (low && !high && out->insight_count < HU_REPLAY_MAX_INSIGHTS) {
                low_engagement_count++;
                if (worst_engaged_my_len == 0 || my_len > worst_engaged_my_len)
                    worst_engaged_my_len = my_len;
                char obs[160];
                char rec[120];
                int no = snprintf(obs, sizeof(obs),
                                  "Message %zu: your %zu-char message got a %zu-char reply",
                                  (size_t)(i + 1), my_len, their_len);
                int nr = snprintf(rec, sizeof(rec),
                                  "Your %zu-char message got a brief reply — try shorter or more "
                                  "specific follow-ups",
                                  my_len);
                if (no > 0 && (size_t)no < sizeof(obs) && nr > 0 && (size_t)nr < sizeof(rec)) {
                    hu_replay_insight_t *ins = &out->insights[out->insight_count];
                    ins->observation = hu_strndup(alloc, obs, (size_t)no);
                    ins->observation_len = ins->observation ? (size_t)no : 0;
                    ins->recommendation = hu_strndup(alloc, rec, (size_t)nr);
                    ins->recommendation_len = ins->recommendation ? (size_t)nr : 0;
                    ins->score_delta = -10;
                    if (ins->observation && ins->recommendation)
                        out->insight_count++;
                    else {
                        if (ins->observation)
                            alloc->free(alloc->ctx, ins->observation, ins->observation_len + 1);
                        if (ins->recommendation)
                            alloc->free(alloc->ctx, ins->recommendation,
                                        ins->recommendation_len + 1);
                    }
                }
            }

            /* Long pause: gap > 30 min */
            if (gap_sec > (double)HU_REPLAY_LONG_PAUSE_SEC &&
                out->insight_count < HU_REPLAY_MAX_INSIGHTS) {
                char obs[96];
                int no = snprintf(obs, sizeof(obs),
                                  "%.0f min pause before their reply after your %zu-char message",
                                  gap_sec / 60.0, my_len);
                if (no > 0 && (size_t)no < sizeof(obs)) {
                    hu_replay_insight_t *ins = &out->insights[out->insight_count];
                    ins->observation = hu_strndup(alloc, obs, (size_t)no);
                    ins->observation_len = ins->observation ? (size_t)no : 0;
                    ins->recommendation =
                        hu_strndup(alloc, "Consider shorter or more engaging messages", 41);
                    ins->recommendation_len = ins->recommendation ? 41 : 0;
                    ins->score_delta = -5;
                    if (ins->observation && ins->recommendation)
                        out->insight_count++;
                    else {
                        if (ins->observation)
                            alloc->free(alloc->ctx, ins->observation, ins->observation_len + 1);
                        if (ins->recommendation)
                            alloc->free(alloc->ctx, ins->recommendation, 42);
                    }
                }
            }
        }

        /* Generic validation: short our-msg to long their-msg, no question */
        if (i > 0 && !entries[i - 1].from_me) {
            size_t their_prev = strlen(entries[i - 1].text);
            if (looks_generic_response(my_len, their_prev, entries[i].text, my_len) &&
                out->insight_count < HU_REPLAY_MAX_INSIGHTS) {
                char obs[96];
                int no = snprintf(obs, sizeof(obs),
                                  "Your %zu-char response to their %zu-char message lacked "
                                  "specificity or a follow-up question",
                                  my_len, their_prev);
                if (no > 0 && (size_t)no < sizeof(obs)) {
                    hu_replay_insight_t *ins = &out->insights[out->insight_count];
                    ins->observation = hu_strndup(alloc, obs, (size_t)no);
                    ins->observation_len = ins->observation ? (size_t)no : 0;
                    ins->recommendation =
                        hu_strndup(alloc, "Use more specific follow-up questions", 37);
                    ins->recommendation_len = ins->recommendation ? 37 : 0;
                    ins->score_delta = -15;
                    if (ins->observation && ins->recommendation)
                        out->insight_count++;
                    else {
                        if (ins->observation)
                            alloc->free(alloc->ctx, ins->observation, ins->observation_len + 1);
                        if (ins->recommendation)
                            alloc->free(alloc->ctx, ins->recommendation, 38);
                    }
                }
            }
        }

        /* Our response too long relative to theirs (previous message) */
        if (i > 0 && !entries[i - 1].from_me && out->insight_count < HU_REPLAY_MAX_INSIGHTS) {
            size_t their_len = strlen(entries[i - 1].text);
            if (their_len > 0 && my_len > their_len * 3 && my_len > 150) {
                char obs[96];
                int no =
                    snprintf(obs, sizeof(obs),
                             "Your %zu-char response was 3x longer than their %zu-char message",
                             my_len, their_len);
                if (no > 0 && (size_t)no < sizeof(obs)) {
                    hu_replay_insight_t *ins = &out->insights[out->insight_count];
                    ins->observation = hu_strndup(alloc, obs, (size_t)no);
                    ins->observation_len = ins->observation ? (size_t)no : 0;
                    ins->recommendation =
                        hu_strndup(alloc, "Mirror their message length more closely", 38);
                    ins->recommendation_len = ins->recommendation ? 38 : 0;
                    ins->score_delta = -8;
                    if (ins->observation && ins->recommendation)
                        out->insight_count++;
                    else {
                        if (ins->observation)
                            alloc->free(alloc->ctx, ins->observation, ins->observation_len + 1);
                        if (ins->recommendation)
                            alloc->free(alloc->ctx, ins->recommendation, 39);
                    }
                }
            }
        }
    }

    /* Overall score: average quality of our responses */
    if (our_response_count > 0)
        out->overall_score = total_quality / (int)our_response_count;
    if (out->overall_score > 100)
        out->overall_score = 100;
    if (out->overall_score < 0)
        out->overall_score = 0;

    /* Build data-rich summary */
    {
        char buf[384];
        int n;
        size_t avg_len = our_response_count > 0 ? total_my_len / our_response_count : 0;
        if (high_engagement_count > 0 && low_engagement_count == 0)
            n = snprintf(buf, sizeof(buf),
                         "Quality %d/100. %d response(s) got high engagement. "
                         "Avg message length %zu chars.",
                         out->overall_score, high_engagement_count, avg_len);
        else if (low_engagement_count > 0 && high_engagement_count == 0)
            n = snprintf(buf, sizeof(buf),
                         "Quality %d/100. %d disengaged moment(s). "
                         "Avg message length %zu chars.",
                         out->overall_score, low_engagement_count, avg_len);
        else if (high_engagement_count > 0 && low_engagement_count > 0)
            n = snprintf(buf, sizeof(buf),
                         "Quality %d/100. %d high-engagement, %d low-engagement. "
                         "Avg message length %zu chars.",
                         out->overall_score, high_engagement_count, low_engagement_count, avg_len);
        else
            n = snprintf(buf, sizeof(buf), "Quality %d/100. Avg message length %zu chars.",
                         out->overall_score, avg_len);
        if (n > 0 && (size_t)n < sizeof(buf)) {
            out->summary = hu_strndup(alloc, buf, (size_t)n);
            out->summary_len = out->summary ? (size_t)n : 0;
        }
    }

    return HU_OK;
}

void hu_replay_result_deinit(hu_replay_result_t *result, hu_allocator_t *alloc) {
    if (!result || !alloc)
        return;
    for (size_t i = 0; i < result->insight_count; i++) {
        hu_replay_insight_t *ins = &result->insights[i];
        if (ins->observation) {
            alloc->free(alloc->ctx, ins->observation, ins->observation_len + 1);
            ins->observation = NULL;
        }
        if (ins->recommendation) {
            alloc->free(alloc->ctx, ins->recommendation, ins->recommendation_len + 1);
            ins->recommendation = NULL;
        }
    }
    result->insight_count = 0;
    if (result->summary) {
        alloc->free(alloc->ctx, result->summary, result->summary_len + 1);
        result->summary = NULL;
    }
    result->summary_len = 0;
}

char *hu_replay_build_context(hu_allocator_t *alloc, const hu_replay_result_t *result,
                              size_t *out_len) {
    if (!alloc || !result || !out_len)
        return NULL;
    *out_len = 0;
    if (result->insight_count == 0)
        return NULL;

    size_t cap = 512;
    for (size_t i = 0; i < result->insight_count; i++) {
        cap += (result->insights[i].observation_len + result->insights[i].recommendation_len + 16);
    }
    if (result->summary)
        cap += result->summary_len + 32;

    char *buf = (char *)alloc->alloc(alloc->ctx, cap);
    if (!buf)
        return NULL;

    size_t pos = 0;
    int w;

    w = snprintf(buf, cap, "### Session Replay Insights\n");
    if (w > 0 && (size_t)w < cap)
        pos += (size_t)w;

    if (pos < cap) {
        w = snprintf(buf + pos, cap - pos, "Overall quality: %d/100\n", result->overall_score);
        if (w > 0 && pos + (size_t)w < cap)
            pos += (size_t)w;
    }
    if (result->summary && pos < cap) {
        w = snprintf(buf + pos, cap - pos, "%s\n", result->summary);
        if (w > 0 && pos + (size_t)w < cap)
            pos += (size_t)w;
    }

    bool has_positive = false;
    bool has_negative = false;
    for (size_t i = 0; i < result->insight_count; i++) {
        if (result->insights[i].score_delta > 0)
            has_positive = true;
        else
            has_negative = true;
    }

    if (has_positive && pos < cap) {
        w = snprintf(buf + pos, cap - pos, "What worked:\n");
        if (w > 0 && pos + (size_t)w < cap)
            pos += (size_t)w;
        for (size_t i = 0; i < result->insight_count && pos < cap; i++) {
            if (result->insights[i].score_delta > 0 && result->insights[i].observation) {
                w = snprintf(buf + pos, cap - pos, "- %s\n", result->insights[i].observation);
                if (w > 0 && pos + (size_t)w < cap)
                    pos += (size_t)w;
            }
        }
    }
    if (has_negative && pos < cap) {
        w = snprintf(buf + pos, cap - pos, "What to improve:\n");
        if (w > 0 && pos + (size_t)w < cap)
            pos += (size_t)w;
        for (size_t i = 0; i < result->insight_count && pos < cap; i++) {
            if (result->insights[i].score_delta <= 0 && result->insights[i].observation) {
                w = snprintf(buf + pos, cap - pos, "- %s\n", result->insights[i].observation);
                if (w > 0 && pos + (size_t)w < cap)
                    pos += (size_t)w;
            }
        }
    }

    if (pos == 0 || pos >= cap) {
        alloc->free(alloc->ctx, buf, cap);
        return NULL;
    }
    buf[pos] = '\0';
    char *out = hu_strndup(alloc, buf, pos);
    alloc->free(alloc->ctx, buf, cap);
    if (out)
        *out_len = (size_t)pos;
    return out;
}
