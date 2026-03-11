typedef int hu_theory_of_mind_unused_;

#ifdef HU_ENABLE_SQLITE

#include "human/context/theory_of_mind.h"
#include "human/core/string.h"
#include "human/memory.h"
#include <ctype.h>
#include <math.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TOM_ALPHA 0.1
#define TOM_MIN_MESSAGES 5
#define TOM_LENGTH_DROP_RATIO 0.6
#define TOM_EMOJI_DROP_BASELINE 0.1
#define TOM_EMOJI_DROP_WINDOW 3
#define TOM_SENTIMENT_SHIFT_THRESH 0.3
#define TOM_INFERENCE_SEVERITY_THRESH 0.3

static size_t count_emoji_in_text(const char *s, size_t len) {
    size_t count = 0;
    const unsigned char *p = (const unsigned char *)s;
    const unsigned char *end = p + len;
    while (p < end && *p) {
        if (*p >= 0xF0 && p + 3 < end) {
            count++;
            p += 4;
        } else if (*p >= 0xE0 && p + 2 < end) {
            p += 3;
        } else if (*p >= 0xC0 && p + 1 < end) {
            p += 2;
        } else {
            p++;
        }
    }
    return count;
}

static size_t count_distinct_words(const char *s, size_t len) {
    if (!s || len == 0)
        return 0;
    char buf[256];
    size_t copy = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, s, copy);
    buf[copy] = '\0';

    size_t distinct = 0;
    size_t seen[64];
    size_t seen_count = 0;

    const char *p = buf;
    while (*p) {
        while (*p && !isalnum((unsigned char)*p))
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && isalnum((unsigned char)*p))
            p++;
        size_t wlen = (size_t)(p - start);
        if (wlen < 3)
            continue;

        bool found = false;
        for (size_t i = 0; i < seen_count && i < 64; i++) {
            const char *ws = buf + (seen[i] >> 8);
            size_t wl = seen[i] & 0xFF;
            if (wl == wlen && strncmp(ws, start, wlen) == 0) {
                found = true;
                break;
            }
        }
        if (!found && seen_count < 64) {
            seen[seen_count] = ((size_t)(start - buf) << 8) | wlen;
            seen_count++;
            distinct++;
        }
    }
    return distinct;
}

static double simple_sentiment(const char *s, size_t len) {
    (void)s;
    (void)len;
    return 0.0;
}

static int parse_timestamp_ms(const char *ts) {
    if (!ts || !ts[0])
        return 0;
    int h = 0, m = 0, s = 0;
    if (sscanf(ts, "%d:%d:%d", &h, &m, &s) >= 2)
        return (h * 3600 + m * 60 + s) * 1000;
    if (sscanf(ts, "%d:%d", &h, &m) >= 2)
        return (h * 3600 + m * 60) * 1000;
    return 0;
}

