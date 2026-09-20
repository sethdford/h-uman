#include "human/memory/evolved_opinions.h"

#ifdef HU_ENABLE_SQLITE

#include "human/core/string.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static bool word_eq_ci(const char *s, size_t n, const char *w) {
    size_t wl = strlen(w);
    return n == wl && strncasecmp(s, w, wl) == 0;
}

bool hu_evolved_opinion_stance_is_usable(const char *topic, size_t topic_len, const char *stance,
                                         size_t stance_len) {
    if (!topic || topic_len == 0 || !stance || stance_len == 0)
        return false;
    /* First word of the topic: a pronoun means the sentence is about the
     * speaker or the listener, not a subject anyone holds a position on. */
    size_t i = 0;
    while (i < topic_len && !isalnum((unsigned char)topic[i]))
        i++;
    size_t start = i;
    while (i < topic_len && (isalnum((unsigned char)topic[i]) || topic[i] == '\''))
        i++;
    static const char *const pronouns[] = {"i",   "i'm",    "i've",  "i'd",  "i'll",  "im",
                                           "you", "you're", "youre", "we",   "me",    "my",
                                           "it",  "it's",   "this",  "that", "there", NULL};
    for (size_t p = 0; pronouns[p]; p++)
        if (word_eq_ci(topic + start, i - start, pronouns[p]))
            return false;
    /* Identity / AI slips: the stance is the model narrating itself. */
    static const char *const slips[] = {
        "glitch",     "confusing me with", "as an ai",     "language model",         "i'm an ai",
        "i am an ai", "i'm here to",       "i am here to", "not sure what you mean", NULL};
    for (size_t k = 0; slips[k]; k++) {
        size_t sl = strlen(slips[k]);
        for (size_t j = 0; j + sl <= stance_len; j++)
            if (strncasecmp(stance + j, slips[k], sl) == 0)
                return false;
    }
    return true;
}

hu_error_t hu_evolved_opinions_ensure_table(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    const char *sql = "CREATE TABLE IF NOT EXISTS evolved_opinions ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "topic TEXT NOT NULL UNIQUE, "
                      "stance TEXT NOT NULL, "
                      "conviction REAL NOT NULL DEFAULT 0.5, "
                      "formed_at INTEGER NOT NULL, "
                      "interactions INTEGER NOT NULL DEFAULT 1, "
                      "updated_at INTEGER NOT NULL)";
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        if (errmsg)
            sqlite3_free(errmsg);
        return HU_ERR_IO;
    }
    return HU_OK;
}

