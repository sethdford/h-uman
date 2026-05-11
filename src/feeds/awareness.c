/*
 * Feed awareness — synthesize news/social/Apple-style feeds into natural conversation hooks.
 */
#include "human/feeds/awareness.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#if HU_IS_TEST
static const hu_awareness_topic_t k_awareness_mock_topics[] = {
    {.text = "Did you see the Warriors game last night? Curry went off",
     .source = "sports_feed",
     .relevance = 0.9,
     .suggested_contact = ""},
    {.text = "There's a new restaurant opening downtown that does craft cocktails",
     .source = "local_news",
     .relevance = 0.7,
     .suggested_contact = ""},
};
#endif

void hu_feed_awareness_topics_free(hu_allocator_t *alloc, hu_awareness_topic_t *topics,
                                   size_t topic_count) {
    if (!alloc || !topics || topic_count == 0)
        return;
    alloc->free(alloc->ctx, topics, topic_count * sizeof(*topics));
}

#ifdef HU_ENABLE_SQLITE
static hu_feed_type_t awareness_parse_content_type(const char *ct) {
    if (!ct || ct[0] == '\0')
        return HU_FEED_NEWS_RSS;
    for (hu_feed_type_t t = (hu_feed_type_t)0; t < HU_FEED_COUNT;
         t = (hu_feed_type_t)((int)t + 1)) {
        if (strcmp(ct, hu_feed_type_str(t)) == 0)
            return t;
    }
    return HU_FEED_NEWS_RSS;
}
void hu_feed_awareness_item_from_stored(const hu_feed_item_stored_t *stored, hu_feed_item_t *out) {
    if (!stored || !out)
        return;
    memset(out, 0, sizeof(*out));
    /* Borrow fixed buffers from stored row (read-only use of content/source). */
    out->content = (char *)(void *)stored->content;
    out->content_len = stored->content_len;
    out->source = (char *)(void *)stored->source;
    out->source_len = strlen(stored->source);
    out->type = awareness_parse_content_type(stored->content_type);
    if (stored->ingested_at > 0)
        out->fetched_at = (uint64_t)stored->ingested_at;
}
#endif

static double max_interest_score(const char *text, size_t text_len, char **lines,
                                 size_t lines_count) {
    double best = 0.0;
    if (!text || text_len == 0 || !lines)
        return 0.0;
    for (size_t i = 0; i < lines_count; i++) {
        if (!lines[i])
            continue;
        size_t len = strlen(lines[i]);
        if (len == 0)
            continue;
        double s = hu_feeds_score_relevance(text, text_len, lines[i], len);
        if (s > best)
            best = s;
    }
    return best;
}

#ifndef HU_IS_TEST
static void build_source_label(char *dst, size_t cap, const hu_feed_item_t *item) {
    if (!dst || cap == 0)
        return;
    dst[0] = '\0';
    if (!item)
        return;
    const char *tstr = hu_feed_type_str(item->type);
    if (item->source && item->source_len > 0)
        (void)snprintf(dst, cap, "%s: %.*s", tstr, (int)item->source_len, item->source);
    else
        (void)snprintf(dst, cap, "%s", tstr);
}

static double recency_factor(uint64_t fetched_at) {
    if (fetched_at == 0)
        return 1.0;
    time_t now = time(NULL);
    double age_h = difftime(now, (time_t)fetched_at) / 3600.0;
    if (age_h > 72.0)
        return 0.65;
    if (age_h > 24.0)
        return 0.85;
    return 1.0;
}

static void pick_suggested_contact(const hu_feed_item_t *item, const hu_persona_t *persona,
                                   char *out, size_t out_cap) {
    out[0] = '\0';
    if (!item || !persona || out_cap == 0)
        return;
    double best = 0.08;
    const char *pick = NULL;
    for (size_t ci = 0; ci < persona->contacts_count; ci++) {
        const hu_contact_profile_t *c = &persona->contacts[ci];
        double s =
            max_interest_score(item->content, item->content_len, c->interests, c->interests_count);
        if (s > best) {
            best = s;
            if (c->name && c->name[0])
                pick = c->name;
            else if (c->contact_id && c->contact_id[0])
                pick = c->contact_id;
        }
    }
    if (pick)
        (void)snprintf(out, out_cap, "%s", pick);
}