hu_error_t hu_theory_of_mind_update_baseline(hu_memory_t *memory, hu_allocator_t *alloc,
                                             const char *contact_id, size_t contact_id_len,
                                             const hu_channel_history_entry_t *entries,
                                             size_t entry_count) {
    (void)alloc;
    if (!memory || !contact_id || contact_id_len == 0 || !entries)
        return HU_ERR_INVALID_ARGUMENT;

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    size_t their_count = 0;
    double sum_len = 0, sum_emoji = 0, sum_topic = 0, sum_sentiment = 0;
    double sum_response_ms = 0;
    int response_count = 0;
    int prev_ts = 0;

    for (size_t i = 0; i < entry_count; i++) {
        const hu_channel_history_entry_t *e = &entries[i];
        if (e->from_me)
            continue;

        size_t text_len = strlen(e->text);
        sum_len += (double)text_len;
        sum_emoji += (double)count_emoji_in_text(e->text, text_len);
        sum_topic += (double)count_distinct_words(e->text, text_len);
        sum_sentiment += simple_sentiment(e->text, text_len);

        int ts = parse_timestamp_ms(e->timestamp);
        if (ts > 0 && prev_ts > 0) {
            int delta = ts - prev_ts;
            if (delta > 0) {
                sum_response_ms += (double)delta;
                response_count++;
            }
        }
        prev_ts = ts;
        their_count++;
    }

    if (their_count < TOM_MIN_MESSAGES)
        return HU_OK;

    double new_avg_len = sum_len / (double)their_count;
    double new_emoji_freq = (their_count > 0) ? sum_emoji / (double)their_count : 0.0;
    double new_topic_div = (their_count > 0) ? sum_topic / (double)their_count : 0.0;
    double new_sentiment = (their_count > 0) ? sum_sentiment / (double)their_count : 0.0;
    double new_response_ms =
        (response_count > 0) ? sum_response_ms / (double)response_count : 0.0;

    int64_t now_ts = (int64_t)time(NULL);

    sqlite3_stmt *sel = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT avg_message_length, avg_response_time_ms, emoji_frequency, "
                                "topic_diversity, sentiment_baseline, messages_sampled "
                                "FROM contact_baselines WHERE contact_id=?",
                                -1, &sel, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(sel, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);

    double old_len = new_avg_len, old_response = new_response_ms, old_emoji = new_emoji_freq;
    double old_topic = new_topic_div, old_sentiment = new_sentiment;
    int old_sampled = (int)their_count;

    rc = sqlite3_step(sel);
    if (rc == SQLITE_ROW) {
        old_len = sqlite3_column_double(sel, 0);
        old_response = sqlite3_column_double(sel, 1);
        old_emoji = sqlite3_column_double(sel, 2);
        old_topic = sqlite3_column_double(sel, 3);
        old_sentiment = sqlite3_column_double(sel, 4);
        old_sampled = sqlite3_column_int(sel, 5);
    }
    sqlite3_finalize(sel);

    double alpha = TOM_ALPHA;
    double final_len = alpha * new_avg_len + (1.0 - alpha) * old_len;
    double final_response = alpha * new_response_ms + (1.0 - alpha) * old_response;
    double final_emoji = alpha * new_emoji_freq + (1.0 - alpha) * old_emoji;
    double final_topic = alpha * new_topic_div + (1.0 - alpha) * old_topic;
    double final_sentiment = alpha * new_sentiment + (1.0 - alpha) * old_sentiment;
    int final_sampled = old_sampled + (int)their_count;

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db,
                            "INSERT OR REPLACE INTO contact_baselines("
                            "contact_id, avg_message_length, avg_response_time_ms, "
                            "emoji_frequency, topic_diversity, sentiment_baseline, "
                            "messages_sampled, updated_at) VALUES(?,?,?,?,?,?,?,?)",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 2, final_len);
    sqlite3_bind_double(stmt, 3, final_response);
    sqlite3_bind_double(stmt, 4, final_emoji);
    sqlite3_bind_double(stmt, 5, final_topic);
    sqlite3_bind_double(stmt, 6, final_sentiment);
    sqlite3_bind_int(stmt, 7, final_sampled);
    sqlite3_bind_int64(stmt, 8, now_ts);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_theory_of_mind_get_baseline(hu_memory_t *memory, hu_allocator_t *alloc,
                                         const char *contact_id, size_t contact_id_len,
                                         hu_contact_baseline_t *out) {
    (void)alloc;
    if (!memory || !contact_id || contact_id_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT avg_message_length, avg_response_time_ms, emoji_frequency, "
                                "topic_diversity, sentiment_baseline, messages_sampled, updated_at "
                                "FROM contact_baselines WHERE contact_id=?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return HU_ERR_NOT_FOUND;
    }

    out->avg_message_length = sqlite3_column_double(stmt, 0);
    out->avg_response_time_ms = sqlite3_column_double(stmt, 1);
    out->emoji_frequency = sqlite3_column_double(stmt, 2);
    out->topic_diversity = sqlite3_column_double(stmt, 3);
    out->sentiment_baseline = sqlite3_column_double(stmt, 4);
    out->messages_sampled = sqlite3_column_int(stmt, 5);
    out->updated_at = sqlite3_column_int64(stmt, 6);

    sqlite3_finalize(stmt);
    return HU_OK;
}

hu_theory_of_mind_deviation_t hu_theory_of_mind_detect_deviation(
    const hu_contact_baseline_t *baseline, const hu_channel_history_entry_t *entries,
    size_t count) {
    hu_theory_of_mind_deviation_t dev = {0};
    if (!baseline || !entries || count == 0)
        return dev;

    size_t their_count = 0;
    double sum_len = 0, sum_emoji = 0, sum_topic = 0, sum_sentiment = 0;
    size_t last_3_emoji[3] = {0};
    size_t last_3_idx = 0;

    for (size_t i = 0; i < count; i++) {
        const hu_channel_history_entry_t *e = &entries[i];
        if (e->from_me)
            continue;

        size_t text_len = strlen(e->text);
        size_t emoji_c = count_emoji_in_text(e->text, text_len);
        sum_len += (double)text_len;
        sum_emoji += (double)emoji_c;
        sum_topic += (double)count_distinct_words(e->text, text_len);
        sum_sentiment += simple_sentiment(e->text, text_len);
        their_count++;

        last_3_emoji[last_3_idx % 3] = emoji_c;
        last_3_idx++;
    }

    size_t emoji_in_last_3 = 0;
    if (last_3_idx >= 3) {
        emoji_in_last_3 = last_3_emoji[(last_3_idx - 1) % 3] + last_3_emoji[(last_3_idx - 2) % 3] +
                          last_3_emoji[(last_3_idx - 3) % 3];
    } else {
        for (size_t j = 0; j < last_3_idx; j++)
            emoji_in_last_3 += last_3_emoji[j];
    }

    if (their_count == 0)
        return dev;

    (void)sum_emoji;
    double curr_avg_len = sum_len / (double)their_count;
    double curr_topic = sum_topic / (double)their_count;
    double curr_sentiment = sum_sentiment / (double)their_count;

    dev.length_drop =
        (baseline->avg_message_length > 1.0) &&
        (curr_avg_len < TOM_LENGTH_DROP_RATIO * baseline->avg_message_length);

    dev.emoji_drop = (baseline->emoji_frequency > TOM_EMOJI_DROP_BASELINE) &&
                     (last_3_idx >= TOM_EMOJI_DROP_WINDOW) && (emoji_in_last_3 == 0);

    dev.topic_narrowing =
        (baseline->topic_diversity > 0.5) &&
        (curr_topic < 0.5 * baseline->topic_diversity);

    dev.sentiment_shift =
        (fabs(curr_sentiment - baseline->sentiment_baseline) > TOM_SENTIMENT_SHIFT_THRESH);

    dev.response_slow = false;

    float weight_sum = 0.0f;
    float severity_sum = 0.0f;
    if (dev.length_drop) {
        severity_sum += 0.3f;
        weight_sum += 1.0f;
    }
    if (dev.emoji_drop) {
        severity_sum += 0.25f;
        weight_sum += 1.0f;
    }
    if (dev.topic_narrowing) {
        severity_sum += 0.2f;
        weight_sum += 1.0f;
    }
    if (dev.sentiment_shift) {
        severity_sum += 0.25f;
        weight_sum += 1.0f;
    }
    if (dev.response_slow) {
        severity_sum += 0.2f;
        weight_sum += 1.0f;
    }

    dev.severity = (weight_sum > 0.0f) ? severity_sum / weight_sum : 0.0f;
    if (dev.severity > 1.0f)
        dev.severity = 1.0f;

    return dev;
}