hu_error_t hu_evolved_opinion_upsert(sqlite3 *db, const char *topic, size_t topic_len,
                                     const char *stance, size_t stance_len, double conviction,
                                     int64_t now_ts) {
    if (!db || !topic || topic_len == 0 || !stance || stance_len == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (conviction < 0.0)
        conviction = 0.0;
    if (conviction > 1.0)
        conviction = 1.0;

    const char *sql = "INSERT INTO evolved_opinions (topic, stance, conviction, formed_at, "
                      "interactions, updated_at) "
                      "VALUES (?1, ?2, ?3, ?4, 1, ?4) "
                      "ON CONFLICT(topic) DO UPDATE SET "
                      "stance = ?2, "
                      "conviction = (conviction + ?3) / 2.0, "
                      "interactions = interactions + 1, "
                      "updated_at = ?4";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(stmt, 1, topic, (int)topic_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, stance, (int)stance_len, SQLITE_STATIC);
    sqlite3_bind_double(stmt, 3, conviction);
    sqlite3_bind_int64(stmt, 4, now_ts);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_IO;
}

/* Find the end of the current sentence (period, newline, or end of string) */
static size_t find_sentence_end(const char *s, size_t start, size_t len) {
    for (size_t i = start; i < len; i++) {
        if (s[i] == '.' || s[i] == '\n' || s[i] == '!' || s[i] == '?')
            return i;
    }
    return len;
}

/* Extract a short topic slug from the first few words after the opinion marker */
static size_t extract_topic(const char *s, size_t start, size_t len, char *out, size_t out_cap) {
    size_t pos = start;
    /* Skip leading whitespace */
    while (pos < len && (s[pos] == ' ' || s[pos] == '\t'))
        pos++;
    /* Skip common filler words: "that", "the", "a", "an" */
    static const char *fillers[] = {"that ", "the ", "a ", "an "};
    for (size_t f = 0; f < 4; f++) {
        size_t fl = strlen(fillers[f]);
        if (pos + fl <= len && strncasecmp(s + pos, fillers[f], fl) == 0) {
            pos += fl;
            break;
        }
    }
    /* Take up to 5 words or 60 chars */
    size_t word_count = 0;
    size_t end = pos;
    while (end < len && word_count < 5 && (end - pos) < 60) {
        if (s[end] == ' ')
            word_count++;
        if (s[end] == '.' || s[end] == ',' || s[end] == '\n')
            break;
        end++;
    }
    /* Trim trailing whitespace */
    while (end > pos && s[end - 1] == ' ')
        end--;
    size_t tlen = end - pos;
    if (tlen == 0 || tlen >= out_cap)
        return 0;
    memcpy(out, s + pos, tlen);
    out[tlen] = '\0';
    return tlen;
}

hu_error_t hu_evolved_opinions_extract_and_store(sqlite3 *db, const char *response,
                                                 size_t response_len, int64_t now_ts) {
    if (!db || !response || response_len == 0)
        return HU_ERR_INVALID_ARGUMENT;

    static const struct {
        const char *phrase;
        size_t len;
    } markers[] = {
        {"I think ", 8},        {"I believe ", 10},
        {"I prefer ", 9},       {"I'd argue ", 10},
        {"I feel like ", 12},   {"my take is ", 11},
        {"in my opinion ", 14}, {"I've come to believe ", 21},
        {"I'm convinced ", 14}, {"honestly, I ", 12},
    };
    static const size_t marker_count = sizeof(markers) / sizeof(markers[0]);

    hu_evolved_opinions_ensure_table(db);
    size_t stored = 0;

    for (size_t i = 0; i + 8 <= response_len && stored < 3; i++) {
        for (size_t m = 0; m < marker_count; m++) {
            if (i + markers[m].len > response_len)
                continue;
            if (strncasecmp(response + i, markers[m].phrase, markers[m].len) != 0)
                continue;

            size_t after = i + markers[m].len;
            size_t sent_end = find_sentence_end(response, after, response_len);

            char topic[64];
            size_t topic_len = extract_topic(response, after, response_len, topic, sizeof(topic));
            if (topic_len < 3) {
                i = sent_end;
                break;
            }

            /* Stance = everything from the marker to end of sentence */
            size_t stance_start = i;
            size_t stance_len = sent_end - stance_start;
            if (stance_len < 10 || stance_len > 300) {
                i = sent_end;
                break;
            }
            /* Judge the WHOLE sentence, not just marker..end: "As an AI language
             * model I think that is fair" carries its slip before the marker. */
            size_t sent_start = i;
            while (sent_start > 0 && response[sent_start - 1] != '.' &&
                   response[sent_start - 1] != '!' && response[sent_start - 1] != '?' &&
                   response[sent_start - 1] != '\n')
                sent_start--;
            if (!hu_evolved_opinion_stance_is_usable(topic, topic_len, response + sent_start,
                                                     sent_end - sent_start)) {
                i = sent_end;
                break;
            }

            hu_evolved_opinion_upsert(db, topic, topic_len, response + stance_start, stance_len,
                                      0.5, now_ts);
            stored++;
            i = sent_end;
            break;
        }
    }

    return HU_OK;
}

hu_error_t hu_evolved_opinions_get(hu_allocator_t *alloc, sqlite3 *db, double min_conviction,
                                   size_t limit, hu_evolved_opinion_t **out, size_t *out_count) {
    if (!alloc || !db || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;
    if (limit == 0)
        limit = 10;

    const char *sql = "SELECT topic, stance, conviction, formed_at, interactions "
                      "FROM evolved_opinions WHERE conviction >= ?1 "
                      "ORDER BY conviction DESC, interactions DESC LIMIT ?2";

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_double(stmt, 1, min_conviction);
    sqlite3_bind_int(stmt, 2, (int)limit);

    hu_evolved_opinion_t *opinions = NULL;
    size_t count = 0;
    size_t cap = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count >= cap) {
            size_t new_cap = cap ? cap * 2 : 4;
            hu_evolved_opinion_t *arr = (hu_evolved_opinion_t *)alloc->alloc(
                alloc->ctx, new_cap * sizeof(hu_evolved_opinion_t));
            if (!arr) {
                sqlite3_finalize(stmt);
                hu_evolved_opinions_free(alloc, opinions, count);
                return HU_ERR_OUT_OF_MEMORY;
            }
            if (opinions) {
                memcpy(arr, opinions, count * sizeof(hu_evolved_opinion_t));
                alloc->free(alloc->ctx, opinions, cap * sizeof(hu_evolved_opinion_t));
            }
            opinions = arr;
            cap = new_cap;
        }

        const char *topic = (const char *)sqlite3_column_text(stmt, 0);
        int topic_len = sqlite3_column_bytes(stmt, 0);
        const char *stance = (const char *)sqlite3_column_text(stmt, 1);
        int stance_len = sqlite3_column_bytes(stmt, 1);

        hu_evolved_opinion_t *op = &opinions[count];
        memset(op, 0, sizeof(*op));
        op->topic = hu_strndup(alloc, topic, (size_t)topic_len);
        op->topic_len = (size_t)topic_len;
        op->stance = hu_strndup(alloc, stance, (size_t)stance_len);
        op->stance_len = (size_t)stance_len;
        op->conviction = sqlite3_column_double(stmt, 2);
        op->formed_at = sqlite3_column_int64(stmt, 3);
        op->interactions = (uint32_t)sqlite3_column_int(stmt, 4);
        count++;
    }

    sqlite3_finalize(stmt);

    /* Shrink to exact size */
    if (count > 0 && count < cap) {
        hu_evolved_opinion_t *shrunk =
            (hu_evolved_opinion_t *)alloc->alloc(alloc->ctx, count * sizeof(hu_evolved_opinion_t));
        if (shrunk) {
            memcpy(shrunk, opinions, count * sizeof(hu_evolved_opinion_t));
            alloc->free(alloc->ctx, opinions, cap * sizeof(hu_evolved_opinion_t));
            opinions = shrunk;
        }
    }

    *out = opinions;
    *out_count = count;
    return HU_OK;
}

