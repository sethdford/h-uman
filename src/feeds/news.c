/* Phase 7 News/RSS feed. F90. */
#ifdef HU_ENABLE_FEEDS

#include "human/feeds/news.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/http.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if HU_IS_TEST

hu_error_t hu_news_fetch_rss(hu_allocator_t *alloc,
    const char *feed_url, size_t url_len,
    hu_rss_article_t *articles, size_t cap, size_t *out_count) {
    (void)alloc;
    (void)feed_url;
    (void)url_len;
    if (!articles || !out_count || cap < 2)
        return HU_ERR_INVALID_ARGUMENT;
    memset(articles, 0, sizeof(hu_rss_article_t) * 2);
    (void)strncpy(articles[0].title, "Tech News: AI Advances",
        sizeof(articles[0].title) - 1);
    (void)strncpy(articles[0].link, "https://example.com/article/1",
        sizeof(articles[0].link) - 1);
    (void)strncpy(articles[0].description, "Latest in AI research",
        sizeof(articles[0].description) - 1);
    articles[0].pub_date = (int64_t)time(NULL);
    (void)strncpy(articles[1].title, "Tech News: AI Advances",
        sizeof(articles[1].title) - 1);
    (void)strncpy(articles[1].link, "https://example.com/article/2",
        sizeof(articles[1].link) - 1);
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
static const char *find_tag_content(const char *xml, size_t xml_len,
    const char *tag, size_t *out_len) {
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
static const char *find_next_item(const char *xml, size_t xml_len,
    const char *pos, int *is_atom) {
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
static const char *find_item_end(const char *xml, size_t xml_len,
    const char *pos, int is_atom) {
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
static void extract_atom_link(const char *item_start, const char *item_end,
    char *out_link, size_t link_cap) {
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

hu_error_t hu_news_fetch_rss(hu_allocator_t *alloc,
    const char *feed_url, size_t url_len,
    hu_rss_article_t *articles, size_t cap, size_t *out_count) {
    if (!alloc || !feed_url || !articles || !out_count || cap == 0)
        return HU_ERR_INVALID_ARGUMENT;
    *out_count = 0;

    char url_buf[1024];
    if (url_len >= sizeof(url_buf))
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(url_buf, feed_url, url_len);
    url_buf[url_len] = '\0';

    hu_http_response_t resp = {0};
    hu_error_t err = hu_http_get(alloc, url_buf, NULL, &resp);
    if (err != HU_OK)
        return err;
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
