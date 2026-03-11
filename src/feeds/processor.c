#include "human/feeds/processor.h"
#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define HU_FEEDS_ESCAPE_BUF 2048

static size_t escape_sql_string(const char *s, size_t len, char *out,
                                size_t out_cap) {
    size_t j = 0;
    for (size_t i = 0; i < len && s[i] != '\0'; i++) {
        if (s[i] == '\'') {
            if (j + 2 > out_cap)
                return 0;
            out[j++] = '\'';
            out[j++] = '\'';
        } else {
            if (j + 1 > out_cap)
                return 0;
            out[j++] = s[i];
        }
    }
    out[j] = '\0';
    return j;
}

hu_error_t hu_feeds_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;

    static const char sql[] =
        "CREATE TABLE IF NOT EXISTS feed_items (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    type TEXT NOT NULL,\n"
        "    source TEXT NOT NULL,\n"
        "    content TEXT NOT NULL,\n"
        "    topic TEXT,\n"
        "    relevance REAL NOT NULL,\n"
        "    fetched_at INTEGER NOT NULL,\n"
        "    processed INTEGER NOT NULL DEFAULT 0\n"
        ")";

    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_feeds_insert_sql(const hu_feed_item_t *item, char *buf,
                               size_t cap, size_t *out_len) {
    if (!item || !buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;

    const char *src = item->source ? item->source : "";
    size_t src_len = item->source_len ? item->source_len : strlen(src);
    const char *cnt = item->content ? item->content : "";
    size_t cnt_len = item->content_len ? item->content_len : strlen(cnt);
    const char *top = item->topic ? item->topic : "";
    size_t top_len = item->topic_len ? item->topic_len : strlen(top);

    char esc_src[HU_FEEDS_ESCAPE_BUF];
    char esc_cnt[HU_FEEDS_ESCAPE_BUF];
    char esc_top[HU_FEEDS_ESCAPE_BUF];

    if (src_len > 0 &&
        escape_sql_string(src, src_len, esc_src, sizeof(esc_src)) == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (src_len == 0)
        esc_src[0] = '\0';

    if (cnt_len > 0 &&
        escape_sql_string(cnt, cnt_len, esc_cnt, sizeof(esc_cnt)) == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (cnt_len == 0)
        esc_cnt[0] = '\0';

    if (top_len > 0 &&
        escape_sql_string(top, top_len, esc_top, sizeof(esc_top)) == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (top_len == 0)
        esc_top[0] = '\0';

    const char *type_str = hu_feed_type_str(item->type);
    int proc = item->processed ? 1 : 0;

    int n = snprintf(
        buf, cap,
        "INSERT INTO feed_items (type, source, content, topic, relevance, "
        "fetched_at, processed) VALUES ('%s', '%s', '%s', '%s', %f, %llu, %d)",
        type_str, esc_src, esc_cnt, esc_top, item->relevance,
        (unsigned long long)item->fetched_at, proc);

    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_feeds_query_unprocessed_sql(hu_feed_type_t type, uint32_t limit,
                                          char *buf, size_t cap,
                                          size_t *out_len) {
    if (!buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;

    const char *type_str = hu_feed_type_str(type);
    char esc[HU_FEEDS_ESCAPE_BUF];
    if (escape_sql_string(type_str, strlen(type_str), esc, sizeof(esc)) == 0)
        return HU_ERR_INVALID_ARGUMENT;

    int n = snprintf(
        buf, cap,
        "SELECT id, type, source, content, topic, relevance, fetched_at, "
        "processed FROM feed_items WHERE type = '%s' AND processed = 0 "
        "ORDER BY relevance DESC, fetched_at DESC LIMIT %u",
        esc, limit);

    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_feeds_mark_processed_sql(int64_t id, char *buf, size_t cap,
                                       size_t *out_len) {
    if (!buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;

    int n = snprintf(buf, cap,
                     "UPDATE feed_items SET processed = 1 WHERE id = %lld",
                     (long long)id);

    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_feeds_query_by_topic_sql(const char *topic, size_t topic_len,
                                      uint32_t limit, char *buf, size_t cap,
                                      size_t *out_len) {
    if (!topic || !buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;

    char esc[HU_FEEDS_ESCAPE_BUF];
    if (escape_sql_string(topic, topic_len, esc, sizeof(esc)) == 0)
        return HU_ERR_INVALID_ARGUMENT;

    int n = snprintf(
        buf, cap,
        "SELECT id, type, source, content, topic, relevance, fetched_at, "
        "processed FROM feed_items WHERE topic = '%s' ORDER BY relevance DESC "
        "LIMIT %u",
        esc, limit);

    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

bool hu_feeds_should_poll(hu_feed_type_t type, const hu_feed_config_t *config,
                          uint64_t last_poll_ms, uint64_t now_ms) {
    if (!config || type >= HU_FEED_COUNT)
        return false;
    if (!config->enabled[type])
        return false;
    uint32_t interval_min = config->poll_interval_minutes[type];
    if (interval_min == 0)
        return true;
    uint64_t interval_ms = (uint64_t)interval_min * 60ULL * 1000ULL;
    uint64_t elapsed = now_ms - last_poll_ms;
    return elapsed >= interval_ms;
}

const char *hu_feed_type_str(hu_feed_type_t type) {
    switch (type) {
    case HU_FEED_SOCIAL_FACEBOOK:
        return "social_facebook";
    case HU_FEED_SOCIAL_INSTAGRAM:
        return "social_instagram";
    case HU_FEED_APPLE_PHOTOS:
        return "apple_photos";
    case HU_FEED_GOOGLE_PHOTOS:
        return "google_photos";
    case HU_FEED_APPLE_CONTACTS:
        return "apple_contacts";
    case HU_FEED_APPLE_REMINDERS:
        return "apple_reminders";
    case HU_FEED_MUSIC:
        return "music";
    case HU_FEED_NEWS_RSS:
        return "news_rss";
    case HU_FEED_APPLE_HEALTH:
        return "apple_health";
    case HU_FEED_EMAIL:
        return "email";
    default:
        return "unknown";
    }
}

static int is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_';
}

static int words_equal(const char *a, size_t a_len, const char *b,
                       size_t b_len) {
    if (a_len != b_len)
        return 0;
    for (size_t i = 0; i < a_len; i++) {
        char ca = (char)(unsigned char)tolower((unsigned char)a[i]);
        char cb = (char)(unsigned char)tolower((unsigned char)b[i]);
        if (ca != cb)
            return 0;
    }
    return 1;
}

static size_t count_word_matches(const char *content, size_t content_len,
                                const char *interest, size_t interest_len) {
    if (content_len == 0 || interest_len == 0)
        return 0;

    size_t matches = 0;
    size_t ic = 0;
    while (ic < content_len) {
        while (ic < content_len && !is_word_char(content[ic]))
            ic++;
        if (ic >= content_len)
            break;
        size_t wc_start = ic;
        while (ic < content_len && is_word_char(content[ic]))
            ic++;
        size_t wc_len = ic - wc_start;

        size_t ii = 0;
        while (ii < interest_len) {
            while (ii < interest_len && !is_word_char(interest[ii]))
                ii++;
            if (ii >= interest_len)
                break;
            size_t wi_start = ii;
            while (ii < interest_len && is_word_char(interest[ii]))
                ii++;
            size_t wi_len = ii - wi_start;

            if (words_equal(content + wc_start, wc_len, interest + wi_start,
                            wi_len)) {
                matches++;
                break;
            }
        }
    }
    return matches;
}

static size_t count_words(const char *s, size_t len) {
    if (len == 0)
        return 0;
    size_t n = 0;
    size_t i = 0;
    while (i < len) {
        while (i < len && !is_word_char(s[i]))
            i++;
        if (i >= len)
            break;
        n++;
        while (i < len && is_word_char(s[i]))
            i++;
    }
    return n;
}

double hu_feeds_score_relevance(const char *content, size_t content_len,
                                const char *interest, size_t interest_len) {
    if (!content || !interest)
        return 0.0;
    size_t interest_words = count_words(interest, interest_len);
    if (interest_words == 0)
        return 0.0;
    size_t matches = count_word_matches(content, content_len, interest,
                                        interest_len);
    double score = (double)matches / (double)interest_words;
    if (score > 1.0)
        score = 1.0;
    return score;
}

static const char *feed_type_to_category(hu_feed_type_t type) {
    switch (type) {
    case HU_FEED_SOCIAL_FACEBOOK:
    case HU_FEED_SOCIAL_INSTAGRAM:
        return "Social";
    case HU_FEED_NEWS_RSS:
        return "News";
    case HU_FEED_MUSIC:
        return "Music";
    case HU_FEED_APPLE_PHOTOS:
    case HU_FEED_GOOGLE_PHOTOS:
        return "Photos";
    case HU_FEED_APPLE_CONTACTS:
        return "Contacts";
    case HU_FEED_APPLE_REMINDERS:
        return "Reminders";
    case HU_FEED_APPLE_HEALTH:
        return "Health";
    case HU_FEED_EMAIL:
        return "Email";
    default:
        return "Other";
    }
}

hu_error_t hu_feeds_build_prompt(hu_allocator_t *alloc,
                                 const hu_feed_item_t *items, size_t count,
                                 char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    if (count == 0 || !items) {
        char *empty =
            (char *)alloc->alloc(alloc->ctx, sizeof("[EXTERNAL AWARENESS]: (none)\n"));
        if (!empty)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(empty, "[EXTERNAL AWARENESS]: (none)\n", 31);
        *out = empty;
        *out_len = 30;
        return HU_OK;
    }

    int need = snprintf(NULL, 0, "[EXTERNAL AWARENESS]: ");
    if (need < 0)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < count; i++) {
        const char *cat = feed_type_to_category(items[i].type);
        const char *cnt = items[i].content ? items[i].content : "(no content)";
        size_t cnt_len =
            items[i].content_len ? items[i].content_len : strlen(cnt);
        const char *sep = (i == 0) ? "" : " ";
        int n = snprintf(NULL, 0, "%s%s: %.*s.", sep, cat, (int)cnt_len, cnt);
        if (n < 0)
            return HU_ERR_INVALID_ARGUMENT;
        need += n;
    }
    need += 2; /* \n and NUL */

    char *buf = (char *)alloc->alloc(alloc->ctx, (size_t)need);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    int n = snprintf(buf + pos, (size_t)need - pos, "[EXTERNAL AWARENESS]: ");
    if (n < 0 || (size_t)n >= (size_t)need - pos) {
        alloc->free(alloc->ctx, buf, (size_t)need);
        return HU_ERR_INVALID_ARGUMENT;
    }
    pos += (size_t)n;

    for (size_t i = 0; i < count; i++) {
        const char *cat = feed_type_to_category(items[i].type);
        const char *cnt = items[i].content ? items[i].content : "(no content)";
        size_t cnt_len =
            items[i].content_len ? items[i].content_len : strlen(cnt);
        const char *sep = (i == 0) ? "" : " ";
        n = snprintf(buf + pos, (size_t)need - pos, "%s%s: %.*s.", sep, cat,
                     (int)cnt_len, cnt);
        if (n < 0 || (size_t)n >= (size_t)need - pos) {
            alloc->free(alloc->ctx, buf, (size_t)need);
            return HU_ERR_INVALID_ARGUMENT;
        }
        pos += (size_t)n;
    }

    n = snprintf(buf + pos, (size_t)need - pos, "\n");
    if (n < 0 || (size_t)n >= (size_t)need - pos) {
        alloc->free(alloc->ctx, buf, (size_t)need);
        return HU_ERR_INVALID_ARGUMENT;
    }
    pos += (size_t)n;

    *out = buf;
    *out_len = pos;
    return HU_OK;
}

void hu_feed_item_deinit(hu_allocator_t *alloc, hu_feed_item_t *item) {
    if (!alloc || !item)
        return;
    if (item->source) {
        hu_str_free(alloc, item->source);
        item->source = NULL;
        item->source_len = 0;
    }
    if (item->content) {
        hu_str_free(alloc, item->content);
        item->content = NULL;
        item->content_len = 0;
    }
    if (item->topic) {
        hu_str_free(alloc, item->topic);
        item->topic = NULL;
        item->topic_len = 0;
    }
}