void hu_evolved_opinions_free(hu_allocator_t *alloc, hu_evolved_opinion_t *opinions, size_t count) {
    if (!alloc || !opinions)
        return;
    for (size_t i = 0; i < count; i++) {
        if (opinions[i].topic)
            alloc->free(alloc->ctx, opinions[i].topic, opinions[i].topic_len + 1);
        if (opinions[i].stance)
            alloc->free(alloc->ctx, opinions[i].stance, opinions[i].stance_len + 1);
    }
    alloc->free(alloc->ctx, opinions, count * sizeof(hu_evolved_opinion_t));
}

/* ── Anti-sycophancy: find existing opinion by topic ─────────────────── */

hu_error_t hu_evolved_opinion_find(hu_allocator_t *alloc, sqlite3 *db, const char *topic,
                                   size_t topic_len, hu_evolved_opinion_t *out, bool *found) {
    if (!alloc || !db || !topic || topic_len == 0 || !out || !found)
        return HU_ERR_INVALID_ARGUMENT;
    *found = false;
    memset(out, 0, sizeof(*out));

    const char *sql = "SELECT topic, stance, conviction, formed_at, interactions "
                      "FROM evolved_opinions WHERE topic = ?1 LIMIT 1";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(stmt, 1, topic, (int)topic_len, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        const char *t = (const char *)sqlite3_column_text(stmt, 0);
        int tl = sqlite3_column_bytes(stmt, 0);
        const char *s = (const char *)sqlite3_column_text(stmt, 1);
        int sl = sqlite3_column_bytes(stmt, 1);
        out->topic = hu_strndup(alloc, t, (size_t)tl);
        out->topic_len = (size_t)tl;
        out->stance = hu_strndup(alloc, s, (size_t)sl);
        out->stance_len = (size_t)sl;
        out->conviction = sqlite3_column_double(stmt, 2);
        out->formed_at = sqlite3_column_int64(stmt, 3);
        out->interactions = (uint32_t)sqlite3_column_int(stmt, 4);
        *found = true;
    }
    sqlite3_finalize(stmt);
    return HU_OK;
}

