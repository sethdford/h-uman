#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/core/string.h"
#include "human/memory/deep_memory.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define HU_DEEP_MEMORY_ESCAPE_BUF 1024
#define HU_DEEP_MEMORY_SQL_BUF 4096

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

/* Tokenize by whitespace into a small stack buffer; returns count. */
static size_t tokenize(const char *s, size_t len, const char *tokens[64], size_t *lens) {
    size_t n = 0;
    size_t i = 0;
    while (i < len && n < 64) {
        while (i < len && (unsigned char)s[i] <= ' ')
            i++;
        if (i >= len)
            break;
        const char *start = s + i;
        while (i < len && (unsigned char)s[i] > ' ')
            i++;
        tokens[n] = start;
        lens[n] = (size_t)(s + i - start);
        n++;
    }
    return n;
}

/* Word-overlap score 0-1: Jaccard-like (intersection / union). */
static double word_overlap_score(const char *a, size_t a_len, const char *b, size_t b_len) {
    const char *ta[64], *tb[64];
    size_t la[64], lb[64];
    size_t na = tokenize(a, a_len, ta, la);
    size_t nb = tokenize(b, b_len, tb, lb);
    if (na == 0 && nb == 0)
        return 1.0;
    if (na == 0 || nb == 0)
        return 0.0;

    size_t inter = 0;
    for (size_t i = 0; i < na; i++) {
        for (size_t j = 0; j < nb; j++) {
            if (la[i] == lb[j] && memcmp(ta[i], tb[j], la[i]) == 0) {
                inter++;
                break;
            }
        }
    }
    size_t un = na + nb - inter;
    return un > 0 ? (double)inter / (double)un : 0.0;
}