char *hu_theory_of_mind_build_inference(hu_allocator_t *alloc,
                                        const char *contact_name, size_t contact_name_len,
                                        const char *pronoun, size_t pronoun_len,
                                        const hu_theory_of_mind_deviation_t *dev,
                                        size_t *out_len) {
    if (!alloc || !dev || !out_len)
        return NULL;
    *out_len = 0;

    if (dev->severity < TOM_INFERENCE_SEVERITY_THRESH)
        return NULL;

    const char *name = contact_name && contact_name_len > 0 ? contact_name : "They";
    size_t nlen = contact_name_len;
    if (nlen == 0 && contact_name)
        nlen = strlen(contact_name);

    const char *pro = "they";
    size_t plen = 4;
    if (pronoun && pronoun_len > 0) {
        pro = pronoun;
        plen = pronoun_len;
    } else if (pronoun) {
        plen = strlen(pronoun);
        pro = pronoun;
    }

    char buf[512];
    size_t pos = 0;

    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "[THEORY OF MIND: ");
    if (nlen > 0 && name) {
        size_t copy = nlen < 32 ? nlen : 32;
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "%.*s's messages ", (int)copy, name);
    } else {
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "Messages ");
    }

    bool first = true;
    if (dev->length_drop) {
        if (!first)
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, " and ");
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "are shorter than usual");
        first = false;
    }
    if (dev->emoji_drop) {
        if (!first)
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, " and ");
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "%.*s hasn't used emoji lately",
                                (int)plen, pro);
        first = false;
    }
    if (dev->topic_narrowing) {
        if (!first)
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, " and ");
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "conversation has narrowed");
        first = false;
    }
    if (dev->sentiment_shift) {
        if (!first)
            pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, " and ");
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "tone seems different");
        first = false;
    }

    if (first)
        pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos, "show subtle changes");

    pos += (size_t)snprintf(buf + pos, sizeof(buf) - pos,
                            ". May be upset or distracted. Use natural probing like 'hey you "
                            "okay?' — never cite data.]");

    char *out = hu_strndup(alloc, buf, pos);
    if (out)
        *out_len = pos;
    return out;
}

#else /* !HU_ENABLE_SQLITE */

#include "human/context/theory_of_mind.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <stddef.h>
#include <string.h>

hu_error_t hu_theory_of_mind_update_baseline(hu_memory_t *memory, hu_allocator_t *alloc,
                                             const char *contact_id, size_t contact_id_len,
                                             const hu_channel_history_entry_t *entries,
                                             size_t entry_count) {
    (void)memory;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    (void)entries;
    (void)entry_count;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_theory_of_mind_get_baseline(hu_memory_t *memory, hu_allocator_t *alloc,
                                         const char *contact_id, size_t contact_id_len,
                                         hu_contact_baseline_t *out) {
    (void)memory;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
}

hu_theory_of_mind_deviation_t hu_theory_of_mind_detect_deviation(
    const hu_contact_baseline_t *baseline, const hu_channel_history_entry_t *entries,
    size_t count) {
    (void)baseline;
    (void)entries;
    (void)count;
    hu_theory_of_mind_deviation_t dev = {0};
    return dev;
}

char *hu_theory_of_mind_build_inference(hu_allocator_t *alloc,
                                        const char *contact_name, size_t contact_name_len,
                                        const char *pronoun, size_t pronoun_len,
                                        const hu_theory_of_mind_deviation_t *dev,
                                        size_t *out_len) {
    (void)alloc;
    (void)contact_name;
    (void)contact_name_len;
    (void)pronoun;
    (void)pronoun_len;
    (void)dev;
    (void)out_len;
    return NULL;
}

#endif /* HU_ENABLE_SQLITE */
