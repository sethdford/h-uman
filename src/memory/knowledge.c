#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory/knowledge.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define HU_KNOWLEDGE_ESCAPE_BUF 1024
#define HU_KNOWLEDGE_SQL_BUF 4096
#define HU_KNOWLEDGE_SUMMARY_MAX_TOPICS 64

static void escape_sql_string(const char *s, size_t len, char *buf, size_t cap, size_t *out_len) {
    size_t pos = 0;
    for (size_t i = 0; i < len && pos + 2 < cap; i++) {
        if (s[i] == '\'') {
            buf[pos++] = '\'';
            buf[pos++] = '\'';
        } else {
            buf[pos++] = s[i];
        }
    }
    buf[pos] = '\0';
    *out_len = pos;
}

static const char *knowledge_source_to_str(hu_knowledge_source_t src) {
    switch (src) {
    case HU_KNOWLEDGE_DIRECT:
        return "direct";
    case HU_KNOWLEDGE_GROUP_CHAT:
        return "group_chat";
    case HU_KNOWLEDGE_INFERRED:
        return "inferred";
    case HU_KNOWLEDGE_PUBLIC:
        return "public";
    default:
        return "direct";
    }
}

static void normalize_topic(const char *topic, size_t topic_len, char *buf, size_t cap,
                            size_t *out_len) {
    size_t pos = 0;
    for (size_t i = 0; i < topic_len && pos + 1 < cap; i++) {
        char c = (char)(unsigned char)topic[i];
        if (c == ' ' || c == '-')
            buf[pos++] = '_';
        else if (isalnum((unsigned char)c) || c == '_')
            buf[pos++] = (char)tolower((unsigned char)c);
    }
    buf[pos] = '\0';
    *out_len = pos;
}

static bool topic_matches(const char *entry_topic, size_t entry_len, const char *query_topic,
                          size_t query_len) {
    char ebuf[256], qbuf[256];
    size_t elen, qlen;
    if (entry_len >= sizeof(ebuf) || query_len >= sizeof(qbuf))
        return false;
    normalize_topic(entry_topic, entry_len, ebuf, sizeof(ebuf), &elen);
    normalize_topic(query_topic, query_len, qbuf, sizeof(qbuf), &qlen);
    if (elen == 0 || qlen == 0)
        return false;
    return strstr(ebuf, qbuf) != NULL || strstr(qbuf, ebuf) != NULL;
}

static bool contact_id_eq(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (a_len != b_len)
        return false;
    return memcmp(a, b, a_len) == 0;
}

hu_error_t hu_knowledge_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    static const char sql[] =
        "CREATE TABLE IF NOT EXISTS contact_knowledge (\n"
        "    id INTEGER PRIMARY KEY,\n"
        "    contact_id TEXT NOT NULL,\n"
        "    topic TEXT NOT NULL,\n"
        "    detail TEXT,\n"
        "    confidence REAL NOT NULL DEFAULT 1.0,\n"
        "    source TEXT NOT NULL,\n"
        "    shared_at INTEGER NOT NULL,\n"
        "    source_contact_id TEXT,\n"
        "    UNIQUE(contact_id, topic)\n"
        ")";
    size_t len = sizeof(sql) - 1;
    if (len >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    memcpy(buf, sql, len + 1);
    *out_len = len;
    return HU_OK;
}

