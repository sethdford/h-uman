/* Phase 7 News/RSS feed. F90. */
#ifdef HU_ENABLE_FEEDS

#include "human/feeds/news.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include "human/core/time.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if HU_IS_TEST

hu_error_t hu_news_fetch_rss(hu_allocator_t *alloc, const char *feed_url, size_t url_len,
                             hu_rss_article_t *articles, size_t cap, size_t *out_count) {
    (void)alloc;
    (void)feed_url;
    (void)url_len;
    if (!articles || !out_count || cap < 2)
        return HU_ERR_INVALID_ARGUMENT;
    memset(articles, 0, sizeof(hu_rss_article_t) * 2);
    (void)strncpy(articles[0].title, "Tech News: AI Advances", sizeof(articles[0].title) - 1);
    (void)strncpy(articles[0].link, "https://example.com/article/1", sizeof(articles[0].link) - 1);
    (void)strncpy(articles[0].description, "Latest in AI research",
                  sizeof(articles[0].description) - 1);
    articles[0].pub_date = (int64_t)time(NULL);
    (void)strncpy(articles[1].title, "Tech News: AI Advances", sizeof(articles[1].title) - 1);
    (void)strncpy(articles[1].link, "https://example.com/article/2", sizeof(articles[1].link) - 1);
    (void)strncpy(articles[1].description, "Latest in AI research",
                  sizeof(articles[1].description) - 1);
    articles[1].pub_date = (int64_t)time(NULL);
    *out_count = 2;
    return HU_OK;
}

#else

