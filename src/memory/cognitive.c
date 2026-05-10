#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory/cognitive.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define HU_COGNITIVE_ESCAPE_BUF 1024
#define HU_COGNITIVE_SQL_BUF 4096

/* Thin wrapper around hu_sql_quote_escape_into in core/string.h. Kept to
 * preserve this module's existing void-return call sites during the gradual
 * dedupe migration tracked in docs/dedupe-debt.md. */
static void escape_sql_string(const char *s, size_t len, char *buf, size_t cap, size_t *out_len) {
    (void)hu_sql_quote_escape_into(s, len, buf, cap, out_len);
}

static int tolower_char(unsigned char c) {
    return (int)tolower(c);
}

static bool strncasecmp_eq(const char *a, size_t a_len, const char *b, size_t b_len) {
    if (a_len != b_len)
        return false;
    for (size_t i = 0; i < a_len; i++) {
        if (tolower_char((unsigned char)a[i]) != tolower_char((unsigned char)b[i]))
            return false;
    }
    return true;
}

/* --- F65 Opinions --- */
hu_error_t hu_opinions_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    int n = snprintf(buf, cap,
                     "CREATE TABLE IF NOT EXISTS opinions ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                     "topic TEXT NOT NULL, "
                     "position TEXT NOT NULL, "
                     "confidence REAL NOT NULL, "
                     "first_expressed INTEGER NOT NULL, "
                     "last_expressed INTEGER NOT NULL, "
                     "superseded_by INTEGER)");
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_opinions_upsert_sql(const char *topic, size_t topic_len,
                                 const char *position, size_t position_len,
                                 double confidence, uint64_t now_ms,
                                 char *buf, size_t cap, size_t *out_len) {
    if (!topic || !position || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    char topic_esc[HU_COGNITIVE_ESCAPE_BUF];
    char pos_esc[HU_COGNITIVE_ESCAPE_BUF];
    size_t te_len, pe_len;
    escape_sql_string(topic, topic_len, topic_esc, sizeof(topic_esc), &te_len);
    escape_sql_string(position, position_len, pos_esc, sizeof(pos_esc), &pe_len);
    int n = snprintf(buf, cap,
                    "INSERT INTO opinions (topic, position, confidence, first_expressed, last_expressed, superseded_by) "
                    "VALUES ('%.*s', '%.*s', %f, %llu, %llu, NULL)",
                    (int)te_len, topic_esc, (int)pe_len, pos_esc, confidence,
                    (unsigned long long)now_ms, (unsigned long long)now_ms);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_opinions_query_current_sql(const char *topic, size_t topic_len,
                                        char *buf, size_t cap, size_t *out_len) {
    if (!topic || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    char topic_esc[HU_COGNITIVE_ESCAPE_BUF];
    size_t te_len;
    escape_sql_string(topic, topic_len, topic_esc, sizeof(topic_esc), &te_len);
    int n = snprintf(buf, cap,
                    "SELECT id, topic, position, confidence, first_expressed, last_expressed, superseded_by "
                    "FROM opinions WHERE topic='%.*s' AND superseded_by IS NULL",
                    (int)te_len, topic_esc);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_opinions_supersede_sql(int64_t old_id, int64_t new_id,
                                    char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;
    int n = snprintf(buf, cap,
                    "UPDATE opinions SET superseded_by=%lld WHERE id=%lld",
                    (long long)new_id, (long long)old_id);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_opinions_build_prompt(hu_allocator_t *alloc, const hu_opinion_t *opinions,
                                   size_t count, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (count == 0 || !opinions) {
        char *empty = hu_strndup(alloc, "[No recorded opinions]", 21);
        if (!empty)
            return HU_ERR_OUT_OF_MEMORY;
        *out = empty;
        *out_len = 21;
        return HU_OK;
    }
    size_t total = 64;
    for (size_t i = 0; i < count; i++) {
        if (opinions[i].topic && opinions[i].position)
            total += 4 + (opinions[i].topic_len > 0 ? opinions[i].topic_len : 0)
                     + 2 + (opinions[i].position_len > 0 ? opinions[i].position_len : 0)
                     + 32;
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, total);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t pos = 0;
    pos = hu_buf_appendf(buf, total, pos, "[YOUR OPINIONS]:");
    for (size_t i = 0; i < count && pos < total - 2; i++) {
        const char *t = opinions[i].topic ? opinions[i].topic : "";
        size_t tl = opinions[i].topic_len;
        const char *p = opinions[i].position ? opinions[i].position : "";
        size_t pl = opinions[i].position_len;
        double conf = opinions[i].confidence;
        pos = hu_buf_appendf(buf, total, pos,
                             " - [%.*s]: %.*s (confidence: %.2f)",
                             (int)tl, t, (int)pl, p, conf);
    }
    *out = buf;
    *out_len = strlen(buf);
    return HU_OK;
}

void hu_opinion_deinit(hu_allocator_t *alloc, hu_opinion_t *op) {
    if (!alloc || !op)
        return;
    hu_str_free(alloc, op->topic);
    hu_str_free(alloc, op->position);
    op->topic = NULL;
    op->position = NULL;
    op->topic_len = 0;
    op->position_len = 0;
}

/* --- F66 Life Chapters --- */
hu_error_t hu_chapters_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    int n = snprintf(buf, cap,
                     "CREATE TABLE IF NOT EXISTS life_chapters ("
                     "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                     "theme TEXT NOT NULL, "
                     "mood TEXT NOT NULL, "
                     "started_at INTEGER NOT NULL, "
                     "ended_at INTEGER NOT NULL, "
                     "key_threads TEXT, "
                     "active INTEGER NOT NULL)");
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_chapters_insert_sql(const hu_life_chapter_t *ch,
                                 char *buf, size_t cap, size_t *out_len) {
    if (!ch || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    char theme_esc[HU_COGNITIVE_ESCAPE_BUF];
    char mood_esc[HU_COGNITIVE_ESCAPE_BUF];
    char threads_esc[HU_COGNITIVE_ESCAPE_BUF];
    size_t te_len, me_len, th_len;
    escape_sql_string(ch->theme ? ch->theme : "", ch->theme_len, theme_esc,
                     sizeof(theme_esc), &te_len);
    escape_sql_string(ch->mood ? ch->mood : "", ch->mood_len, mood_esc,
                     sizeof(mood_esc), &me_len);
    const char *th = ch->key_threads ? ch->key_threads : "";
    size_t th_src = ch->key_threads ? ch->key_threads_len : 0;
    escape_sql_string(th, th_src, threads_esc, sizeof(threads_esc), &th_len);
    int n = snprintf(buf, cap,
                    "INSERT INTO life_chapters (theme, mood, started_at, ended_at, key_threads, active) "
                    "VALUES ('%.*s', '%.*s', %llu, %llu, '%.*s', %d)",
                    (int)te_len, theme_esc, (int)me_len, mood_esc,
                    (unsigned long long)ch->started_at, (unsigned long long)ch->ended_at,
                    (int)th_len, threads_esc, ch->active ? 1 : 0);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_chapters_query_active_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;
    int n = snprintf(buf, cap,
                    "SELECT id, theme, mood, started_at, ended_at, key_threads, active "
                    "FROM life_chapters WHERE active=1");
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_chapters_close_sql(int64_t id, uint64_t ended_at,
                                 char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;
    int n = snprintf(buf, cap,
                    "UPDATE life_chapters SET ended_at=%llu, active=0 WHERE id=%lld",
                    (unsigned long long)ended_at, (long long)id);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_chapters_build_prompt(hu_allocator_t *alloc, const hu_life_chapter_t *chapters,
                                   size_t count, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (count == 0 || !chapters) {
        char *empty = hu_strndup(alloc, "[No active life chapter]", 24);
        if (!empty)
            return HU_ERR_OUT_OF_MEMORY;
        *out = empty;
        *out_len = 24;
        return HU_OK;
    }
    char buf[HU_COGNITIVE_SQL_BUF];
    size_t pos = 0;
    size_t cap = sizeof(buf);
    int n = snprintf(buf + pos, cap - pos, "[CURRENT LIFE CHAPTER]: ");
    if (n < 0 || (size_t)n >= cap - pos)
        return HU_ERR_INVALID_ARGUMENT;
    pos += (size_t)n;
    for (size_t i = 0; i < count && pos < cap - 64; i++) {
        const char *theme = chapters[i].theme ? chapters[i].theme : "";
        size_t tl = chapters[i].theme_len;
        const char *mood = chapters[i].mood ? chapters[i].mood : "";
        size_t ml = chapters[i].mood_len;
        const char *threads = chapters[i].key_threads ? chapters[i].key_threads : "";
        size_t thl = chapters[i].key_threads_len;
        n = snprintf(buf + pos, cap - pos,
                     "Theme: %.*s. Mood: %.*s. Key threads: %.*s.",
                     (int)tl, theme, (int)ml, mood, (int)thl, threads);
        if (n < 0 || (size_t)n >= cap - pos)
            break;
        pos += (size_t)n;
    }
    *out = hu_strndup(alloc, buf, pos);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_len = pos;
    return HU_OK;
}

void hu_chapter_deinit(hu_allocator_t *alloc, hu_life_chapter_t *ch) {
    if (!alloc || !ch)
        return;
    hu_str_free(alloc, ch->theme);
    hu_str_free(alloc, ch->mood);
    hu_str_free(alloc, ch->key_threads);
    ch->theme = NULL;
    ch->mood = NULL;
    ch->key_threads = NULL;
    ch->theme_len = 0;
    ch->mood_len = 0;
    ch->key_threads_len = 0;
}

/* --- F67 Social Graph --- */
hu_error_t hu_social_graph_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    int n = snprintf(buf, cap,
                     "CREATE TABLE IF NOT EXISTS social_graph ("
                     "contact_a TEXT NOT NULL, "
                     "contact_b TEXT NOT NULL, "
                     "rel_type INTEGER NOT NULL, "
                     "closeness REAL NOT NULL, "
                     "context TEXT, "
                     "PRIMARY KEY(contact_a, contact_b))");
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_social_graph_insert_link_sql(const hu_social_link_t *link,
                                          char *buf, size_t cap, size_t *out_len) {
    if (!link || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    char a_esc[HU_COGNITIVE_ESCAPE_BUF];
    char b_esc[HU_COGNITIVE_ESCAPE_BUF];
    char ctx_esc[HU_COGNITIVE_ESCAPE_BUF];
    size_t ae_len, be_len, ce_len;
    escape_sql_string(link->contact_a ? link->contact_a : "", link->contact_a_len,
                     a_esc, sizeof(a_esc), &ae_len);
    escape_sql_string(link->contact_b ? link->contact_b : "", link->contact_b_len,
                     b_esc, sizeof(b_esc), &be_len);
    const char *ctx = link->context ? link->context : "";
    size_t ctx_src = link->context_len;
    escape_sql_string(ctx, ctx_src, ctx_esc, sizeof(ctx_esc), &ce_len);
    int n = snprintf(buf, cap,
                    "INSERT OR REPLACE INTO social_graph (contact_a, contact_b, rel_type, closeness, context) "
                    "VALUES ('%.*s', '%.*s', %d, %f, '%.*s')",
                    (int)ae_len, a_esc, (int)be_len, b_esc,
                    (int)link->rel_type, link->closeness, (int)ce_len, ctx_esc);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_social_graph_query_for_contact_sql(const char *contact_id, size_t len,
                                                 char *buf, size_t cap, size_t *out_len) {
    if (!contact_id || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    char id_esc[HU_COGNITIVE_ESCAPE_BUF];
    size_t ie_len;
    escape_sql_string(contact_id, len, id_esc, sizeof(id_esc), &ie_len);
    int n = snprintf(buf, cap,
                    "SELECT contact_a, contact_b, rel_type, closeness, context "
                    "FROM social_graph WHERE contact_a='%.*s' OR contact_b='%.*s'",
                    (int)ie_len, id_esc, (int)ie_len, id_esc);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

bool hu_social_graph_contacts_connected(const hu_social_link_t *links, size_t count,
                                        const char *a, size_t a_len,
                                        const char *b, size_t b_len) {
    if (!links || !a || !b)
        return false;
    for (size_t i = 0; i < count; i++) {
        const char *la = links[i].contact_a;
        size_t la_len = links[i].contact_a_len;
        const char *lb = links[i].contact_b;
        size_t lb_len = links[i].contact_b_len;
        if (!la || !lb)
            continue;
        if ((strncasecmp_eq(a, a_len, la, la_len) && strncasecmp_eq(b, b_len, lb, lb_len))
            || (strncasecmp_eq(a, a_len, lb, lb_len) && strncasecmp_eq(b, b_len, la, la_len)))
            return true;
    }
    return false;
}

const char *hu_social_rel_type_str(hu_social_rel_type_t t) {
    switch (t) {
    case HU_SOCIAL_FAMILY:
        return "family";
    case HU_SOCIAL_FRIEND:
        return "friend";
    case HU_SOCIAL_COWORKER:
        return "coworker";
    case HU_SOCIAL_ACQUAINTANCE:
        return "acquaintance";
    case HU_SOCIAL_PARTNER:
        return "partner";
    default:
        return "unknown";
    }
}

hu_error_t hu_social_graph_build_prompt(hu_allocator_t *alloc, const hu_social_link_t *links,
                                        size_t count, const char *contact_id, size_t cid_len,
                                        char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (count == 0 || !links) {
        char *empty = hu_strndup(alloc, "[No social links for contact]", 30);
        if (!empty)
            return HU_ERR_OUT_OF_MEMORY;
        *out = empty;
        *out_len = 30;
        return HU_OK;
    }
    char buf[HU_COGNITIVE_SQL_BUF];
    size_t pos = 0;
    size_t cap = sizeof(buf);
    int n = snprintf(buf + pos, cap - pos, "[SOCIAL NETWORK for contact]: They know: ");
    if (n < 0 || (size_t)n >= cap - pos)
        return HU_ERR_INVALID_ARGUMENT;
    pos += (size_t)n;
    bool first = true;
    for (size_t i = 0; i < count && pos < cap - 64; i++) {
        const char *other = NULL;
        size_t other_len = 0;
        if (contact_id && cid_len > 0 && links[i].contact_a && links[i].contact_b) {
            if (strncasecmp_eq(contact_id, cid_len, links[i].contact_a, links[i].contact_a_len)) {
                other = links[i].contact_b;
                other_len = links[i].contact_b_len;
            } else {
                other = links[i].contact_a;
                other_len = links[i].contact_a_len;
            }
        }
        if (other && other_len > 0) {
            const char *rel = hu_social_rel_type_str(links[i].rel_type);
            if (!first) {
                n = snprintf(buf + pos, cap - pos, ", ");
                if (n > 0 && (size_t)n < cap - pos)
                    pos += (size_t)n;
            }
            n = snprintf(buf + pos, cap - pos, "%.*s (%s)", (int)other_len, other, rel);
            if (n < 0 || (size_t)n >= cap - pos)
                break;
            pos += (size_t)n;
            first = false;
        }
    }
    *out = hu_strndup(alloc, buf, pos);
    if (!*out)
        return HU_ERR_OUT_OF_MEMORY;
    *out_len = pos;
    return HU_OK;
}

void hu_social_link_deinit(hu_allocator_t *alloc, hu_social_link_t *link) {
    if (!alloc || !link)
        return;
    hu_str_free(alloc, link->contact_a);
    hu_str_free(alloc, link->contact_b);
    hu_str_free(alloc, link->context);
    link->contact_a = NULL;
    link->contact_b = NULL;
    link->context = NULL;
    link->contact_a_len = 0;
    link->contact_b_len = 0;
    link->context_len = 0;
}