hu_error_t hu_knowledge_insert_sql(const hu_knowledge_entry_t *entry, char *buf, size_t cap,
                                   size_t *out_len) {
    if (!entry || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    if (!entry->contact_id || !entry->topic)
        return HU_ERR_INVALID_ARGUMENT;

    char topic_esc[HU_KNOWLEDGE_ESCAPE_BUF];
    char detail_esc[HU_KNOWLEDGE_ESCAPE_BUF];
    char contact_esc[HU_KNOWLEDGE_ESCAPE_BUF];
    char source_contact_esc[HU_KNOWLEDGE_ESCAPE_BUF];

    size_t te_len, de_len, ce_len, sce_len;
    escape_sql_string(entry->topic, entry->topic_len, topic_esc, sizeof(topic_esc), &te_len);
    escape_sql_string(entry->contact_id, entry->contact_id_len, contact_esc, sizeof(contact_esc),
                      &ce_len);
    size_t detail_len = entry->detail ? entry->detail_len : 0;
    escape_sql_string(entry->detail ? entry->detail : "", detail_len, detail_esc,
                      sizeof(detail_esc), &de_len);
    if (entry->source_contact_id && entry->source_contact_id_len > 0) {
        escape_sql_string(entry->source_contact_id, entry->source_contact_id_len,
                          source_contact_esc, sizeof(source_contact_esc), &sce_len);
    } else {
        source_contact_esc[0] = '\0';
        sce_len = 0;
    }

    const char *src_str = knowledge_source_to_str(entry->source);
    int n;
    if (sce_len > 0) {
        n = snprintf(buf, cap,
                     "INSERT OR REPLACE INTO contact_knowledge "
                     "(id, contact_id, topic, detail, confidence, source, shared_at, "
                     "source_contact_id) VALUES (%lld, '%s', '%s', '%s', %f, '%s', %llu, '%s')",
                     (long long)entry->id, contact_esc, topic_esc, detail_esc, entry->confidence,
                     src_str, (unsigned long long)entry->shared_at, source_contact_esc);
    } else {
        n = snprintf(buf, cap,
                     "INSERT OR REPLACE INTO contact_knowledge "
                     "(id, contact_id, topic, detail, confidence, source, shared_at, "
                     "source_contact_id) VALUES (%lld, '%s', '%s', '%s', %f, '%s', %llu, NULL)",
                     (long long)entry->id, contact_esc, topic_esc, detail_esc, entry->confidence,
                     src_str, (unsigned long long)entry->shared_at);
    }
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_knowledge_query_sql(const char *contact_id, size_t contact_id_len,
                                  double min_confidence, char *buf, size_t cap, size_t *out_len) {
    if (!contact_id || contact_id_len == 0 || !buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[257];
    size_t ce_len;
    escape_sql_string(contact_id, contact_id_len, contact_esc, sizeof(contact_esc), &ce_len);

    int n = snprintf(buf, cap,
                     "SELECT topic, detail, confidence, source, shared_at FROM contact_knowledge "
                     "WHERE contact_id = '%s' AND confidence >= %f ORDER BY shared_at DESC",
                     contact_esc, min_confidence);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

bool hu_knowledge_contact_knows(const hu_knowledge_entry_t *entries, size_t entry_count,
                                const char *contact_id, size_t contact_id_len,
                                const char *topic, size_t topic_len, double min_confidence) {
    if (!entries || !contact_id || !topic)
        return false;
    for (size_t i = 0; i < entry_count; i++) {
        const hu_knowledge_entry_t *e = &entries[i];
        if (!e->contact_id || !e->topic)
            continue;
        if (!contact_id_eq(e->contact_id, e->contact_id_len, contact_id, contact_id_len))
            continue;
        if (!topic_matches(e->topic, e->topic_len, topic, topic_len))
            continue;
        if (e->confidence >= min_confidence)
            return true;
    }
    return false;
}

bool hu_knowledge_would_leak(const hu_knowledge_entry_t *entries, size_t entry_count,
                             const char *topic, size_t topic_len,
                             const char *source_contact_id, size_t source_len,
                             const char *target_contact_id, size_t target_len) {
    if (!entries || !topic || !source_contact_id || !target_contact_id)
        return false;

    bool source_knows = false;
    bool target_knows = false;
    bool is_public = false;

    for (size_t i = 0; i < entry_count; i++) {
        const hu_knowledge_entry_t *e = &entries[i];
        if (!e->topic || !topic_matches(e->topic, e->topic_len, topic, topic_len))
            continue;

        if (e->source == HU_KNOWLEDGE_PUBLIC)
            is_public = true;

        if (contact_id_eq(e->contact_id, e->contact_id_len, source_contact_id, source_len))
            source_knows = true;
        if (e->source_contact_id &&
            contact_id_eq(e->source_contact_id, e->source_contact_id_len, source_contact_id,
                          source_len))
            source_knows = true;

        if (contact_id_eq(e->contact_id, e->contact_id_len, target_contact_id, target_len))
            target_knows = true;
    }

    if (is_public)
        return false;
    return source_knows && !target_knows;
}

hu_error_t hu_knowledge_build_summary(hu_allocator_t *alloc,
                                      const hu_knowledge_entry_t *entries, size_t entry_count,
                                      const char *contact_id, size_t contact_id_len,
                                      const char **all_topics, size_t topic_count,
                                      hu_knowledge_summary_t *out) {
    if (!alloc || !out || !contact_id)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out, 0, sizeof(*out));
    out->contact_id = hu_strndup(alloc, contact_id, contact_id_len);
    if (!out->contact_id)
        return HU_ERR_OUT_OF_MEMORY;
    out->contact_id_len = contact_id_len;

    if (!all_topics || topic_count == 0)
        return HU_OK;

    size_t cap = HU_KNOWLEDGE_SUMMARY_MAX_TOPICS;

    char **known = (char **)alloc->alloc(alloc->ctx, cap * sizeof(char *));
    char **unknown = (char **)alloc->alloc(alloc->ctx, cap * sizeof(char *));
    char **uncertain = (char **)alloc->alloc(alloc->ctx, cap * sizeof(char *));
    if (!known || !unknown || !uncertain) {
        if (known)
            alloc->free(alloc->ctx, known, cap * sizeof(char *));
        if (unknown)
            alloc->free(alloc->ctx, unknown, cap * sizeof(char *));
        if (uncertain)
            alloc->free(alloc->ctx, uncertain, cap * sizeof(char *));
        hu_str_free(alloc, out->contact_id);
        out->contact_id = NULL;
        return HU_ERR_OUT_OF_MEMORY;
    }
    memset(known, 0, cap * sizeof(char *));
    memset(unknown, 0, cap * sizeof(char *));
    memset(uncertain, 0, cap * sizeof(char *));

    size_t known_count = 0, unknown_count = 0, uncertain_count = 0;

    for (size_t t = 0; t < topic_count; t++) {
        const char *topic = all_topics[t];
        size_t topic_len = topic ? strlen(topic) : 0;
        if (!topic || topic_len == 0)
            continue;

        double best_conf = -1.0;
        for (size_t i = 0; i < entry_count; i++) {
            const hu_knowledge_entry_t *e = &entries[i];
            if (!e->contact_id || !e->topic)
                continue;
            if (!contact_id_eq(e->contact_id, e->contact_id_len, contact_id, contact_id_len))
                continue;
            if (!topic_matches(e->topic, e->topic_len, topic, topic_len))
                continue;
            if (e->confidence > best_conf)
                best_conf = e->confidence;
        }

        char *dup = hu_strndup(alloc, topic, topic_len);
        if (!dup) {
            for (size_t k = 0; k < known_count; k++)
                hu_str_free(alloc, known[k]);
            for (size_t k = 0; k < unknown_count; k++)
                hu_str_free(alloc, unknown[k]);
            for (size_t k = 0; k < uncertain_count; k++)
                hu_str_free(alloc, uncertain[k]);
            alloc->free(alloc->ctx, known, cap * sizeof(char *));
            alloc->free(alloc->ctx, unknown, cap * sizeof(char *));
            alloc->free(alloc->ctx, uncertain, cap * sizeof(char *));
            hu_str_free(alloc, out->contact_id);
            out->contact_id = NULL;
            return HU_ERR_OUT_OF_MEMORY;
        }

        if (best_conf >= 0.7) {
            if (known_count < cap)
                known[known_count++] = dup;
            else
                hu_str_free(alloc, dup);
        } else if (best_conf >= 0.3 && best_conf < 0.7) {
            if (uncertain_count < cap)
                uncertain[uncertain_count++] = dup;
            else
                hu_str_free(alloc, dup);
        } else {
            if (unknown_count < cap)
                unknown[unknown_count++] = dup;
            else
                hu_str_free(alloc, dup);
        }
    }

    out->known_topics = known;
    out->known_count = known_count;
    out->unknown_topics = unknown;
    out->unknown_count = unknown_count;
    out->uncertain_topics = uncertain;
    out->uncertain_count = uncertain_count;
    return HU_OK;
}

hu_error_t hu_knowledge_summary_to_prompt(hu_allocator_t *alloc,
                                          const hu_knowledge_summary_t *summary, char **out,
                                          size_t *out_len) {
    if (!alloc || !summary || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;

    char buf[HU_KNOWLEDGE_SQL_BUF];
    size_t pos = 0;
    size_t cap = sizeof(buf);

    const char *contact = summary->contact_id ? summary->contact_id : "unknown";
    int n = snprintf(buf + pos, cap - pos, "[KNOWLEDGE STATE for %s]:\n", contact);
    if (n < 0 || (size_t)n >= cap - pos)
        return HU_ERR_INVALID_ARGUMENT;
    pos += (size_t)n;

    n = snprintf(buf + pos, cap - pos, "- They know about: ");
    if (n < 0 || (size_t)n >= cap - pos)
        return HU_ERR_INVALID_ARGUMENT;
    pos += (size_t)n;
    for (size_t i = 0; i < summary->known_count && pos < cap - 32; i++) {
        if (i > 0) {
            buf[pos++] = ',';
            buf[pos++] = ' ';
        }
        size_t len = strlen(summary->known_topics[i]);
        if (pos + len >= cap - 2)
            break;
        memcpy(buf + pos, summary->known_topics[i], len + 1);
        pos += len;
    }
    if (summary->known_count == 0) {
        n = snprintf(buf + pos, cap - pos, "(none)");
        if (n > 0)
            pos += (size_t)n;
    }
    buf[pos++] = '\n';
    buf[pos] = '\0';

    n = snprintf(buf + pos, cap - pos, "- They DON'T know about: ");
    if (n < 0 || (size_t)n >= cap - pos)
        return HU_ERR_INVALID_ARGUMENT;
    pos += (size_t)n;
    for (size_t i = 0; i < summary->unknown_count && pos < cap - 32; i++) {
        if (i > 0) {
            buf[pos++] = ',';
            buf[pos++] = ' ';
        }
        size_t len = strlen(summary->unknown_topics[i]);
        if (pos + len >= cap - 2)
            break;
        memcpy(buf + pos, summary->unknown_topics[i], len + 1);
        pos += len;
    }
    if (summary->unknown_count == 0) {
        n = snprintf(buf + pos, cap - pos, "(none)");
        if (n > 0)
            pos += (size_t)n;
    }
    buf[pos++] = '\n';
    buf[pos] = '\0';

    n = snprintf(buf + pos, cap - pos, "- Uncertain: ");
    if (n < 0 || (size_t)n >= cap - pos)
        return HU_ERR_INVALID_ARGUMENT;
    pos += (size_t)n;
    for (size_t i = 0; i < summary->uncertain_count && pos < cap - 64; i++) {
        if (i > 0) {
            buf[pos++] = ',';
            buf[pos++] = ' ';
        }
        n = snprintf(buf + pos, cap - pos, "%s (mentioned in group chat)", summary->uncertain_topics[i]);
        if (n > 0)
            pos += (size_t)n;
    }
    if (summary->uncertain_count == 0) {
        n = snprintf(buf + pos, cap - pos, "(none)");
        if (n > 0)
            pos += (size_t)n;
    }
    buf[pos++] = '\n';
    buf[pos] = '\0';

    n = snprintf(buf + pos, cap - pos,
                 "\nRULE: Do not re-tell stories they already know. If genuinely unsure, use "
                 "\"did I tell you about X?\" framing.\n");
    if (n > 0 && (size_t)n < cap - pos)
        pos += (size_t)n;

    *out = hu_strndup(alloc, buf, pos);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_len = pos;
    return HU_OK;
}

void hu_knowledge_entry_deinit(hu_allocator_t *alloc, hu_knowledge_entry_t *entry) {
    if (!alloc || !entry)
        return;
    if (entry->topic) {
        hu_str_free(alloc, entry->topic);
        entry->topic = NULL;
        entry->topic_len = 0;
    }
    if (entry->detail) {
        hu_str_free(alloc, entry->detail);
        entry->detail = NULL;
        entry->detail_len = 0;
    }
    if (entry->contact_id) {
        hu_str_free(alloc, entry->contact_id);
        entry->contact_id = NULL;
        entry->contact_id_len = 0;
    }
    if (entry->source_contact_id) {
        hu_str_free(alloc, entry->source_contact_id);
        entry->source_contact_id = NULL;
        entry->source_contact_id_len = 0;
    }
}

void hu_knowledge_summary_deinit(hu_allocator_t *alloc, hu_knowledge_summary_t *summary) {
    if (!alloc || !summary)
        return;
    if (summary->contact_id) {
        hu_str_free(alloc, summary->contact_id);
        summary->contact_id = NULL;
        summary->contact_id_len = 0;
    }
    if (summary->known_topics) {
        for (size_t i = 0; i < summary->known_count; i++)
            hu_str_free(alloc, summary->known_topics[i]);
        alloc->free(alloc->ctx, summary->known_topics,
                    HU_KNOWLEDGE_SUMMARY_MAX_TOPICS * sizeof(char *));
        summary->known_topics = NULL;
        summary->known_count = 0;
    }
    if (summary->unknown_topics) {
        for (size_t i = 0; i < summary->unknown_count; i++)
            hu_str_free(alloc, summary->unknown_topics[i]);
        alloc->free(alloc->ctx, summary->unknown_topics,
                    HU_KNOWLEDGE_SUMMARY_MAX_TOPICS * sizeof(char *));
        summary->unknown_topics = NULL;
        summary->unknown_count = 0;
    }
    if (summary->uncertain_topics) {
        for (size_t i = 0; i < summary->uncertain_count; i++)
            hu_str_free(alloc, summary->uncertain_topics[i]);
        alloc->free(alloc->ctx, summary->uncertain_topics,
                    HU_KNOWLEDGE_SUMMARY_MAX_TOPICS * sizeof(char *));
        summary->uncertain_topics = NULL;
        summary->uncertain_count = 0;
    }
}
