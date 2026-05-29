#include "human/context/context_ext.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define HU_CONTEXT_EXT_ESCAPE_BUF 1024
#define HU_CONTEXT_EXT_SQL_BUF 4096

/* --- F47: Content Forwarding --- */
static void escape_sql_string(const char *s, size_t len, char *buf, size_t cap, size_t *out_len) {
    (void)hu_sql_quote_escape_into(s, len, buf, cap, out_len);
}

hu_error_t hu_forwarding_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    static const char sql[] =
        "CREATE TABLE IF NOT EXISTS shareable_content (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    content TEXT NOT NULL,\n"
        "    source TEXT NOT NULL,\n"
        "    topic TEXT NOT NULL,\n"
        "    received_at INTEGER NOT NULL,\n"
        "    share_score REAL NOT NULL,\n"
        "    shared_with TEXT\n"
        ")";
    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_forwarding_insert_sql(const hu_shareable_content_t *c, char *buf, size_t cap,
                                    size_t *out_len) {
    if (!c || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    if (!c->content || !c->source || !c->topic)
        return HU_ERR_INVALID_ARGUMENT;

    char content_esc[HU_CONTEXT_EXT_ESCAPE_BUF];
    char source_esc[HU_CONTEXT_EXT_ESCAPE_BUF];
    char topic_esc[HU_CONTEXT_EXT_ESCAPE_BUF];
    size_t ce_len, se_len, te_len;
    escape_sql_string(c->content, c->content_len, content_esc, sizeof(content_esc), &ce_len);
    escape_sql_string(c->source, c->source_len, source_esc, sizeof(source_esc), &se_len);
    escape_sql_string(c->topic, c->topic_len, topic_esc, sizeof(topic_esc), &te_len);

    int n = snprintf(buf, cap,
                     "INSERT INTO shareable_content (content, source, topic, received_at, "
                     "share_score, shared_with) VALUES ('%s', '%s', '%s', %llu, %f, NULL)",
                     content_esc, source_esc, topic_esc, (unsigned long long)c->received_at,
                     c->share_score);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_forwarding_query_for_contact_sql(const char *contact_id, size_t len, char *buf,
                                                size_t cap, size_t *out_len) {
    if (!contact_id || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_CONTEXT_EXT_ESCAPE_BUF];
    size_t ce_len;
    escape_sql_string(contact_id, len, contact_esc, sizeof(contact_esc), &ce_len);

    int n = snprintf(buf, cap,
                     "SELECT id, content, source, topic, received_at, share_score, shared_with "
                     "FROM shareable_content WHERE (shared_with IS NULL OR shared_with NOT LIKE "
                     "'%%' || '%s' || '%%') ORDER BY share_score DESC LIMIT 20",
                     contact_esc);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

double hu_forwarding_score(bool topic_match, double contact_closeness,
                           uint32_t hours_since_received, bool already_shared) {
    double freshness = 1.0 - (double)hours_since_received / 168.0;
    if (freshness < 0.0)
        freshness = 0.0;
    return (topic_match ? 0.4 : 0.0) + contact_closeness * 0.3 + freshness * 0.2 +
           (already_shared ? 0.0 : 0.1);
}

void hu_shareable_content_deinit(hu_allocator_t *alloc, hu_shareable_content_t *c) {
    if (!alloc || !c)
        return;
    if (c->content) {
        alloc->free(alloc->ctx, c->content, c->content_len + 1);
        c->content = NULL;
        c->content_len = 0;
    }
    if (c->source) {
        alloc->free(alloc->ctx, c->source, c->source_len + 1);
        c->source = NULL;
        c->source_len = 0;
    }
    if (c->topic) {
        alloc->free(alloc->ctx, c->topic, c->topic_len + 1);
        c->topic = NULL;
        c->topic_len = 0;
    }
}

/* --- F51: Weather Context --- */
static int ctx_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char ca = (unsigned char)a[i];
        unsigned char cb = (unsigned char)b[i];
        int da = (ca >= 'A' && ca <= 'Z') ? ca - 'A' + 'a' : ca;
        int db = (cb >= 'A' && cb <= 'Z') ? cb - 'A' + 'a' : cb;
        if (da != db)
            return da - db;
    }
    return 0;
}


bool hu_weather_is_notable(const char *condition, size_t len, double temp_f) {
    if (condition && len > 0) {
        if (hu_str_contains_ci_cstr(condition, len, "snow") || hu_str_contains_ci_cstr(condition, len, "storm") ||
            hu_str_contains_ci_cstr(condition, len, "hurricane"))
            return true;
    }
    if (temp_f > 100.0 || temp_f < 10.0)
        return true;
    return false;
}

hu_error_t hu_weather_build_directive(hu_allocator_t *alloc, const hu_weather_state_t *w,
                                      char **out, size_t *out_len) {
    if (!alloc || !w || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (!w->is_notable)
        return HU_OK;

    const char *cond = w->condition && w->condition_len > 0 ? w->condition : "unknown";
    size_t cond_len = w->condition_len > 0 ? w->condition_len : 7;
    int n = snprintf(NULL, 0, "[WEATHER: It's %.*s. Perfect conversation starter.]",
                     (int)cond_len, cond);
    if (n < 0)
        return HU_ERR_INVALID_ARGUMENT;
    size_t need = (size_t)n + 1;
    char *buf = alloc->alloc(alloc->ctx, need);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    (void)snprintf(buf, need, "[WEATHER: It's %.*s. Perfect conversation starter.]",
                  (int)cond_len, cond);
    *out = buf;
    *out_len = (size_t)n;
    return HU_OK;
}

void hu_weather_state_deinit(hu_allocator_t *alloc, hu_weather_state_t *w) {
    if (!alloc || !w)
        return;
    if (w->condition) {
        alloc->free(alloc->ctx, w->condition, w->condition_len + 1);
        w->condition = NULL;
        w->condition_len = 0;
    }
    if (w->location) {
        alloc->free(alloc->ctx, w->location, w->location_len + 1);
        w->location = NULL;
        w->location_len = 0;
    }
}

/* --- F52: Current Events --- */
hu_error_t hu_events_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    static const char sql[] =
        "CREATE TABLE IF NOT EXISTS current_events (\n"
        "    id INTEGER PRIMARY KEY AUTOINCREMENT,\n"
        "    topic TEXT NOT NULL,\n"
        "    summary TEXT NOT NULL,\n"
        "    source TEXT NOT NULL,\n"
        "    published_at INTEGER NOT NULL,\n"
        "    relevance REAL NOT NULL\n"
        ")";
    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_events_insert_sql(const hu_current_event_t *e, char *buf, size_t cap,
                                size_t *out_len) {
    if (!e || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    if (!e->topic || !e->summary || !e->source)
        return HU_ERR_INVALID_ARGUMENT;

    char topic_esc[HU_CONTEXT_EXT_ESCAPE_BUF];
    char summary_esc[HU_CONTEXT_EXT_ESCAPE_BUF];
    char source_esc[HU_CONTEXT_EXT_ESCAPE_BUF];
    size_t te_len, se_len, sce_len;
    escape_sql_string(e->topic, e->topic_len, topic_esc, sizeof(topic_esc), &te_len);
    escape_sql_string(e->summary, e->summary_len, summary_esc, sizeof(summary_esc), &se_len);
    escape_sql_string(e->source, e->source_len, source_esc, sizeof(source_esc), &sce_len);

    int n = snprintf(buf, cap,
                     "INSERT INTO current_events (topic, summary, source, published_at, relevance) "
                     "VALUES ('%s', '%s', '%s', %llu, %f)",
                     topic_esc, summary_esc, source_esc,
                     (unsigned long long)e->published_at, e->relevance);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_events_build_prompt(hu_allocator_t *alloc, const hu_current_event_t *events,
                                  size_t count, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    if (count == 0 || !events) {
        static const char empty[] = "[No notable current events]";
        char *buf = hu_strndup(alloc, empty, sizeof(empty) - 1);
        if (!buf)
            return HU_ERR_OUT_OF_MEMORY;
        *out = buf;
        *out_len = sizeof(empty) - 1;
        return HU_OK;
    }

    size_t total = 64;
    for (size_t i = 0; i < count; i++) {
        total += 4 + (events[i].topic ? events[i].topic_len : 0) + 4 +
                 (events[i].summary ? events[i].summary_len : 0) + 4;
    }
    total += 64;

    char *buf = alloc->alloc(alloc->ctx, total);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;

    int n0 = snprintf(buf, total, "[CURRENT EVENTS you're aware of]:");
    size_t pos = (n0 > 0 && (size_t)n0 < total) ? (size_t)n0 : 0;
    for (size_t i = 0; i < count && pos + 16 < total; i++) {
        const char *topic = events[i].topic ? events[i].topic : "";
        size_t topic_len = events[i].topic_len;
        const char *summary = events[i].summary ? events[i].summary : "";
        size_t summary_len = events[i].summary_len;
        int n = snprintf(buf + pos, total - pos, "\n- [%.*s]: %.*s", (int)topic_len, topic,
                         (int)summary_len, summary);
        if (n > 0 && (size_t)n < total - pos)
            pos += (size_t)n;
    }

    *out = buf;
    *out_len = pos;
    return HU_OK;
}

void hu_current_event_deinit(hu_allocator_t *alloc, hu_current_event_t *e) {
    if (!alloc || !e)
        return;
    if (e->topic) {
        alloc->free(alloc->ctx, e->topic, e->topic_len + 1);
        e->topic = NULL;
        e->topic_len = 0;
    }
    if (e->summary) {
        alloc->free(alloc->ctx, e->summary, e->summary_len + 1);
        e->summary = NULL;
        e->summary_len = 0;
    }
    if (e->source) {
        alloc->free(alloc->ctx, e->source, e->source_len + 1);
        e->source = NULL;
        e->source_len = 0;
    }
}

void hu_current_events_free(hu_allocator_t *alloc, hu_current_event_t *events, size_t count) {
    if (!alloc || !events)
        return;
    for (size_t i = 0; i < count; i++)
        hu_current_event_deinit(alloc, &events[i]);
    alloc->free(alloc->ctx, events, count * sizeof(hu_current_event_t));
}

#ifdef HU_IS_TEST

hu_error_t hu_current_events_fetch(hu_allocator_t *alloc, const char *const *topics,
                                   size_t topic_count, hu_current_event_t **out,
                                   size_t *out_count) {
    if (!alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (!topics || topic_count == 0)
        return HU_OK;

    hu_current_event_t *events = (hu_current_event_t *)alloc->alloc(
        alloc->ctx, sizeof(hu_current_event_t));
    if (!events)
        return HU_ERR_OUT_OF_MEMORY;
    memset(events, 0, sizeof(hu_current_event_t));
    events[0].topic = hu_strndup(alloc, topics[0], strlen(topics[0]));
    events[0].topic_len = strlen(topics[0]);
    events[0].summary = hu_strndup(alloc, "Mock event for testing", 22);
    events[0].summary_len = 22;
    events[0].source = hu_strndup(alloc, "test", 4);
    events[0].source_len = 4;
    events[0].published_at = 0;
    events[0].relevance = 0.8;
    if (!events[0].topic || !events[0].summary || !events[0].source) {
        hu_current_events_free(alloc, events, 1);
        return HU_ERR_OUT_OF_MEMORY;
    }
    *out = events;
    *out_count = 1;
    return HU_OK;
}

#elif defined(HU_ENABLE_FEEDS)

#include "human/feeds/news.h"

static const char *topic_to_rss_url(const char *topic) {
    if (!topic)
        return NULL;
    if (strcasecmp(topic, "tech") == 0 || strcasecmp(topic, "technology") == 0)
        return "https://feeds.arstechnica.com/arstechnica/technology-lab";
    if (strcasecmp(topic, "sports") == 0)
        return "https://rss.nytimes.com/services/xml/rss/nyt/Sports.xml";
    if (strcasecmp(topic, "science") == 0)
        return "https://rss.nytimes.com/services/xml/rss/nyt/Science.xml";
    if (strcasecmp(topic, "business") == 0)
        return "https://feeds.bbci.co.uk/news/business/rss.xml";
    if (strcasecmp(topic, "world") == 0 || strcasecmp(topic, "news") == 0)
        return "https://feeds.bbci.co.uk/news/world/rss.xml";
    return NULL;
}

hu_error_t hu_current_events_fetch(hu_allocator_t *alloc, const char *const *topics,
                                   size_t topic_count, hu_current_event_t **out,
                                   size_t *out_count) {
    if (!alloc || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (!topics || topic_count == 0)
        return HU_OK;

    size_t cap = topic_count * 3;
    if (cap > 20) cap = 20;
    hu_current_event_t *events = (hu_current_event_t *)alloc->alloc(
        alloc->ctx, cap * sizeof(hu_current_event_t));
    if (!events)
        return HU_ERR_OUT_OF_MEMORY;
    memset(events, 0, cap * sizeof(hu_current_event_t));
    size_t count = 0;

    for (size_t t = 0; t < topic_count && count < cap; t++) {
        const char *url = topic_to_rss_url(topics[t]);
        if (!url)
            continue;
        hu_rss_article_t articles[5];
        size_t article_count = 0;
        if (hu_news_fetch_rss(alloc, url, strlen(url), articles, 5, &article_count) != HU_OK)
            continue;
        for (size_t a = 0; a < article_count && count < cap; a++) {
            hu_current_event_t *ev = &events[count];
            ev->topic = hu_strndup(alloc, topics[t], strlen(topics[t]));
            ev->topic_len = strlen(topics[t]);
            size_t dlen = strlen(articles[a].description);
            if (dlen > 200) dlen = 200;
            ev->summary = hu_strndup(alloc, articles[a].description, dlen);
            ev->summary_len = dlen;
            size_t tlen = strlen(articles[a].title);
            ev->source = hu_strndup(alloc, articles[a].title, tlen);
            ev->source_len = tlen;
            ev->published_at = articles[a].pub_date > 0
                                   ? (uint64_t)articles[a].pub_date
                                   : 0;
            ev->relevance = 0.5;
            if (!ev->topic || !ev->summary || !ev->source) {
                hu_current_events_free(alloc, events, count);
                return HU_ERR_OUT_OF_MEMORY;
            }
            count++;
        }
    }

    if (count == 0) {
        alloc->free(alloc->ctx, events, cap * sizeof(hu_current_event_t));
        return HU_OK;
    }
    if (count < cap) {
        hu_current_event_t *shrunk = (hu_current_event_t *)alloc->realloc(
            alloc->ctx, events, cap * sizeof(hu_current_event_t),
            count * sizeof(hu_current_event_t));
        if (shrunk)
            events = shrunk;
    }
    *out = events;
    *out_count = count;
    return HU_OK;
}

#else

hu_error_t hu_current_events_fetch(hu_allocator_t *alloc, const char *const *topics,
                                   size_t topic_count, hu_current_event_t **out,
                                   size_t *out_count) {
    (void)alloc;
    (void)topics;
    (void)topic_count;
    if (out) *out = NULL;
    if (out_count) *out_count = 0;
    return HU_ERR_NOT_SUPPORTED;
}

#endif

/* --- F55-F57: Group Chat --- */
/* LCG: state = (a * state + c) mod m. glibc-style constants. */
static uint32_t lcg_next(uint32_t *state) {
    *state = (uint32_t)((1103515245ULL * (uint32_t)*state + 12345ULL) & 0x7FFFFFFF);
    return *state;
}

bool hu_group_should_respond(const hu_group_chat_state_t *state, uint32_t seed) {
    if (!state)
        return false;
    if (state->was_mentioned || state->has_direct_question)
        return true;

    double prob = state->response_rate * (1.0 / (state->active_participants > 0
                                                     ? (double)state->active_participants
                                                     : 1.0));
    if (prob >= 1.0)
        return true;
    if (prob <= 0.0)
        return false;

    uint32_t s = seed;
    lcg_next(&s);
    double r = (double)lcg_next(&s) / 2147483648.0;
    return r < prob;
}

bool hu_group_should_mention(const char *message, size_t msg_len, const char *contact_name,
                             size_t name_len) {
    if (!message || !contact_name || name_len == 0 || msg_len < name_len)
        return false;
    for (size_t i = 0; i <= msg_len - name_len; i++) {
        if (ctx_strncasecmp(message + i, contact_name, name_len) == 0)
            return true;
    }
    return false;
}

hu_error_t hu_group_build_directive(hu_allocator_t *alloc,
                                    const hu_group_chat_state_t *state, char **out,
                                    size_t *out_len) {
    if (!alloc || !state || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;

    const char *mentioned = state->was_mentioned ? "yes" : "no";
    int n = snprintf(NULL, 0,
                     "[GROUP CHAT RULES]: %u active participants. Mentioned: %s. Keep responses "
                     "brief. Don't dominate.",
                     (unsigned)state->active_participants, mentioned);
    if (n < 0)
        return HU_ERR_INVALID_ARGUMENT;
    size_t need = (size_t)n + 1;
    char *buf = alloc->alloc(alloc->ctx, need);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    (void)snprintf(buf, need,
                  "[GROUP CHAT RULES]: %u active participants. Mentioned: %s. Keep responses "
                  "brief. Don't dominate.",
                  (unsigned)state->active_participants, mentioned);
    *out = buf;
    *out_len = (size_t)n;
    return HU_OK;
}