/* --- F70 Episodic Memory --- */
hu_error_t hu_episodic_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    int n = snprintf(buf, cap,
                    "CREATE TABLE IF NOT EXISTS episodes ("
                    "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                    "summary TEXT NOT NULL, "
                    "emotional_arc TEXT, "
                    "impact_score REAL NOT NULL, "
                    "participants TEXT, "
                    "occurred_at INTEGER NOT NULL, "
                    "source_tag TEXT)");
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_episodic_insert_sql(const hu_episode_t *ep, char *buf, size_t cap, size_t *out_len) {
    if (!ep || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    if (!ep->summary)
        return HU_ERR_INVALID_ARGUMENT;

    char sum_esc[HU_DEEP_MEMORY_ESCAPE_BUF];
    char arc_esc[HU_DEEP_MEMORY_ESCAPE_BUF];
    char part_esc[HU_DEEP_MEMORY_ESCAPE_BUF];
    char tag_esc[HU_DEEP_MEMORY_ESCAPE_BUF];
    size_t se_len, ae_len, pe_len, te_len;

    escape_sql_string(ep->summary, ep->summary_len, sum_esc, sizeof(sum_esc), &se_len);
    escape_sql_string(ep->emotional_arc ? ep->emotional_arc : "", ep->emotional_arc_len, arc_esc,
                     sizeof(arc_esc), &ae_len);
    escape_sql_string(ep->participants ? ep->participants : "", ep->participants_len, part_esc,
                     sizeof(part_esc), &pe_len);
    escape_sql_string(ep->source_tag ? ep->source_tag : "", ep->source_tag_len, tag_esc,
                     sizeof(tag_esc), &te_len);

    int n = snprintf(buf, cap,
                    "INSERT INTO episodes (summary, emotional_arc, impact_score, participants, "
                    "occurred_at, source_tag) VALUES ('%.*s', '%.*s', %f, '%.*s', %llu, '%.*s')",
                    (int)se_len, sum_esc, (int)ae_len, arc_esc, ep->impact_score, (int)pe_len,
                    part_esc, (unsigned long long)ep->occurred_at, (int)te_len, tag_esc);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_episodic_query_by_contact_sql(const char *contact_id, size_t len, uint32_t limit,
                                            char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    char contact_esc[HU_DEEP_MEMORY_ESCAPE_BUF];
    size_t ce_len;
    escape_sql_string(contact_id ? contact_id : "", len, contact_esc, sizeof(contact_esc),
                     &ce_len);

    int n = snprintf(buf, cap,
                    "SELECT id, summary, emotional_arc, impact_score, participants, occurred_at, "
                    "source_tag FROM episodes WHERE participants LIKE '%%\"%.*s\"%%' ORDER BY "
                    "occurred_at DESC LIMIT %u",
                    (int)ce_len, contact_esc, limit);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_episodic_query_high_impact_sql(double min_impact, uint32_t limit, char *buf,
                                             size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    int n = snprintf(buf, cap,
                    "SELECT id, summary, emotional_arc, impact_score, participants, occurred_at, "
                    "source_tag FROM episodes WHERE impact_score >= %f ORDER BY impact_score DESC, "
                    "occurred_at DESC LIMIT %u",
                    min_impact, limit);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

/* --- F71 Associative Recall --- */
double hu_episodic_relevance_score(const char *episode_summary, size_t summary_len,
                                   const char *trigger, size_t trigger_len) {
    if (!episode_summary || !trigger)
        return 0.0;
    return word_overlap_score(episode_summary, summary_len, trigger, trigger_len);
}

/* --- F72 Consolidation Engine --- */
bool hu_consolidation_should_merge(const char *a, size_t a_len, const char *b, size_t b_len,
                                  double threshold) {
    if (!a || !b || threshold < 0.0 || threshold > 1.0)
        return false;
    return word_overlap_score(a, a_len, b, b_len) >= threshold;
}

hu_error_t hu_consolidation_merge_sql(int64_t keep_id, int64_t remove_id, char *buf, size_t cap,
                                      size_t *out_len) {
    if (!buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;
    (void)keep_id;
    int n = snprintf(buf, cap, "DELETE FROM episodes WHERE id=%lld", (long long)remove_id);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

/* --- F75 Prospective Memory --- */
hu_error_t hu_prospective_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    int n = snprintf(buf, cap,
                    "CREATE TABLE IF NOT EXISTS prospective_memory ("
                    "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                    "description TEXT NOT NULL, "
                    "trigger_type TEXT NOT NULL, "
                    "trigger_value TEXT, "
                    "created_at INTEGER NOT NULL, "
                    "completed INTEGER NOT NULL DEFAULT 0)");
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_prospective_insert_sql(const hu_prospective_item_t *item, char *buf, size_t cap,
                                    size_t *out_len) {
    if (!item || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    if (!item->description || !item->trigger_type)
        return HU_ERR_INVALID_ARGUMENT;

    char desc_esc[HU_DEEP_MEMORY_ESCAPE_BUF];
    char type_esc[HU_DEEP_MEMORY_ESCAPE_BUF];
    char val_esc[HU_DEEP_MEMORY_ESCAPE_BUF];
    size_t de_len, te_len, ve_len;

    escape_sql_string(item->description, item->description_len, desc_esc, sizeof(desc_esc),
                     &de_len);
    escape_sql_string(item->trigger_type, item->trigger_type_len, type_esc, sizeof(type_esc),
                     &te_len);
    escape_sql_string(item->trigger_value ? item->trigger_value : "", item->trigger_value_len,
                     val_esc, sizeof(val_esc), &ve_len);

    int n = snprintf(buf, cap,
                    "INSERT INTO prospective_memory (description, trigger_type, trigger_value, "
                    "created_at, completed) VALUES ('%.*s', '%.*s', '%.*s', %llu, %d)",
                    (int)de_len, desc_esc, (int)te_len, type_esc, (int)ve_len, val_esc,
                    (unsigned long long)item->created_at, item->completed ? 1 : 0);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_prospective_query_pending_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;
    int n = snprintf(buf, cap,
                    "SELECT id, description, trigger_type, trigger_value, created_at, completed "
                    "FROM prospective_memory WHERE completed=0 ORDER BY created_at ASC");
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_prospective_complete_sql(int64_t id, char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 128)
        return HU_ERR_INVALID_ARGUMENT;
    int n = snprintf(buf, cap, "UPDATE prospective_memory SET completed=1 WHERE id=%lld",
                    (long long)id);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_prospective_build_prompt(hu_allocator_t *alloc,
                                       const hu_prospective_item_t *items, size_t count,
                                       char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (count == 0 || !items) {
        char *empty = hu_strndup(alloc, "[No pending reminders]", 24);
        if (!empty)
            return HU_ERR_OUT_OF_MEMORY;
        *out = empty;
        *out_len = 24;
        return HU_OK;
    }
    size_t total = 128;
    for (size_t i = 0; i < count; i++) {
        if (items[i].description)
            total += 16 + items[i].description_len + items[i].trigger_type_len
                     + items[i].trigger_value_len + 32;
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, total);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, total - pos, "[PENDING REMINDERS]:");
    for (size_t i = 0; i < count && pos < total - 2; i++) {
        const char *d = items[i].description ? items[i].description : "";
        size_t dl = items[i].description_len;
        const char *tt = items[i].trigger_type ? items[i].trigger_type : "";
        size_t tt_len = items[i].trigger_type_len;
        const char *tv = items[i].trigger_value ? items[i].trigger_value : "";
        size_t tv_len = items[i].trigger_value_len;
        pos += (size_t)snprintf(buf + pos, total - pos, " - %.*s (%.*s: %.*s)", (int)dl, d,
                               (int)tt_len, tt, (int)tv_len, tv);
    }
    *out = buf;
    *out_len = strlen(buf);
    return HU_OK;
}

/* --- F76 Emotional Residue --- */
hu_error_t hu_residue_create_table_sql(char *buf, size_t cap, size_t *out_len) {
    if (!buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    int n = snprintf(buf, cap,
                    "CREATE TABLE IF NOT EXISTS emotional_residue ("
                    "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                    "contact_id TEXT NOT NULL, "
                    "emotion TEXT NOT NULL, "
                    "intensity REAL NOT NULL, "
                    "from_timestamp INTEGER NOT NULL, "
                    "decay_rate REAL NOT NULL)");
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_residue_insert_sql(const hu_emotional_residue_t *r, char *buf, size_t cap,
                                size_t *out_len) {
    if (!r || !buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;
    if (!r->contact_id || !r->emotion)
        return HU_ERR_INVALID_ARGUMENT;

    char cid_esc[HU_DEEP_MEMORY_ESCAPE_BUF];
    char emo_esc[HU_DEEP_MEMORY_ESCAPE_BUF];
    size_t ce_len, ee_len;

    escape_sql_string(r->contact_id, r->contact_id_len, cid_esc, sizeof(cid_esc), &ce_len);
    escape_sql_string(r->emotion, r->emotion_len, emo_esc, sizeof(emo_esc), &ee_len);

    int n = snprintf(buf, cap,
                    "INSERT INTO emotional_residue (contact_id, emotion, intensity, from_timestamp, "
                    "decay_rate) VALUES ('%.*s', '%.*s', %f, %llu, %f)",
                    (int)ce_len, cid_esc, (int)ee_len, emo_esc, r->intensity,
                    (unsigned long long)r->from_timestamp, r->decay_rate);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

hu_error_t hu_residue_query_active_sql(const char *contact_id, size_t len, char *buf, size_t cap,
                                       size_t *out_len) {
    if (!buf || !out_len || cap < 256)
        return HU_ERR_INVALID_ARGUMENT;

    char cid_esc[HU_DEEP_MEMORY_ESCAPE_BUF];
    size_t ce_len;
    escape_sql_string(contact_id ? contact_id : "", len, cid_esc, sizeof(cid_esc), &ce_len);

    int n = snprintf(buf, cap,
                    "SELECT id, contact_id, emotion, intensity, from_timestamp, decay_rate "
                    "FROM emotional_residue WHERE contact_id='%.*s' ORDER BY from_timestamp DESC",
                    (int)ce_len, cid_esc);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    *out_len = (size_t)n;
    return HU_OK;
}

double hu_residue_current_intensity(double initial_intensity, double decay_rate,
                                   double hours_elapsed) {
    if (initial_intensity <= 0.0 || hours_elapsed < 0.0)
        return 0.0;
    return initial_intensity * exp(-decay_rate * hours_elapsed);
}

hu_error_t hu_residue_build_prompt(hu_allocator_t *alloc,
                                  const hu_emotional_residue_t *residues, size_t count,
                                  double hours_elapsed, char **out, size_t *out_len) {
    if (!alloc || !out || !out_len)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_len = 0;
    if (count == 0 || !residues) {
        char *empty = hu_strndup(alloc, "[No emotional residue]", 21);
        if (!empty)
            return HU_ERR_OUT_OF_MEMORY;
        *out = empty;
        *out_len = 21;
        return HU_OK;
    }
    size_t total = 128;
    for (size_t i = 0; i < count; i++) {
        if (residues[i].emotion)
            total += 24 + residues[i].emotion_len + 48;
    }
    char *buf = (char *)alloc->alloc(alloc->ctx, total);
    if (!buf)
        return HU_ERR_OUT_OF_MEMORY;
    size_t pos = 0;
    pos += (size_t)snprintf(buf + pos, total - pos, "[EMOTIONAL RESIDUE]:");
    for (size_t i = 0; i < count && pos < total - 2; i++) {
        const char *e = residues[i].emotion ? residues[i].emotion : "";
        size_t el = residues[i].emotion_len;
        double cur =
            hu_residue_current_intensity(residues[i].intensity, residues[i].decay_rate,
                                         hours_elapsed);
        pos += (size_t)snprintf(buf + pos, total - pos, " - %.*s (intensity: %.2f)", (int)el, e,
                               cur);
    }
    *out = buf;
    *out_len = strlen(buf);
    return HU_OK;
}

void hu_episode_deinit(hu_allocator_t *alloc, hu_episode_t *ep) {
    if (!alloc || !ep)
        return;
    hu_str_free(alloc, ep->summary);
    hu_str_free(alloc, ep->emotional_arc);
    hu_str_free(alloc, ep->participants);
    hu_str_free(alloc, ep->source_tag);
    ep->summary = NULL;
    ep->emotional_arc = NULL;
    ep->participants = NULL;
    ep->source_tag = NULL;
    ep->summary_len = 0;
    ep->emotional_arc_len = 0;
    ep->participants_len = 0;
    ep->source_tag_len = 0;
}

void hu_prospective_item_deinit(hu_allocator_t *alloc, hu_prospective_item_t *item) {
    if (!alloc || !item)
        return;
    hu_str_free(alloc, item->description);
    hu_str_free(alloc, item->trigger_type);
    hu_str_free(alloc, item->trigger_value);
    item->description = NULL;
    item->trigger_type = NULL;
    item->trigger_value = NULL;
    item->description_len = 0;
    item->trigger_type_len = 0;
    item->trigger_value_len = 0;
}

void hu_emotional_residue_deinit(hu_allocator_t *alloc, hu_emotional_residue_t *r) {
    if (!alloc || !r)
        return;
    hu_str_free(alloc, r->contact_id);
    hu_str_free(alloc, r->emotion);
    r->contact_id = NULL;
    r->emotion = NULL;
    r->contact_id_len = 0;
    r->emotion_len = 0;
}