char *hu_opinion_check_before_agree(hu_allocator_t *alloc, sqlite3 *db, const char *topic,
                                    size_t topic_len, size_t *out_len) {
    if (!alloc || !db || !topic || topic_len == 0)
        return NULL;
    if (out_len)
        *out_len = 0;

    hu_evolved_opinion_t op;
    bool found = false;
    hu_error_t err = hu_evolved_opinion_find(alloc, db, topic, topic_len, &op, &found);
    if (err != HU_OK || !found)
        return NULL;

    /* Only inject directive for opinions with conviction > 0.5 */
    if (op.conviction <= 0.5) {
        if (op.topic)
            alloc->free(alloc->ctx, op.topic, op.topic_len + 1);
        if (op.stance)
            alloc->free(alloc->ctx, op.stance, op.stance_len + 1);
        return NULL;
    }

#define HU_OPINION_DIRECTIVE_CAP 512
    char *buf = (char *)alloc->alloc(alloc->ctx, HU_OPINION_DIRECTIVE_CAP);
    if (!buf) {
        alloc->free(alloc->ctx, op.topic, op.topic_len + 1);
        alloc->free(alloc->ctx, op.stance, op.stance_len + 1);
        return NULL;
    }

    int n = snprintf(buf, HU_OPINION_DIRECTIVE_CAP,
                     "You have a view on %.*s: %.*s. Lead with your actual opinion.",
                     (int)op.topic_len, op.topic, (int)op.stance_len, op.stance);

    alloc->free(alloc->ctx, op.topic, op.topic_len + 1);
    alloc->free(alloc->ctx, op.stance, op.stance_len + 1);

    if (n <= 0 || (size_t)n >= HU_OPINION_DIRECTIVE_CAP) {
        alloc->free(alloc->ctx, buf, HU_OPINION_DIRECTIVE_CAP);
        return NULL;
    }

    size_t need = (size_t)n + 1;
    char *shrunk = (char *)alloc->realloc(alloc->ctx, buf, HU_OPINION_DIRECTIVE_CAP, need);
    if (!shrunk) {
        alloc->free(alloc->ctx, buf, HU_OPINION_DIRECTIVE_CAP);
        return NULL;
    }
    if (out_len)
        *out_len = (size_t)n;
    return shrunk;
#undef HU_OPINION_DIRECTIVE_CAP
}

/* ── Deliberate disagreement (~15% of debatable topics) ──────────────── */

char *hu_opinion_contrarian_prompt(hu_allocator_t *alloc, const char *topic, size_t topic_len,
                                   uint32_t turn_counter, size_t *out_len) {
    if (!alloc || !topic || topic_len == 0)
        return NULL;
    if (out_len)
        *out_len = 0;

    /* Simple hash-based budget: ~15% of turns fire contrarian */
    uint32_t hash = 2166136261u; /* FNV-1a */
    for (size_t i = 0; i < topic_len; i++)
        hash = (hash ^ (uint8_t)topic[i]) * 16777619u;
    hash ^= turn_counter * 2654435761u;
    /* 15% ≈ hash % 100 < 15 */
    if ((hash % 100) >= 15)
        return NULL;

    const char *directive =
        "Consider offering a thoughtful counterpoint on this topic. "
        "Play devil's advocate with genuine reasoning, not just for the sake of disagreement.";
    size_t dlen = strlen(directive);
    char *buf = hu_strndup(alloc, directive, dlen);
    if (out_len && buf)
        *out_len = dlen;
    return buf;
}

/* ── Opinion history tracking ──────────────────────────────────────────── */

hu_error_t hu_opinion_history_ensure_table(sqlite3 *db) {
    if (!db)
        return HU_ERR_INVALID_ARGUMENT;
    const char *sql = "CREATE TABLE IF NOT EXISTS opinion_history ("
                      "id INTEGER PRIMARY KEY AUTOINCREMENT, "
                      "topic TEXT NOT NULL, "
                      "old_stance TEXT NOT NULL, "
                      "new_stance TEXT NOT NULL, "
                      "change_reason TEXT NOT NULL DEFAULT '', "
                      "changed_at INTEGER NOT NULL)";
    char *errmsg = NULL;
    int rc = sqlite3_exec(db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        if (errmsg)
            sqlite3_free(errmsg);
        return HU_ERR_IO;
    }
    return HU_OK;
}

