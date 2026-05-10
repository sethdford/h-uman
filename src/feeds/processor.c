#include "human/feeds/processor.h"
#include "human/core/log.h"
#include "human/memory/vector.h"
#ifdef HU_ENABLE_FEEDS
#include "human/feeds/file_ingest.h"
#include "human/feeds/gmail.h"
#include "human/feeds/imessage.h"
#include "human/feeds/news.h"
#include "human/feeds/twitter.h"
#endif
#include "human/core/string.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define HU_FEEDS_PROMPT_MAX_NEED (SIZE_MAX / 2U)

#define HU_FEEDS_ESCAPE_BUF 2048

static size_t escape_sql_string(const char *s, size_t len, char *out, size_t out_cap) {
    size_t n = 0;
    (void)hu_sql_quote_escape_into(s, len, out, out_cap, &n);
    return n;
}

hu_error_t hu_feeds_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;

    static const char sql[] = "CREATE TABLE IF NOT EXISTS feed_items (\n"
                              "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
                              "    source TEXT NOT NULL,\n"
                              "    contact_id TEXT,\n"
                              "    content_type TEXT NOT NULL,\n"
                              "    content TEXT NOT NULL,\n"
                              "    url TEXT,\n"
                              "    ingested_at INTEGER NOT NULL,\n"
                              "    referenced INTEGER DEFAULT 0,\n"
                              "    cluster_id INTEGER DEFAULT NULL\n"
                              ")";

    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_feeds_insert_sql(const hu_feed_item_t *item, char *buf, size_t cap, size_t *out_len) {
    if (!item || !buf || !out_len || cap < 512)
        return HU_ERR_INVALID_ARGUMENT;

    const char *src = item->source ? item->source : "";
    size_t src_len = item->source_len ? item->source_len : strlen(src);
    const char *cnt = item->content ? item->content : "";
    size_t cnt_len = item->content_len ? item->content_len : strlen(cnt);

    char esc_src[HU_FEEDS_ESCAPE_BUF];
    char esc_cnt[HU_FEEDS_ESCAPE_BUF];

    if (src_len > 0 && escape_sql_string(src, src_len, esc_src, sizeof(esc_src)) == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (src_len == 0)
        esc_src[0] = '\0';

    if (cnt_len > 0 && escape_sql_string(cnt, cnt_len, esc_cnt, sizeof(esc_cnt)) == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (cnt_len == 0)
        esc_cnt[0] = '\0';

    const char *type_str = hu_feed_type_str(item->type);
    const char *cid = item->contact_id ? item->contact_id : "";
    size_t cid_len = item->contact_id_len ? item->contact_id_len : strlen(cid);
    char esc_cid[HU_FEEDS_ESCAPE_BUF];
    if (cid_len > 0 && escape_sql_string(cid, cid_len, esc_cid, sizeof(esc_cid)) == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (cid_len == 0)
        esc_cid[0] = '\0';

    int n = snprintf(
        buf, cap,
        "INSERT INTO feed_items (source, contact_id, content_type, content, url, ingested_at) "
        "VALUES ('%s', '%s', '%s', '%s', '', %llu)",
        esc_src, esc_cid, type_str, esc_cnt, (unsigned long long)item->fetched_at);

    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_feeds_query_unprocessed_sql(hu_feed_type_t type, uint32_t limit, char *buf,
                                          size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;

    const char *type_str = hu_feed_type_str(type);
    char esc[HU_FEEDS_ESCAPE_BUF];
    if (escape_sql_string(type_str, strlen(type_str), esc, sizeof(esc)) == 0)
        return HU_ERR_INVALID_ARGUMENT;

    int n = snprintf(buf, cap,
                     "SELECT id, source, content_type, content, url, ingested_at FROM "
                     "feed_items WHERE source LIKE '%s%%' ORDER BY ingested_at DESC LIMIT %u",
                     esc, limit);

    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_feeds_mark_processed_sql(int64_t id, char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;

    int n =
        snprintf(buf, cap, "UPDATE feed_items SET referenced = 1 WHERE id = %lld", (long long)id);

    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_feeds_query_by_topic_sql(const char *topic, size_t topic_len, uint32_t limit,
                                       char *buf, size_t cap, size_t *out_len) {
    if (!topic || !buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;

    char esc[HU_FEEDS_ESCAPE_BUF];
    if (escape_sql_string(topic, topic_len, esc, sizeof(esc)) == 0)
        return HU_ERR_INVALID_ARGUMENT;

    int n = snprintf(buf, cap,
                     "SELECT id, source, content_type, content, url, ingested_at FROM "
                     "feed_items WHERE content LIKE '%%%s%%' ORDER BY ingested_at DESC LIMIT %u",
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
    case HU_FEED_GMAIL:
        return "gmail";
    case HU_FEED_IMESSAGE:
        return "imessage";
    case HU_FEED_TWITTER:
        return "twitter";
    case HU_FEED_TIKTOK:
        return "tiktok";
    case HU_FEED_FILE_INGEST:
        return "file_ingest";
    default:
        return "unknown";
    }
}

static int is_word_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

static int words_equal(const char *a, size_t a_len, const char *b, size_t b_len) {
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

static size_t count_word_matches(const char *content, size_t content_len, const char *interest,
                                 size_t interest_len) {
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

            if (words_equal(content + wc_start, wc_len, interest + wi_start, wi_len)) {
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

double hu_feeds_score_relevance(const char *content, size_t content_len, const char *interest,
                                size_t interest_len) {
    if (!content || !interest)
        return 0.0;
    size_t interest_words = count_words(interest, interest_len);
    if (interest_words == 0)
        return 0.0;
    size_t matches = count_word_matches(content, content_len, interest, interest_len);
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
    case HU_FEED_GMAIL:
        return "Email";
    case HU_FEED_IMESSAGE:
        return "Messages";
    case HU_FEED_TWITTER:
        return "Social";
    case HU_FEED_TIKTOK:
        return "Social";
    case HU_FEED_FILE_INGEST:
        return "Ingest";
    default:
        return "Other";
    }
}

hu_error_t hu_feeds_build_prompt(hu_allocator_t *alloc, const hu_feed_item_t *items, size_t count,
                                 char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    if (count == 0 || !items) {
        static const char msg[] = "[EXTERNAL AWARENESS]: (none)\n";
        char *empty = (char *)alloc->alloc(alloc->ctx, sizeof(msg));
        if (!empty)
            return HU_ERR_OUT_OF_MEMORY;
        memcpy(empty, msg, sizeof(msg));
        *out = empty;
        *out_len = sizeof(msg) - 1;
        return HU_OK;
    }

    int hn = snprintf(NULL, 0, "[EXTERNAL AWARENESS]: ");
    if (hn < 0)
        return HU_ERR_INVALID_ARGUMENT;
    size_t need = (size_t)hn;
    if (need > HU_FEEDS_PROMPT_MAX_NEED)
        return HU_ERR_INVALID_ARGUMENT;

    for (size_t i = 0; i < count; i++) {
        const char *cat = feed_type_to_category(items[i].type);
        const char *cnt = items[i].content ? items[i].content : "(no content)";
        size_t cnt_len = items[i].content_len ? items[i].content_len : strlen(cnt);
        const char *sep = (i == 0) ? "" : " ";
        int n = snprintf(NULL, 0, "%s%s: %.*s.", sep, cat, (int)cnt_len, cnt);
        if (n < 0)
            return HU_ERR_INVALID_ARGUMENT;
        size_t add = (size_t)n;
        if (need > HU_FEEDS_PROMPT_MAX_NEED - add)
            return HU_ERR_INVALID_ARGUMENT;
        need += add;
    }
    if (need > HU_FEEDS_PROMPT_MAX_NEED - 2U)
        return HU_ERR_INVALID_ARGUMENT;
    need += 2U; /* \n and NUL */

    char *buf = (char *)alloc->alloc(alloc->ctx, need);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    int n = snprintf(buf + pos, need - pos, "[EXTERNAL AWARENESS]: ");
    if (n < 0 || (size_t)n >= need - pos) {
        alloc->free(alloc->ctx, buf, need);
        return HU_ERR_INVALID_ARGUMENT;
    }
    pos += (size_t)n;

    for (size_t i = 0; i < count; i++) {
        const char *cat = feed_type_to_category(items[i].type);
        const char *cnt = items[i].content ? items[i].content : "(no content)";
        size_t cnt_len = items[i].content_len ? items[i].content_len : strlen(cnt);
        const char *sep = (i == 0) ? "" : " ";
        n = snprintf(buf + pos, need - pos, "%s%s: %.*s.", sep, cat, (int)cnt_len, cnt);
        if (n < 0 || (size_t)n >= need - pos) {
            alloc->free(alloc->ctx, buf, need);
            return HU_ERR_INVALID_ARGUMENT;
        }
        pos += (size_t)n;
    }

    n = snprintf(buf + pos, need - pos, "\n");
    if (n < 0 || (size_t)n >= need - pos) {
        alloc->free(alloc->ctx, buf, need);
        return HU_ERR_INVALID_ARGUMENT;
    }
    pos += (size_t)n;

    *out = buf;
    *out_len = pos;
    return HU_OK;
}

#ifdef HU_ENABLE_SQLITE
#include <sqlite3.h>

hu_error_t hu_feed_processor_store_item(hu_feed_processor_t *proc,
                                        const hu_feed_item_stored_t *item) {
    if (!proc || !proc->db || !item)
        return HU_ERR_INVALID_ARGUMENT;
    if (proc->interests && proc->interests_len > 0 && proc->relevance_threshold > 0.0) {
        double score = hu_feeds_score_relevance(item->content, item->content_len, proc->interests,
                                                proc->interests_len);
        if (score < proc->relevance_threshold)
            return HU_OK;
    }
    const char *sql =
        "INSERT OR IGNORE INTO feed_items (source, contact_id, content_type, content, "
        "url, ingested_at) VALUES (?, ?, ?, ?, ?, ?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(proc->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(stmt, 1, item->source, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, item->contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, item->content_type, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 4, item->content, (int)item->content_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 5, item->url, -1, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 6, item->ingested_at);

    int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        sqlite3_finalize(stmt);
        return HU_ERR_IO;
    }
    int inserted = sqlite3_changes(proc->db);
    sqlite3_finalize(stmt);

    /* Generate and store embedding if configured and row was inserted */
    if (inserted > 0 && proc->embedder && proc->vec_store && proc->embedder->vtable &&
        proc->vec_store->vtable) {
        char id_str[32];
        int64_t row_id = sqlite3_last_insert_rowid(proc->db);
        snprintf(id_str, sizeof(id_str), "%lld", (long long)row_id);
        hu_embedding_t emb = {0};
        if (proc->embedder->vtable->embed(proc->embedder->ctx, proc->alloc, item->content,
                                          item->content_len, &emb) == HU_OK) {
            proc->vec_store->vtable->insert(proc->vec_store->ctx, proc->alloc, id_str,
                                            strlen(id_str), &emb, item->content, item->content_len);
            hu_embedding_free(proc->alloc, &emb);
        }
    }
    return HU_OK;
}

hu_error_t hu_feed_processor_get_recent(hu_allocator_t *alloc, sqlite3 *db, const char *source,
                                        size_t src_len, size_t limit, hu_feed_item_stored_t **out,
                                        size_t *out_count) {
    (void)src_len;
    if (!alloc || !db || !source || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_count = 0;

    const char *sql = "SELECT source, contact_id, content_type, content, url, ingested_at "
                      "FROM feed_items WHERE source = ? ORDER BY ingested_at DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(stmt, 1, source, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, (int)limit);

    hu_feed_item_stored_t *items = NULL;
    size_t count = 0;
    size_t cap = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            size_t new_cap = cap == 0 ? 4 : cap * 2;
            hu_feed_item_stored_t *tmp = (hu_feed_item_stored_t *)alloc->alloc(
                alloc->ctx, new_cap * sizeof(hu_feed_item_stored_t));
            if (!tmp) {
                if (items)
                    alloc->free(alloc->ctx, items, cap * sizeof(hu_feed_item_stored_t));
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            if (items) {
                memcpy(tmp, items, count * sizeof(hu_feed_item_stored_t));
                alloc->free(alloc->ctx, items, cap * sizeof(hu_feed_item_stored_t));
            }
            items = tmp;
            cap = new_cap;
        }
        hu_feed_item_stored_t *it = &items[count];
        memset(it, 0, sizeof(*it));

        const char *s = (const char *)sqlite3_column_text(stmt, 0);
        if (s)
            snprintf(it->source, sizeof(it->source), "%s", s);
        s = (const char *)sqlite3_column_text(stmt, 1);
        if (s)
            snprintf(it->contact_id, sizeof(it->contact_id), "%s", s);
        s = (const char *)sqlite3_column_text(stmt, 2);
        if (s)
            snprintf(it->content_type, sizeof(it->content_type), "%s", s);
        s = (const char *)sqlite3_column_text(stmt, 3);
        if (s) {
            snprintf(it->content, sizeof(it->content), "%s", s);
            it->content_len = strlen(it->content);
        }
        s = (const char *)sqlite3_column_text(stmt, 4);
        if (s)
            snprintf(it->url, sizeof(it->url), "%s", s);
        it->ingested_at = sqlite3_column_int64(stmt, 5);
        count++;
    }
    sqlite3_finalize(stmt);

    *out = items;
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_feed_processor_get_for_contact(hu_allocator_t *alloc, sqlite3 *db,
                                             const char *contact_id, size_t cid_len, size_t limit,
                                             hu_feed_item_stored_t **out, size_t *out_count) {
    (void)cid_len;
    if (!alloc || !db || !contact_id || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_count = 0;

    const char *sql = "SELECT source, contact_id, content_type, content, url, ingested_at "
                      "FROM feed_items WHERE contact_id = ? ORDER BY ingested_at DESC "
                      "LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(stmt, 1, contact_id, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, (int)limit);

    hu_feed_item_stored_t *items = NULL;
    size_t count = 0;
    size_t cap = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            size_t new_cap = cap == 0 ? 4 : cap * 2;
            hu_feed_item_stored_t *tmp = (hu_feed_item_stored_t *)alloc->alloc(
                alloc->ctx, new_cap * sizeof(hu_feed_item_stored_t));
            if (!tmp) {
                if (items)
                    alloc->free(alloc->ctx, items, cap * sizeof(hu_feed_item_stored_t));
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            if (items) {
                memcpy(tmp, items, count * sizeof(hu_feed_item_stored_t));
                alloc->free(alloc->ctx, items, cap * sizeof(hu_feed_item_stored_t));
            }
            items = tmp;
            cap = new_cap;
        }
        hu_feed_item_stored_t *it = &items[count];
        memset(it, 0, sizeof(*it));

        const char *s = (const char *)sqlite3_column_text(stmt, 0);
        if (s)
            snprintf(it->source, sizeof(it->source), "%s", s);
        s = (const char *)sqlite3_column_text(stmt, 1);
        if (s)
            snprintf(it->contact_id, sizeof(it->contact_id), "%s", s);
        s = (const char *)sqlite3_column_text(stmt, 2);
        if (s)
            snprintf(it->content_type, sizeof(it->content_type), "%s", s);
        s = (const char *)sqlite3_column_text(stmt, 3);
        if (s) {
            snprintf(it->content, sizeof(it->content), "%s", s);
            it->content_len = strlen(it->content);
        }
        s = (const char *)sqlite3_column_text(stmt, 4);
        if (s)
            snprintf(it->url, sizeof(it->url), "%s", s);
        it->ingested_at = sqlite3_column_int64(stmt, 5);
        count++;
    }
    sqlite3_finalize(stmt);

    *out = items;
    *out_count = count;
    return HU_OK;
}

void hu_feed_items_free(hu_allocator_t *alloc, hu_feed_item_stored_t *items, size_t count) {
    (void)count;
    if (alloc && items)
        alloc->free(alloc->ctx, items, count * sizeof(hu_feed_item_stored_t));
}

hu_error_t hu_feed_processor_poll(hu_feed_processor_t *proc, const hu_feed_config_t *config,
                                  uint64_t *last_poll_ms, uint64_t now_ms, size_t *items_ingested) {
    if (!proc || !config || !last_poll_ms || !items_ingested)
        return HU_ERR_INVALID_ARGUMENT;
    *items_ingested = 0;

    for (int i = 0; i < HU_FEED_COUNT; i++) {
        hu_feed_type_t type = (hu_feed_type_t)i;
        if (!hu_feeds_should_poll(type, config, last_poll_ms[i], now_ms))
            continue;

        /* Mark as polled regardless of fetch outcome to avoid tight retry loops */
        last_poll_ms[i] = now_ms;

        if (type == HU_FEED_NEWS_RSS) {
#ifdef HU_ENABLE_FEEDS
            static const char *ai_rss_feeds[] = {
                "https://feeds.bbci.co.uk/news/technology/rss.xml",
                "https://hnrss.org/newest?q=AI+OR+LLM+OR+GPT+OR+Claude",
                "https://arxiv.org/rss/cs.AI",
                "https://arxiv.org/rss/cs.CL",
                "https://blog.google/technology/ai/rss/",
                "https://openai.com/blog/rss.xml",
            };
            static const size_t ai_feed_count = sizeof(ai_rss_feeds) / sizeof(ai_rss_feeds[0]);
            for (size_t fi = 0; fi < ai_feed_count; fi++) {
                hu_rss_article_t articles[10];
                size_t article_count = 0;
                size_t url_len = strlen(ai_rss_feeds[fi]);
                if (hu_news_fetch_rss(proc->alloc, ai_rss_feeds[fi], url_len, articles, 10,
                                      &article_count) == HU_OK) {
                    for (size_t a = 0; a < article_count; a++) {
                        hu_feed_item_stored_t item = {0};
                        snprintf(item.source, sizeof(item.source), "rss");
                        snprintf(item.content_type, sizeof(item.content_type), "article");
                        snprintf(item.content, sizeof(item.content), "%s: %s", articles[a].title,
                                 articles[a].description);
                        item.content_len = strlen(item.content);
                        snprintf(item.url, sizeof(item.url), "%s", articles[a].link);
                        item.ingested_at = (int64_t)(now_ms / 1000);
                        if (hu_feed_processor_store_item(proc, &item) == HU_OK)
                            (*items_ingested)++;
                    }
                }
            }
#endif
        }

        if (type == HU_FEED_GMAIL) {
#ifdef HU_ENABLE_FEEDS
            hu_feed_ingest_item_t gmail_items[10];
            size_t gmail_count = 0;
            if (hu_gmail_feed_fetch(proc->alloc, proc->gmail_client_id, proc->gmail_client_id_len,
                                    proc->gmail_client_secret, proc->gmail_client_secret_len,
                                    proc->gmail_refresh_token, proc->gmail_refresh_token_len,
                                    gmail_items, 10, &gmail_count) == HU_OK) {
                for (size_t g = 0; g < gmail_count; g++) {
                    hu_feed_item_stored_t item = {0};
                    snprintf(item.source, sizeof(item.source), "%s", gmail_items[g].source);
                    snprintf(item.content_type, sizeof(item.content_type), "%s",
                             gmail_items[g].content_type);
                    size_t clen = gmail_items[g].content_len;
                    if (clen >= sizeof(item.content))
                        clen = sizeof(item.content) - 1;
                    memcpy(item.content, gmail_items[g].content, clen);
                    item.content[clen] = '\0';
                    item.content_len = clen;
                    item.ingested_at = gmail_items[g].ingested_at;
                    if (hu_feed_processor_store_item(proc, &item) == HU_OK)
                        (*items_ingested)++;
                }
            }
#endif
        }

        if (type == HU_FEED_IMESSAGE) {
#ifdef HU_ENABLE_FEEDS
            int64_t since = (int64_t)(now_ms / 1000) - 86400;
            hu_feed_ingest_item_t im_items[20];
            size_t im_count = 0;
            if (hu_imessage_feed_fetch(proc->alloc, since, im_items, 20, &im_count) == HU_OK) {
                for (size_t m = 0; m < im_count; m++) {
                    hu_feed_item_stored_t item = {0};
                    snprintf(item.source, sizeof(item.source), "%s", im_items[m].source);
                    snprintf(item.contact_id, sizeof(item.contact_id), "%s",
                             im_items[m].contact_id);
                    snprintf(item.content_type, sizeof(item.content_type), "%s",
                             im_items[m].content_type);
                    size_t clen = im_items[m].content_len;
                    if (clen >= sizeof(item.content))
                        clen = sizeof(item.content) - 1;
                    memcpy(item.content, im_items[m].content, clen);
                    item.content[clen] = '\0';
                    item.content_len = clen;
                    item.ingested_at = im_items[m].ingested_at;
                    if (hu_feed_processor_store_item(proc, &item) == HU_OK)
                        (*items_ingested)++;
                }
            }
#endif
        }

        if (type == HU_FEED_TWITTER) {
#ifdef HU_ENABLE_FEEDS
            hu_feed_ingest_item_t tw_items[20];
            size_t tw_count = 0;
            if (hu_twitter_feed_fetch(proc->alloc, proc->twitter_bearer_token,
                                      proc->twitter_bearer_token_len, tw_items, 20,
                                      &tw_count) == HU_OK) {
                for (size_t t = 0; t < tw_count; t++) {
                    hu_feed_item_stored_t item = {0};
                    snprintf(item.source, sizeof(item.source), "%s", tw_items[t].source);
                    snprintf(item.content_type, sizeof(item.content_type), "%s",
                             tw_items[t].content_type);
                    size_t clen = tw_items[t].content_len;
                    if (clen >= sizeof(item.content))
                        clen = sizeof(item.content) - 1;
                    memcpy(item.content, tw_items[t].content, clen);
                    item.content[clen] = '\0';
                    item.content_len = clen;
                    if (tw_items[t].url[0])
                        snprintf(item.url, sizeof(item.url), "%s", tw_items[t].url);
                    item.ingested_at = tw_items[t].ingested_at;
                    if (hu_feed_processor_store_item(proc, &item) == HU_OK)
                        (*items_ingested)++;
                }
            }
#endif
        }

        if (type == HU_FEED_FILE_INGEST) {
#ifdef HU_ENABLE_FEEDS
            hu_feed_ingest_item_t fi_items[100];
            size_t fi_count = 0;
            if (hu_file_ingest_fetch(proc->alloc, fi_items, 100, &fi_count) == HU_OK) {
                for (size_t f = 0; f < fi_count; f++) {
                    hu_feed_item_stored_t item = {0};
                    snprintf(item.source, sizeof(item.source), "%s", fi_items[f].source);
                    snprintf(item.content_type, sizeof(item.content_type), "%s",
                             fi_items[f].content_type);
                    size_t clen = fi_items[f].content_len;
                    if (clen >= sizeof(item.content))
                        clen = sizeof(item.content) - 1;
                    memcpy(item.content, fi_items[f].content, clen);
                    item.content[clen] = '\0';
                    item.content_len = clen;
                    if (fi_items[f].url[0])
                        snprintf(item.url, sizeof(item.url), "%s", fi_items[f].url);
                    item.ingested_at = fi_items[f].ingested_at;
                    if (hu_feed_processor_store_item(proc, &item) == HU_OK)
                        (*items_ingested)++;
                }
            }
#endif
        }
    }
    return HU_OK;
}

hu_error_t hu_feed_processor_get_all_recent(hu_allocator_t *alloc, sqlite3 *db, int64_t since_ts,
                                            size_t limit, hu_feed_item_stored_t **out,
                                            size_t *out_count) {
    if (!alloc || !db || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_count = 0;

    const char *sql = "SELECT source, contact_id, content_type, content, url, ingested_at "
                      "FROM feed_items WHERE ingested_at >= ? ORDER BY ingested_at DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_int64(stmt, 1, since_ts);
    sqlite3_bind_int(stmt, 2, (int)limit);

    hu_feed_item_stored_t *items = NULL;
    size_t count = 0;
    size_t cap = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            size_t new_cap = cap == 0 ? 4 : cap * 2;
            hu_feed_item_stored_t *tmp = (hu_feed_item_stored_t *)alloc->alloc(
                alloc->ctx, new_cap * sizeof(hu_feed_item_stored_t));
            if (!tmp) {
                if (items)
                    alloc->free(alloc->ctx, items, cap * sizeof(hu_feed_item_stored_t));
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            if (items) {
                memcpy(tmp, items, count * sizeof(hu_feed_item_stored_t));
                alloc->free(alloc->ctx, items, cap * sizeof(hu_feed_item_stored_t));
            }
            items = tmp;
            cap = new_cap;
        }
        hu_feed_item_stored_t *it = &items[count];
        memset(it, 0, sizeof(*it));

        const char *s = (const char *)sqlite3_column_text(stmt, 0);
        if (s)
            snprintf(it->source, sizeof(it->source), "%s", s);
        s = (const char *)sqlite3_column_text(stmt, 1);
        if (s)
            snprintf(it->contact_id, sizeof(it->contact_id), "%s", s);
        s = (const char *)sqlite3_column_text(stmt, 2);
        if (s)
            snprintf(it->content_type, sizeof(it->content_type), "%s", s);
        s = (const char *)sqlite3_column_text(stmt, 3);
        if (s) {
            snprintf(it->content, sizeof(it->content), "%s", s);
            it->content_len = strlen(it->content);
        }
        s = (const char *)sqlite3_column_text(stmt, 4);
        if (s)
            snprintf(it->url, sizeof(it->url), "%s", s);
        it->ingested_at = sqlite3_column_int64(stmt, 5);
        count++;
    }
    sqlite3_finalize(stmt);

    *out = items;
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_feed_search(hu_allocator_t *alloc, sqlite3 *db, const char *query, size_t query_len,
                          size_t limit, hu_feed_item_stored_t **out, size_t *out_count) {
    (void)query_len;
    if (!alloc || !db || !query || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_count = 0;

    const char *sql = "SELECT source, contact_id, content_type, content, url, ingested_at "
                      "FROM feed_items WHERE id IN (SELECT rowid FROM feed_items_fts WHERE "
                      "feed_items_fts MATCH ?) ORDER BY ingested_at DESC LIMIT ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(stmt, 1, query, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, (int)limit);

    hu_feed_item_stored_t *items = NULL;
    size_t count = 0;
    size_t cap = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            size_t new_cap = cap == 0 ? 4 : cap * 2;
            hu_feed_item_stored_t *tmp = (hu_feed_item_stored_t *)alloc->alloc(
                alloc->ctx, new_cap * sizeof(hu_feed_item_stored_t));
            if (!tmp) {
                if (items)
                    alloc->free(alloc->ctx, items, cap * sizeof(hu_feed_item_stored_t));
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            if (items) {
                memcpy(tmp, items, count * sizeof(hu_feed_item_stored_t));
                alloc->free(alloc->ctx, items, cap * sizeof(hu_feed_item_stored_t));
            }
            items = tmp;
            cap = new_cap;
        }
        hu_feed_item_stored_t *it = &items[count];
        memset(it, 0, sizeof(*it));

        const char *s = (const char *)sqlite3_column_text(stmt, 0);
        if (s)
            snprintf(it->source, sizeof(it->source), "%s", s);
        s = (const char *)sqlite3_column_text(stmt, 1);
        if (s)
            snprintf(it->contact_id, sizeof(it->contact_id), "%s", s);
        s = (const char *)sqlite3_column_text(stmt, 2);
        if (s)
            snprintf(it->content_type, sizeof(it->content_type), "%s", s);
        s = (const char *)sqlite3_column_text(stmt, 3);
        if (s) {
            snprintf(it->content, sizeof(it->content), "%s", s);
            it->content_len = strlen(it->content);
        }
        s = (const char *)sqlite3_column_text(stmt, 4);
        if (s)
            snprintf(it->url, sizeof(it->url), "%s", s);
        it->ingested_at = sqlite3_column_int64(stmt, 5);
        count++;
    }
    sqlite3_finalize(stmt);

    *out = items;
    *out_count = count;
    return HU_OK;
}

hu_error_t hu_feed_build_daily_digest(hu_allocator_t *alloc, sqlite3 *db, int64_t since_ts,
                                      size_t max_chars, char **out, size_t *out_len) {
    if (!alloc || !db || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    *out = NULL;
    *out_len = 0;

    const char *count_sql = "SELECT source, COUNT(*) as cnt FROM feed_items WHERE ingested_at >= ? "
                            "GROUP BY source ORDER BY cnt DESC";
    sqlite3_stmt *count_stmt = NULL;
    if (sqlite3_prepare_v2(db, count_sql, -1, &count_stmt, NULL) != SQLITE_OK)
        goto no_items;

    sqlite3_bind_int64(count_stmt, 1, since_ts);

    size_t total_items = 0;
    size_t source_count = 0;
    char sources[32][32];
    int counts[32];
    memset(sources, 0, sizeof(sources));
    memset(counts, 0, sizeof(counts));

    while (sqlite3_step(count_stmt) == SQLITE_ROW && source_count < 32) {
        const char *src = (const char *)sqlite3_column_text(count_stmt, 0);
        int cnt = sqlite3_column_int(count_stmt, 1);
        if (src) {
            snprintf(sources[source_count], sizeof(sources[0]), "%s", src);
            counts[source_count] = cnt;
            total_items += (size_t)cnt;
            source_count++;
        }
    }
    sqlite3_finalize(count_stmt);
    count_stmt = NULL;

    if (source_count == 0)
        goto no_items;

    size_t need = 128;
    int hdr_n = snprintf(NULL, 0, "## Feed Digest (%zu items from %zu sources)\n\n", total_items,
                         source_count);
    if (hdr_n < 0)
        return HU_ERR_INVALID_ARGUMENT;
    if ((size_t)hdr_n > SIZE_MAX - need)
        return HU_ERR_INVALID_ARGUMENT;
    need += (size_t)hdr_n;

    char *buf = (char *)alloc->alloc(alloc->ctx, need);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    size_t pos = 0;
    int n = snprintf(buf + pos, need - pos, "## Feed Digest (%zu items from %zu sources)\n\n",
                     total_items, source_count);
    if (n < 0 || (size_t)n >= need - pos) {
        alloc->free(alloc->ctx, buf, need);
        return HU_ERR_INVALID_ARGUMENT;
    }
    pos += (size_t)n;

    const char *item_sql =
        "SELECT content_type, substr(content, 1, 200), url FROM feed_items "
        "WHERE source = ? AND ingested_at >= ? ORDER BY ingested_at DESC LIMIT 5";
    sqlite3_stmt *item_stmt = NULL;
    if (sqlite3_prepare_v2(db, item_sql, -1, &item_stmt, NULL) != SQLITE_OK) {
        alloc->free(alloc->ctx, buf, need);
        return HU_ERR_IO;
    }

    for (size_t si = 0; si < source_count && pos < max_chars; si++) {
        sqlite3_reset(item_stmt);
        sqlite3_bind_text(item_stmt, 1, sources[si], -1, SQLITE_STATIC);
        sqlite3_bind_int64(item_stmt, 2, since_ts);

        int row = 0;
        while (sqlite3_step(item_stmt) == SQLITE_ROW && pos < max_chars && row < 5) {
            const char *preview = (const char *)sqlite3_column_text(item_stmt, 1);
            const char *url = (const char *)sqlite3_column_text(item_stmt, 2);
            const char *prev_str = preview ? preview : "";
            const char *url_str = url ? url : "";

            size_t line_cap = 512;
            char line[512];
            n = snprintf(line, line_cap, "- [%s] %s (%s)\n", sources[si], prev_str, url_str);
            if (n > 0 && (size_t)n < line_cap) {
                size_t grow = (size_t)n + 1;
                char *new_buf = (char *)alloc->alloc(alloc->ctx, pos + grow);
                if (!new_buf) {
                    sqlite3_finalize(item_stmt);
                    alloc->free(alloc->ctx, buf, need);
                    return HU_ERR_OUT_OF_MEMORY;
                }
                memcpy(new_buf, buf, pos);
                memcpy(new_buf + pos, line, (size_t)n + 1);
                alloc->free(alloc->ctx, buf, need);
                buf = new_buf;
                need = pos + grow;
                pos += (size_t)n;
            }
            row++;
        }
    }
    sqlite3_finalize(item_stmt);

    *out = buf;
    *out_len = pos;
    return HU_OK;

no_items: {
    static const char empty[] = "(No recent items)";
    char *fallback = (char *)alloc->alloc(alloc->ctx, sizeof(empty));
    if (!fallback)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(fallback, empty, sizeof(empty));
    *out = fallback;
    *out_len = sizeof(empty) - 1;
    return HU_OK;
}
}

hu_error_t hu_feed_processor_cleanup(hu_feed_processor_t *proc, uint32_t retention_days) {
    if (!proc || !proc->db)
        return HU_ERR_INVALID_ARGUMENT;

    int64_t cutoff = (int64_t)time(NULL) - (int64_t)(retention_days * 86400u);
    const char *sql = "DELETE FROM feed_items WHERE ingested_at < ?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(proc->db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_int64(stmt, 1, cutoff);
    int rc = sqlite3_step(stmt);
    int deleted = sqlite3_changes(proc->db);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE)
        return HU_ERR_IO;
    hu_log_info("feed", NULL, "feed_processor_cleanup: deleted %d rows", deleted);
    return HU_OK;
}

hu_error_t hu_feed_correlate_recent(hu_allocator_t *alloc, sqlite3 *db, int64_t since_ts,
                                    double threshold) {
    if (!alloc || !db)
        return HU_ERR_INVALID_ARGUMENT;
    if (threshold <= 0.0)
        threshold = 0.3;

    const char *sql = "SELECT id, source, content FROM feed_items "
                      "WHERE ingested_at >= ? AND cluster_id IS NULL "
                      "ORDER BY ingested_at DESC LIMIT 200";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_int64(stmt, 1, since_ts);

    typedef struct {
        int64_t id;
        char source[32];
        char content[512];
    } row_t;
    row_t *rows = NULL;
    size_t count = 0, cap = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        if (count >= cap) {
            size_t nc = cap == 0 ? 16 : cap * 2;
            row_t *tmp = (row_t *)alloc->alloc(alloc->ctx, nc * sizeof(row_t));
            if (!tmp) {
                if (rows)
                    alloc->free(alloc->ctx, rows, cap * sizeof(row_t));
                sqlite3_finalize(stmt);
                return HU_ERR_OUT_OF_MEMORY;
            }
            if (rows) {
                memcpy(tmp, rows, count * sizeof(row_t));
                alloc->free(alloc->ctx, rows, cap * sizeof(row_t));
            }
            rows = tmp;
            cap = nc;
        }
        row_t *r = &rows[count];
        memset(r, 0, sizeof(*r));
        r->id = sqlite3_column_int64(stmt, 0);
        const char *s = (const char *)sqlite3_column_text(stmt, 1);
        if (s)
            snprintf(r->source, sizeof(r->source), "%s", s);
        s = (const char *)sqlite3_column_text(stmt, 2);
        if (s)
            snprintf(r->content, sizeof(r->content), "%s", s);
        count++;
    }
    sqlite3_finalize(stmt);

    if (count < 2) {
        if (rows)
            alloc->free(alloc->ctx, rows, cap * sizeof(row_t));
        return HU_OK;
    }

    int64_t next_cluster = (int64_t)time(NULL);
    const char *update_sql = "UPDATE feed_items SET cluster_id = ? WHERE id = ?";
    size_t clusters_formed = 0;
    int *assigned = (int *)alloc->alloc(alloc->ctx, count * sizeof(int));
    if (!assigned) {
        alloc->free(alloc->ctx, rows, cap * sizeof(row_t));
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(assigned, 0, count * sizeof(int));

    for (size_t i = 0; i < count; i++) {
        if (assigned[i])
            continue;
        int has_cluster = 0;
        for (size_t j = i + 1; j < count; j++) {
            if (assigned[j])
                continue;
            if (strcmp(rows[i].source, rows[j].source) == 0)
                continue;
            double score = hu_feeds_score_relevance(rows[j].content, strlen(rows[j].content),
                                                    rows[i].content, strlen(rows[i].content));
            if (score >= threshold) {
                if (!has_cluster) {
                    sqlite3_stmt *us = NULL;
                    if (sqlite3_prepare_v2(db, update_sql, -1, &us, NULL) == SQLITE_OK) {
                        sqlite3_bind_int64(us, 1, next_cluster);
                        sqlite3_bind_int64(us, 2, rows[i].id);
                        sqlite3_step(us);
                        sqlite3_finalize(us);
                    }
                    assigned[i] = 1;
                    has_cluster = 1;
                }
                sqlite3_stmt *us = NULL;
                if (sqlite3_prepare_v2(db, update_sql, -1, &us, NULL) == SQLITE_OK) {
                    sqlite3_bind_int64(us, 1, next_cluster);
                    sqlite3_bind_int64(us, 2, rows[j].id);
                    sqlite3_step(us);
                    sqlite3_finalize(us);
                }
                assigned[j] = 1;
            }
        }
        if (has_cluster) {
            clusters_formed++;
            next_cluster++;
        }
    }

    alloc->free(alloc->ctx, assigned, count * sizeof(int));
    alloc->free(alloc->ctx, rows, cap * sizeof(row_t));
    if (clusters_formed > 0)
        hu_log_info("feeds", NULL, "correlated %zu clusters from %zu items", clusters_formed,
                    count);
    return HU_OK;
}

hu_error_t hu_feed_semantic_search(hu_allocator_t *alloc, sqlite3 *db, hu_embedder_t *embedder,
                                   hu_vector_store_t *store, const char *query, size_t query_len,
                                   size_t limit, hu_feed_item_stored_t **out, size_t *out_count) {
    if (!alloc || !db || !embedder || !store || !query || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    hu_embedding_t qe = {0};
    hu_error_t err = embedder->vtable->embed(embedder->ctx, alloc, query, query_len, &qe);
    if (err != HU_OK)
        return err;

    hu_vector_entry_t *entries = NULL;
    size_t entry_count = 0;
    err = store->vtable->search(store->ctx, alloc, &qe, limit, &entries, &entry_count);
    hu_embedding_free(alloc, &qe);
    if (err != HU_OK)
        return err;

    if (entry_count == 0) {
        hu_vector_entries_free(alloc, entries, 0);
        return HU_OK;
    }

    hu_feed_item_stored_t *items = (hu_feed_item_stored_t *)alloc->alloc(
        alloc->ctx, entry_count * sizeof(hu_feed_item_stored_t));
    if (!items) {
        hu_vector_entries_free(alloc, entries, entry_count);
        return HU_ERR_OUT_OF_MEMORY;
    }

    size_t found = 0;
    const char *sql = "SELECT source, contact_id, content_type, content, url, ingested_at "
                      "FROM feed_items WHERE id = ?";
    for (size_t i = 0; i < entry_count; i++) {
        if (!entries[i].id)
            continue;
        int64_t row_id = 0;
        for (const char *p = entries[i].id; *p >= '0' && *p <= '9'; p++)
            row_id = row_id * 10 + (*p - '0');
        if (row_id == 0)
            continue;

        sqlite3_stmt *stmt = NULL;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
            continue;
        sqlite3_bind_int64(stmt, 1, row_id);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            hu_feed_item_stored_t *it = &items[found];
            memset(it, 0, sizeof(*it));
            const char *s = (const char *)sqlite3_column_text(stmt, 0);
            if (s)
                snprintf(it->source, sizeof(it->source), "%s", s);
            s = (const char *)sqlite3_column_text(stmt, 1);
            if (s)
                snprintf(it->contact_id, sizeof(it->contact_id), "%s", s);
            s = (const char *)sqlite3_column_text(stmt, 2);
            if (s)
                snprintf(it->content_type, sizeof(it->content_type), "%s", s);
            s = (const char *)sqlite3_column_text(stmt, 3);
            if (s) {
                snprintf(it->content, sizeof(it->content), "%s", s);
                it->content_len = strlen(it->content);
            }
            s = (const char *)sqlite3_column_text(stmt, 4);
            if (s)
                snprintf(it->url, sizeof(it->url), "%s", s);
            it->ingested_at = sqlite3_column_int64(stmt, 5);
            found++;
        }
        sqlite3_finalize(stmt);
    }

    hu_vector_entries_free(alloc, entries, entry_count);
    *out = items;
    *out_count = found;
    return HU_OK;
}

#endif /* HU_ENABLE_SQLITE */

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