static int cmp_awareness_desc(const void *a, const void *b) {
    const hu_awareness_topic_t *ta = (const hu_awareness_topic_t *)a;
    const hu_awareness_topic_t *tb = (const hu_awareness_topic_t *)b;
    if (ta->relevance > tb->relevance)
        return -1;
    if (ta->relevance < tb->relevance)
        return 1;
    return 0;
}

static double score_item_for_persona(const hu_feed_item_t *item, const hu_persona_t *persona,
                                     char *combined, size_t combined_cap) {
    if (!item || !combined || combined_cap < 2)
        return 0.0;

    size_t clen = 0;
    if (item->content && item->content_len > 0) {
        size_t n = item->content_len < combined_cap - 1 ? item->content_len : combined_cap - 1;
        memcpy(combined, item->content, n);
        clen = n;
    }
    combined[clen] = '\0';
    if (item->topic && item->topic_len > 0 && clen + 2 < combined_cap) {
        combined[clen++] = ' ';
        size_t rem = combined_cap - 1 - clen;
        size_t tn = item->topic_len < rem ? item->topic_len : rem;
        memcpy(combined + clen, item->topic, tn);
        clen += tn;
        combined[clen] = '\0';
    }

    if (!persona)
        return 0.45 * recency_factor(item->fetched_at);

    double best = 0.0;
    best = max_interest_score(combined, clen, persona->traits, persona->traits_count);
    double v = max_interest_score(combined, clen, persona->intellectual.curiosity_areas,
                                  persona->intellectual.curiosity_areas_count);
    if (v > best)
        best = v;

    for (size_t ti = 0; ti < persona->context_awareness.sports_teams_count; ti++) {
        if (persona->context_awareness.sports_teams[ti][0] == '\0')
            continue;
        v = hu_feeds_score_relevance(combined, clen, persona->context_awareness.sports_teams[ti],
                                     strlen(persona->context_awareness.sports_teams[ti]));
        if (v > best)
            best = v;
    }
    for (size_t ni = 0; ni < persona->context_awareness.news_topics_count; ni++) {
        if (persona->context_awareness.news_topics[ni][0] == '\0')
            continue;
        v = hu_feeds_score_relevance(combined, clen, persona->context_awareness.news_topics[ni],
                                     strlen(persona->context_awareness.news_topics[ni]));
        if (v > best)
            best = v;
    }

    return best * recency_factor(item->fetched_at);
}
#endif /* !HU_IS_TEST */

hu_error_t hu_feed_awareness_synthesize(hu_allocator_t *alloc, const hu_feed_item_t *feed_items,
                                        size_t feed_count, const hu_persona_t *persona,
                                        hu_awareness_topic_t **out_topics,
                                        size_t *out_topic_count) {
    if (!alloc || !out_topics || !out_topic_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out_topics = NULL;
    *out_topic_count = 0;

#if HU_IS_TEST
    (void)feed_items;
    (void)feed_count;
    (void)persona;
    size_t n = sizeof(k_awareness_mock_topics) / sizeof(k_awareness_mock_topics[0]);
    hu_awareness_topic_t *buf = (hu_awareness_topic_t *)alloc->alloc(alloc->ctx, n * sizeof(*buf));
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, k_awareness_mock_topics, n * sizeof(*buf));
    *out_topics = buf;
    *out_topic_count = n;
    return HU_OK;
#else
    if (feed_count == 0)
        return HU_OK;

    enum { MAX_TOPICS = 16 };
    hu_awareness_topic_t stack[MAX_TOPICS];
    size_t n = 0;
    char combined[3072];

    for (size_t i = 0; i < feed_count && n < MAX_TOPICS; i++) {
        const hu_feed_item_t *it = &feed_items[i];
        if (!it->content || it->content_len == 0)
            continue;

        double score = score_item_for_persona(it, persona, combined, sizeof(combined));
        if (persona && score < 0.12)
            continue;
        if (!persona && score < 0.01)
            continue;

        hu_awareness_topic_t *t = &stack[n];
        memset(t, 0, sizeof(*t));
        size_t copy = it->content_len < sizeof(t->text) - 1 ? it->content_len : sizeof(t->text) - 1;
        memcpy(t->text, it->content, copy);
        t->text[copy] = '\0';
        build_source_label(t->source, sizeof(t->source), it);
        t->relevance = score > 1.0 ? 1.0 : score;
        if (persona)
            pick_suggested_contact(it, persona, t->suggested_contact, sizeof(t->suggested_contact));
        n++;
    }

    if (n == 0)
        return HU_OK;

    qsort(stack, n, sizeof(stack[0]), cmp_awareness_desc);

    hu_awareness_topic_t *buf = (hu_awareness_topic_t *)alloc->alloc(alloc->ctx, n * sizeof(*buf));
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    memcpy(buf, stack, n * sizeof(*buf));
    *out_topics = buf;
    *out_topic_count = n;
    return HU_OK;
#endif
}