hu_error_t hu_opinion_history_record(sqlite3 *db, const char *topic, size_t topic_len,
                                     const char *old_stance, size_t old_stance_len,
                                     const char *new_stance, size_t new_stance_len,
                                     const char *reason, size_t reason_len, int64_t changed_at) {
    if (!db || !topic || topic_len == 0 || !old_stance || !new_stance)
        return HU_ERR_INVALID_ARGUMENT;

    const char *sql = "INSERT INTO opinion_history (topic, old_stance, new_stance, "
                      "change_reason, changed_at) VALUES (?1, ?2, ?3, ?4, ?5)";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(stmt, 1, topic, (int)topic_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, old_stance, (int)old_stance_len, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, new_stance, (int)new_stance_len, SQLITE_STATIC);
    if (reason && reason_len > 0)
        sqlite3_bind_text(stmt, 4, reason, (int)reason_len, SQLITE_STATIC);
    else
        sqlite3_bind_text(stmt, 4, "", 0, SQLITE_STATIC);
    sqlite3_bind_int64(stmt, 5, changed_at);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_IO;
}

hu_error_t hu_evolved_opinion_history(hu_allocator_t *alloc, sqlite3 *db, const char *topic,
                                      size_t topic_len, hu_opinion_history_entry_t **out,
                                      size_t *out_count) {
    if (!alloc || !db || !topic || topic_len == 0 || !out || !out_count)
        return HU_ERR_INVALID_ARGUMENT;
    *out = NULL;
    *out_count = 0;

    const char *sql = "SELECT topic, old_stance, new_stance, change_reason, changed_at "
                      "FROM opinion_history WHERE topic = ?1 ORDER BY changed_at ASC LIMIT 50";
    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_IO;

    sqlite3_bind_text(stmt, 1, topic, (int)topic_len, SQLITE_STATIC);

    hu_opinion_history_entry_t *entries = NULL;
    size_t count = 0;
    size_t cap = 0;

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count >= cap) {
            size_t new_cap = cap ? cap * 2 : 4;
            hu_opinion_history_entry_t *arr = (hu_opinion_history_entry_t *)alloc->alloc(
                alloc->ctx, new_cap * sizeof(hu_opinion_history_entry_t));
            if (!arr) {
                sqlite3_finalize(stmt);
                hu_opinion_history_free(alloc, entries, count);
                return HU_ERR_OUT_OF_MEMORY;
            }
            if (entries) {
                memcpy(arr, entries, count * sizeof(hu_opinion_history_entry_t));
                alloc->free(alloc->ctx, entries, cap * sizeof(hu_opinion_history_entry_t));
            }
            entries = arr;
            cap = new_cap;
        }

        hu_opinion_history_entry_t *e = &entries[count];
        memset(e, 0, sizeof(*e));

        const char *t = (const char *)sqlite3_column_text(stmt, 0);
        int tl = sqlite3_column_bytes(stmt, 0);
        const char *os = (const char *)sqlite3_column_text(stmt, 1);
        int osl = sqlite3_column_bytes(stmt, 1);
        const char *ns = (const char *)sqlite3_column_text(stmt, 2);
        int nsl = sqlite3_column_bytes(stmt, 2);
        const char *cr = (const char *)sqlite3_column_text(stmt, 3);
        int crl = sqlite3_column_bytes(stmt, 3);

        e->topic = hu_strndup(alloc, t, (size_t)tl);
        e->topic_len = (size_t)tl;
        e->old_stance = hu_strndup(alloc, os, (size_t)osl);
        e->old_stance_len = (size_t)osl;
        e->new_stance = hu_strndup(alloc, ns, (size_t)nsl);
        e->new_stance_len = (size_t)nsl;
        e->change_reason = hu_strndup(alloc, cr, (size_t)crl);
        e->change_reason_len = (size_t)crl;
        e->changed_at = sqlite3_column_int64(stmt, 4);
        count++;
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        hu_opinion_history_free(alloc, entries, count);
        *out = NULL;
        *out_count = 0;
        return HU_ERR_MEMORY_BACKEND;
    }
    *out = entries;
    *out_count = count;
    return HU_OK;
}