static const char *bounded_strstr(const char *hay, size_t hay_len, const char *needle) {
    size_t nlen = strlen(needle);
    if (nlen > hay_len)
        return NULL;
    for (size_t i = 0; i <= hay_len - nlen; i++) {
        if (memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    }
    return NULL;
}

/* Simple substring extraction: find content between <tag> and </tag>. */
static const char *find_tag_content(const char *xml, size_t xml_len, const char *tag,
                                    size_t *out_len) {
    char open[64], close[64];
    size_t tag_len = strlen(tag);
    if (tag_len >= sizeof(open) - 2)
        return NULL;
    (void)snprintf(open, sizeof(open), "<%s>", tag);
    (void)snprintf(close, sizeof(close), "</%s>", tag);
    size_t open_len = strlen(open);
    const char *p = bounded_strstr(xml, xml_len, open);
    if (!p)
        return NULL;
    p += open_len;
    size_t remain = xml_len - (size_t)(p - xml);
    const char *q = bounded_strstr(p, remain, close);
    if (!q)
        return NULL;
    *out_len = (size_t)(q - p);
    return p;
}

/* Find next <item> or <entry> start. */
static const char *find_next_item(const char *xml, size_t xml_len, const char *pos, int *is_atom) {
    (void)xml;
    (void)xml_len;
    const char *item = strstr(pos, "<item>");
    const char *entry = strstr(pos, "<entry>");
    if (item && (!entry || item < entry)) {
        *is_atom = 0;
        return item + 6;
    }
    if (entry) {
        *is_atom = 1;
        return entry + 7;
    }
    return NULL;
}

/* Find end of current item/entry. */
static const char *find_item_end(const char *xml, size_t xml_len, const char *pos, int is_atom) {
    const char *end_tag = is_atom ? "</entry>" : "</item>";
    const char *e = strstr(pos, end_tag);
    return e ? e : xml + xml_len;
}

/* Parse RFC 2822 or ISO 8601 date to Unix timestamp. Returns 0 on failure. */
static int64_t parse_pub_date(const char *s, size_t len) {
    if (!s || len == 0)
        return 0;
    char buf[64];
    if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
    memcpy(buf, s, len);
    buf[len] = '\0';
    /* Trim whitespace */
    while (len > 0 && (buf[len - 1] == ' ' || buf[len - 1] == '\t'))
        buf[--len] = '\0';
    const char *p = buf;
    while (*p == ' ' || *p == '\t')
        p++;
    struct tm tm = {0};
    int year, month, day, hour, min, sec;
    if (sscanf(p, "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &min, &sec) >= 6) {
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        tm.tm_hour = hour;
        tm.tm_min = min;
        tm.tm_sec = sec;
        return (int64_t)timegm(&tm);
    }
    if (sscanf(p, "%d-%d-%d", &year, &month, &day) >= 3) {
        tm.tm_year = year - 1900;
        tm.tm_mon = month - 1;
        tm.tm_mday = day;
        return (int64_t)timegm(&tm);
    }
    return 0;
}

/* Extract link from Atom <link href="..."/> */
static void extract_atom_link(const char *item_start, const char *item_end, char *out_link,
                              size_t link_cap) {
    out_link[0] = '\0';
    const char *p = item_start;
    while (p < item_end) {
        const char *link = strstr(p, "<link");
        if (!link || link >= item_end)
            break;
        const char *href = strstr(link, "href=\"");
        if (href && href < item_end && href < link + 128) {
            href += 6;
            const char *q = strchr(href, '"');
            if (q && q - href < (ptrdiff_t)link_cap) {
                size_t len = (size_t)(q - href);
                if (len >= link_cap)
                    len = link_cap - 1;
                memcpy(out_link, href, len);
                out_link[len] = '\0';
                return;
            }
        }
        p = link + 1;
    }
}

hu_error_t hu_news_fetch_rss(hu_allocator_t *alloc, const char *feed_url, size_t url_len,
                             hu_rss_article_t *articles, size_t cap, size_t *out_count) {
    if (!alloc || !feed_url || !articles || !out_count || cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

    char url_buf[1024];
    if (url_len >= sizeof(url_buf))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(url_buf, feed_url, url_len);
    url_buf[url_len] = '\0';

    /* A feed that refused us recently is not asked again yet (hnrss.org
     * answered 429/502 107 times in a week of 30-minute polls). */
    uint64_t now_ms = (uint64_t)hu_time_wall_ms();
    if (hu_news_backoff_active(url_buf, now_ms))
        return HU_ERR_IO;

    hu_http_response_t resp = {0};
    /* Publishers relocate feeds behind 301/307 and leave the old URL
     * redirecting for years; follow a few HTTPS hops instead of logging
     * "HTTP 307" on every poll (openai.com/blog/rss.xml did, 2026-09). */
    hu_error_t err = hu_http_get_follow(alloc, url_buf, NULL, 3, &resp);
    if (err != HU_OK)
        return err;
    hu_news_backoff_record(url_buf, (int)resp.status_code, now_ms);
    if (!resp.body || resp.status_code != 200) {
        hu_http_response_free(alloc, &resp);
        return HU_ERR_IO;
    }

    const char *xml = resp.body;
    size_t xml_len = resp.body_len;
    const char *pos = xml;
    size_t n = 0;

    while (n < cap) {
        int is_atom = 0;
        const char *item_start = find_next_item(xml, xml_len, pos, &is_atom);
        if (!item_start)
            break;
        const char *item_end = find_item_end(xml, xml_len, item_start, is_atom);
        size_t item_len = (size_t)(item_end - item_start);

        hu_rss_article_t *a = &articles[n];
        memset(a, 0, sizeof(*a));

        size_t len = 0;
        const char *title_s = find_tag_content(item_start, item_len, "title", &len);
        if (title_s && len > 0) {
            if (len >= sizeof(a->title))
                len = sizeof(a->title) - 1;
            memcpy(a->title, title_s, len);
            a->title[len] = '\0';
        }

        if (is_atom) {
            extract_atom_link(item_start, item_end, a->link, sizeof(a->link));
        }
        if (!a->link[0]) {
            const char *link_s = find_tag_content(item_start, item_len, "link", &len);
            if (link_s && len > 0) {
                if (len >= sizeof(a->link))
                    len = sizeof(a->link) - 1;
                memcpy(a->link, link_s, len);
                a->link[len] = '\0';
            }
        }

        const char *desc_s = find_tag_content(item_start, item_len, "description", &len);
        if (!desc_s && is_atom)
            desc_s = find_tag_content(item_start, item_len, "summary", &len);
        if (!desc_s && is_atom)
            desc_s = find_tag_content(item_start, item_len, "content", &len);
        if (desc_s && len > 0) {
            if (len >= sizeof(a->description))
                len = sizeof(a->description) - 1;
            memcpy(a->description, desc_s, len);
            a->description[len] = '\0';
        }

        const char *date_s = find_tag_content(item_start, item_len, "pubDate", &len);
        if (!date_s && is_atom)
            date_s = find_tag_content(item_start, item_len, "updated", &len);
        if (date_s && len > 0)
            a->pub_date = parse_pub_date(date_s, len);

        n++;
        pos = item_end;
    }

    hu_http_response_free(alloc, &resp);
    *out_count = n;
    return HU_OK;
}

#endif /* HU_IS_TEST */

#else
typedef int hu_news_stub_avoid_empty_tu;
#endif /* HU_ENABLE_FEEDS */

#ifdef HU_ENABLE_FEEDS
/* ── Per-URL backoff ───────────────────────────────────────────────── */

#define NEWS_BACKOFF_MIN_S (2u * 3600u)
#define NEWS_BACKOFF_MAX_S (24u * 3600u)
#define NEWS_BACKOFF_SLOTS 32

static struct {
    uint32_t hash; /* 0 = free */
    uint64_t until_ms;
    uint32_t step_s;
} s_backoff[NEWS_BACKOFF_SLOTS];

static uint32_t news_url_hash(const char *u) {
    uint32_t h = 2166136261u;
    for (; u && *u; u++)
        h = (h ^ (uint8_t)*u) * 16777619u;
    return h ? h : 1u;
}

uint32_t hu_news_backoff_secs(int status, uint32_t prev_secs) {
    if (status != 429 && (status < 500 || status > 599))
        return 0;
    uint32_t next = prev_secs ? prev_secs * 2u : NEWS_BACKOFF_MIN_S;
    if (next < NEWS_BACKOFF_MIN_S)
        next = NEWS_BACKOFF_MIN_S;
    return next > NEWS_BACKOFF_MAX_S ? NEWS_BACKOFF_MAX_S : next;
}

static int news_backoff_find(uint32_t h) {
    for (int i = 0; i < NEWS_BACKOFF_SLOTS; i++)
        if (s_backoff[i].hash == h)
            return i;
    return -1;
}

bool hu_news_backoff_active(const char *url, uint64_t now_ms) {
    int i = news_backoff_find(news_url_hash(url));
    return i >= 0 && now_ms < s_backoff[i].until_ms;
}

void hu_news_backoff_record(const char *url, int status, uint64_t now_ms) {
    uint32_t h = news_url_hash(url);
    int i = news_backoff_find(h);
    uint32_t secs = hu_news_backoff_secs(status, i >= 0 ? s_backoff[i].step_s : 0);
    if (secs == 0) { /* answered: forget any backoff */
        if (i >= 0)
            memset(&s_backoff[i], 0, sizeof(s_backoff[i]));
        return;
    }
    if (i < 0) { /* a free slot, else the one expiring first */
        i = 0;
        for (int j = 0; j < NEWS_BACKOFF_SLOTS; j++) {
            if (s_backoff[j].hash == 0) {
                i = j;
                break;
            }
            if (s_backoff[j].until_ms < s_backoff[i].until_ms)
                i = j;
        }
    }
    s_backoff[i].hash = h;
    s_backoff[i].step_s = secs;
    s_backoff[i].until_ms = now_ms + (uint64_t)secs * 1000u;
}
#endif