bool hu_feed_awareness_should_share(const hu_awareness_topic_t *topic,
                                    const hu_contact_profile_t *contact_profile) {
    if (!topic || !contact_profile)
        return false;
    if (topic->relevance < 0.35)
        return false;

    const char *txt = topic->text;
    size_t tlen = strlen(txt);

    for (size_t si = 0; si < contact_profile->sensitive_topics_count; si++) {
        const char *s = contact_profile->sensitive_topics[si];
        if (!s || !s[0])
            continue;
        if (hu_feeds_score_relevance(txt, tlen, s, strlen(s)) > 0.35)
            return false;
    }

    bool suggested_for_them = false;
    if (topic->suggested_contact[0] != '\0') {
        if (contact_profile->name && contact_profile->name[0] &&
            strcasecmp(topic->suggested_contact, contact_profile->name) == 0)
            suggested_for_them = true;
        if (contact_profile->contact_id && contact_profile->contact_id[0] &&
            strcmp(topic->suggested_contact, contact_profile->contact_id) == 0)
            suggested_for_them = true;
    }

    double shared =
        max_interest_score(txt, tlen, contact_profile->interests, contact_profile->interests_count);
    double recent = max_interest_score(txt, tlen, contact_profile->recent_topics,
                                       contact_profile->recent_topics_count);

    if (suggested_for_them && topic->relevance >= 0.5)
        return true;
    if (shared >= 0.2)
        return true;
    if (recent >= 0.15 && topic->relevance >= 0.45)
        return true;
    /* High-relevance topics still require at least minimal contact interest
     * match — otherwise persona-relevant items (e.g. tech news) leak into
     * proactive messages to contacts who have no interest in those topics. */
    if (topic->relevance >= 0.82 && (shared >= 0.08 || recent >= 0.08))
        return true;
    return false;
}

#ifdef HU_ENABLE_SQLITE
bool hu_feed_awareness_contact_has_high_topics(hu_allocator_t *alloc, sqlite3 *db,
                                               const hu_persona_t *persona,
                                               const hu_contact_profile_t *cp,
                                               double min_relevance) {
    if (!alloc || !db || !persona || !cp || min_relevance <= 0.0)
        return false;

    int64_t since = (int64_t)time(NULL) - (int64_t)172800; /* 48h */
    hu_feed_item_stored_t *stored = NULL;
    size_t scount = 0;
    if (hu_feed_processor_get_all_recent(alloc, db, since, 32, &stored, &scount) != HU_OK ||
        !stored || scount == 0)
        return false;

    hu_feed_item_t *items = (hu_feed_item_t *)alloc->alloc(alloc->ctx, scount * sizeof(*items));
    if (!items) {
        hu_feed_items_free(alloc, stored, scount);
        return false;
    }
    memset(items, 0, scount * sizeof(*items));
    for (size_t i = 0; i < scount; i++)
        hu_feed_awareness_item_from_stored(&stored[i], &items[i]);

    hu_awareness_topic_t *topics = NULL;
    size_t tcount = 0;
    hu_error_t syn_err =
        hu_feed_awareness_synthesize(alloc, items, scount, persona, &topics, &tcount);
    alloc->free(alloc->ctx, items, scount * sizeof(*items));
    hu_feed_items_free(alloc, stored, scount);

    if (syn_err != HU_OK || !topics) {
        if (topics)
            hu_feed_awareness_topics_free(alloc, topics, tcount);
        return false;
    }

    bool hit = false;
    for (size_t i = 0; i < tcount; i++) {
        if (topics[i].relevance >= min_relevance &&
            hu_feed_awareness_should_share(&topics[i], cp)) {
            hit = true;
            break;
        }
    }
    hu_feed_awareness_topics_free(alloc, topics, tcount);
    return hit;
}
#endif