void hu_opinion_history_free(hu_allocator_t *alloc, hu_opinion_history_entry_t *entries,
                             size_t count) {
    if (!alloc || !entries)
        return;
    for (size_t i = 0; i < count; i++) {
        if (entries[i].topic)
            alloc->free(alloc->ctx, entries[i].topic, entries[i].topic_len + 1);
        if (entries[i].old_stance)
            alloc->free(alloc->ctx, entries[i].old_stance, entries[i].old_stance_len + 1);
        if (entries[i].new_stance)
            alloc->free(alloc->ctx, entries[i].new_stance, entries[i].new_stance_len + 1);
        if (entries[i].change_reason)
            alloc->free(alloc->ctx, entries[i].change_reason, entries[i].change_reason_len + 1);
    }
    alloc->free(alloc->ctx, entries, count * sizeof(hu_opinion_history_entry_t));
}

/* ── Upsert with history tracking and narrative ──────────────────────── */

char *hu_evolved_opinion_upsert_with_history(hu_allocator_t *alloc, sqlite3 *db, const char *topic,
                                             size_t topic_len, const char *stance,
                                             size_t stance_len, double conviction, int64_t now_ts,
                                             const char *reason, size_t reason_len,
                                             uint32_t opinion_changes_this_convo, size_t *out_len) {
    if (!alloc || !db || !topic || topic_len == 0 || !stance || stance_len == 0)
        return NULL;
    if (out_len)
        *out_len = 0;

    hu_opinion_history_ensure_table(db);

    /* Fetch existing opinion before upsert */
    hu_evolved_opinion_t old_op;
    bool had_old = false;
    hu_evolved_opinion_find(alloc, db, topic, topic_len, &old_op, &had_old);

    /* Perform the upsert */
    hu_evolved_opinion_upsert(db, topic, topic_len, stance, stance_len, conviction, now_ts);

    char *narrative = NULL;

    if (had_old) {
        double old_conv = old_op.conviction;
        /* New conviction after blend: (old + new) / 2 */
        double new_conv = (old_conv + conviction) / 2.0;
        double shift = new_conv - old_conv;
        if (shift < 0.0)
            shift = -shift;

        /* Record in history if stance changed */
        if (old_op.stance && strcmp(old_op.stance, stance) != 0) {
            hu_opinion_history_record(db, topic, topic_len, old_op.stance, old_op.stance_len,
                                      stance, stance_len, reason, reason_len, now_ts);
        }

        /* Generate narrative only if conviction shift > 0.2 and within budget */
        if (shift > 0.2 && opinion_changes_this_convo < 2) {
#define HU_NARRATIVE_CAP 512
            narrative = (char *)alloc->alloc(alloc->ctx, HU_NARRATIVE_CAP);
            if (narrative) {
                int n = snprintf(narrative, HU_NARRATIVE_CAP,
                                 "You've shifted on %.*s. Acknowledge: "
                                 "'I've been rethinking this...'",
                                 (int)topic_len, topic);
                if (n > 0 && (size_t)n < HU_NARRATIVE_CAP) {
                    size_t need = (size_t)n + 1;
                    char *shrunk =
                        (char *)alloc->realloc(alloc->ctx, narrative, HU_NARRATIVE_CAP, need);
                    if (shrunk)
                        narrative = shrunk;
                    if (out_len)
                        *out_len = (size_t)n;
                } else {
                    alloc->free(alloc->ctx, narrative, HU_NARRATIVE_CAP);
                    narrative = NULL;
                }
            }
#undef HU_NARRATIVE_CAP
        }

        if (old_op.topic)
            alloc->free(alloc->ctx, old_op.topic, old_op.topic_len + 1);
        if (old_op.stance)
            alloc->free(alloc->ctx, old_op.stance, old_op.stance_len + 1);
    }

    return narrative;
}

#endif /* HU_ENABLE_SQLITE */
