#ifdef HU_ENABLE_SQLITE

#include "human/context/style_tracker.h"
#include "human/core/debug.h"
#include "human/memory.h"
#include <ctype.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Laugh patterns: check longer first to avoid "ha" matching "haha". */
static const char *const LAUGH_PATTERNS[] = {"haha", "lol", "lmao", "hehe", "ha"};
static const size_t LAUGH_COUNT = sizeof(LAUGH_PATTERNS) / sizeof(LAUGH_PATTERNS[0]);

/* Minimal stopwords for distinctive-word filtering. */
static const char *const STOPWORDS[] = {
    "the",    "a",    "an",   "is",  "are",  "was",  "were", "be",    "been",
    "have",   "has",  "had",  "do",  "does", "did",  "will", "would", "could",
    "should", "to",   "of",   "in",  "for",  "on",   "with", "at",    "by",
    "it",     "that", "this", "and", "or",   "but",  "i",    "me",    "you",
    "we",     "they", "its",  "im",  "dont", "cant", "wont", "thats", "what",
};
static const size_t STOP_COUNT = sizeof(STOPWORDS) / sizeof(STOPWORDS[0]);

static bool is_stopword(const char *word, size_t len) {
    for (size_t i = 0; i < STOP_COUNT; i++) {
        size_t sw_len = strlen(STOPWORDS[i]);
        if (len == sw_len && strncasecmp(word, STOPWORDS[i], len) == 0)
            return true;
    }
    return false;
}

/* Extract laugh style from message; first match wins. */
static void extract_laugh_style(const char *msg, size_t msg_len, char *out, size_t out_cap) {
    out[0] = '\0';
    if (!msg || msg_len == 0 || out_cap < 4)
        return;
    for (size_t i = 0; i < LAUGH_COUNT; i++) {
        size_t pat_len = strlen(LAUGH_PATTERNS[i]);
        if (pat_len >= out_cap)
            continue;
        for (size_t j = 0; j + pat_len <= msg_len; j++) {
            if (strncasecmp(msg + j, LAUGH_PATTERNS[i], pat_len) == 0) {
                size_t copy = pat_len < out_cap - 1 ? pat_len : out_cap - 1;
                memcpy(out, LAUGH_PATTERNS[i], copy);
                out[copy] = '\0';
                return;
            }
        }
    }
}

/* Compute lowercase ratio; return true if >80%. */
static bool compute_uses_lowercase(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0)
        return false;
    size_t letters = 0;
    size_t lower = 0;
    for (size_t i = 0; i < msg_len; i++) {
        unsigned char c = (unsigned char)msg[i];
        if (isalpha(c)) {
            letters++;
            if (islower(c))
                lower++;
        }
    }
    return (letters > 0 && (lower * 100) / letters > 80);
}

/* Check if any sentence ends with period. */
static bool compute_uses_periods(const char *msg, size_t msg_len) {
    if (!msg || msg_len == 0)
        return false;
    for (size_t i = 0; i < msg_len; i++) {
        if (msg[i] == '.' &&
            (i + 1 >= msg_len || msg[i + 1] == ' ' || msg[i + 1] == '\n' || msg[i + 1] == '\0'))
            return true;
    }
    return false;
}

/* Collect words used >2x in message (lowercased), excluding stopwords.
 * Writes comma-separated to buf, max cap. */
static void extract_distinctive_words(const char *msg, size_t msg_len, char *buf, size_t cap) {
    buf[0] = '\0';
    if (!msg || msg_len == 0 || cap < 4)
        return;

#define MAX_WORDS 64
    char *words[MAX_WORDS];
    size_t counts[MAX_WORDS];
    size_t n = 0;

    for (size_t i = 0; i < MAX_WORDS; i++) {
        words[i] = NULL;
        counts[i] = 0;
    }

    size_t i = 0;
    while (i < msg_len && n < MAX_WORDS) {
        while (i < msg_len && !isalnum((unsigned char)msg[i]))
            i++;
        if (i >= msg_len)
            break;
        size_t start = i;
        while (i < msg_len && isalnum((unsigned char)msg[i]))
            i++;
        size_t len = i - start;
        if (len == 0 || len > 48)
            continue;

        char w[64];
        size_t copy = len < sizeof(w) - 1 ? len : sizeof(w) - 1;
        for (size_t k = 0; k < copy; k++)
            w[k] = (char)tolower((unsigned char)msg[start + k]);
        w[copy] = '\0';

        if (is_stopword(w, copy))
            continue;

        size_t slot = MAX_WORDS;
        for (size_t k = 0; k < n; k++) {
            if (strncmp(words[k], w, copy) == 0 && words[k][copy] == '\0') {
                counts[k]++;
                slot = k;
                break;
            }
        }
        if (slot == MAX_WORDS) {
            words[n] = (char *)malloc(copy + 1);
            if (words[n]) {
                memcpy(words[n], w, copy + 1);
                counts[n] = 1;
                n++;
            }
        }
    }

    size_t pos = 0;
    for (size_t k = 0; k < n && pos < cap - 1; k++) {
        if (counts[k] < 3)
            continue;
        size_t wlen = strlen(words[k]);
        if (pos > 0) {
            if (pos + 1 < cap)
                buf[pos++] = ',';
            else
                break;
        }
        if (pos + wlen < cap) {
            memcpy(buf + pos, words[k], wlen + 1);
            pos += wlen;
        } else
            break;
    }
    buf[pos] = '\0';

    for (size_t k = 0; k < n; k++)
        free(words[k]);
#undef MAX_WORDS
}

static void merge_distinctive_words(const char *existing, const char *new_words, char *out,
                                    size_t cap) {
    out[0] = '\0';
    if (cap < 4)
        return;
    if (new_words && new_words[0]) {
        if (existing && existing[0]) {
            int n = snprintf(out, cap, "%s,%s", existing, new_words);
            if (n < 0 || (size_t)n >= cap)
                out[cap - 1] = '\0';
        } else {
            size_t len = strlen(new_words);
            if (len >= cap)
                len = cap - 1;
            memcpy(out, new_words, len + 1);
        }
    } else if (existing && existing[0]) {
        size_t len = strlen(existing);
        if (len >= cap)
            len = cap - 1;
        memcpy(out, existing, len + 1);
    }
    if (strlen(out) >= cap - 1)
        out[cap - 1] = '\0';
}

#define MAX_PHRASE_LEN     48
#define MAX_PHRASE_ENTRIES 32
#define TOP_PHRASES        10

typedef struct {
    char phrase[MAX_PHRASE_LEN + 1];
    size_t count;
} phrase_entry_t;

/* Parse JSON array ["a","b","c"] and add each phrase to entries. Returns number added. */
static size_t parse_phrase_json(const char *json, phrase_entry_t *entries, size_t max_entries) {
    if (!json || !entries || max_entries == 0)
        return 0;
    const char *p = json;
    while (*p && *p != '[')
        p++;
    if (*p != '[')
        return 0;
    p++;
    size_t n = 0;
    while (n < max_entries) {
        while (*p == ' ' || *p == '\t' || *p == ',')
            p++;
        if (*p == ']' || *p == '\0')
            break;
        if (*p != '"')
            break;
        p++;
        const char *start = p;
        while (*p && *p != '"') {
            if (*p == '\\' && p[1])
                p++;
            p++;
        }
        if (!*p)
            break;
        size_t len = (size_t)(p - start);
        if (len > MAX_PHRASE_LEN)
            len = MAX_PHRASE_LEN;
        p++;
        int found = -1;
        for (size_t k = 0; k < n; k++) {
            if (strncmp(entries[k].phrase, start, len) == 0 && entries[k].phrase[len] == '\0') {
                entries[k].count++;
                found = (int)k;
                break;
            }
        }
        if (found < 0) {
            memcpy(entries[n].phrase, start, len);
            entries[n].phrase[len] = '\0';
            entries[n].count = 1;
            n++;
        }
    }
    return n;
}

/* Extract bigrams from message and add to entries. Returns number added. */
static size_t extract_bigrams(const char *msg, size_t msg_len, phrase_entry_t *entries,
                              size_t max_entries) {
    if (!msg || msg_len == 0 || !entries || max_entries == 0)
        return 0;

#define MAX_WORDS 64
    char *words[MAX_WORDS];
    size_t n_words = 0;
    for (size_t i = 0; i < MAX_WORDS; i++)
        words[i] = NULL;

    size_t i = 0;
    while (i < msg_len && n_words < MAX_WORDS) {
        while (i < msg_len && !isalnum((unsigned char)msg[i]))
            i++;
        if (i >= msg_len)
            break;
        size_t start = i;
        while (i < msg_len && isalnum((unsigned char)msg[i]))
            i++;
        size_t len = i - start;
        if (len == 0 || len > 48)
            continue;
        char w[64];
        size_t copy = len < sizeof(w) - 1 ? len : sizeof(w) - 1;
        for (size_t k = 0; k < copy; k++)
            w[k] = (char)tolower((unsigned char)msg[start + k]);
        w[copy] = '\0';
        if (is_stopword(w, copy))
            continue;
        words[n_words] = (char *)malloc(copy + 1);
        if (words[n_words]) {
            memcpy(words[n_words], w, copy + 1);
            n_words++;
        }
    }

    size_t n_entries = 0;
    for (size_t k = 0; k + 1 < n_words && n_entries < max_entries; k++) {
        size_t l1 = strlen(words[k]);
        size_t l2 = strlen(words[k + 1]);
        if (l1 + 1 + l2 > MAX_PHRASE_LEN)
            continue;
        char bigram[MAX_PHRASE_LEN + 1];
        int r = snprintf(bigram, sizeof(bigram), "%s %s", words[k], words[k + 1]);
        if (r <= 0 || (size_t)r >= sizeof(bigram))
            continue;
        int found = -1;
        for (size_t j = 0; j < n_entries; j++) {
            if (strcmp(entries[j].phrase, bigram) == 0) {
                entries[j].count++;
                found = (int)j;
                break;
            }
        }
        if (found < 0) {
            strncpy(entries[n_entries].phrase, bigram, MAX_PHRASE_LEN);
            entries[n_entries].phrase[MAX_PHRASE_LEN] = '\0';
            entries[n_entries].count = 1;
            n_entries++;
        }
    }

    for (size_t k = 0; k < n_words; k++)
        free(words[k]);
#undef MAX_WORDS
    return n_entries;
}

/* Sort entries by count descending. */
static int phrase_count_cmp(const void *a, const void *b) {
    const phrase_entry_t *pa = (const phrase_entry_t *)a;
    const phrase_entry_t *pb = (const phrase_entry_t *)b;
    if (pa->count > pb->count)
        return -1;
    if (pa->count < pb->count)
        return 1;
    return 0;
}

/* Build JSON array from top phrases. */
static void build_phrase_json(const phrase_entry_t *entries, size_t n, char *out, size_t cap) {
    out[0] = '\0';
    if (!out || cap < 4 || n == 0)
        return;
    size_t pos = 0;
    if (pos + 2 <= cap)
        out[pos++] = '[';
    for (size_t i = 0; i < n && i < TOP_PHRASES && pos < cap - 1; i++) {
        if (i > 0 && pos + 1 < cap)
            out[pos++] = ',';
        if (pos + 2 < cap)
            out[pos++] = '"';
        const char *p = entries[i].phrase;
        while (*p && pos < cap - 2) {
            if (*p == '"' || *p == '\\') {
                if (pos + 1 < cap)
                    out[pos++] = '\\';
            }
            if (pos < cap - 1)
                out[pos++] = *p++;
        }
        if (pos < cap - 1)
            out[pos++] = '"';
    }
    if (pos < cap - 1)
        out[pos++] = ']';
    out[pos] = '\0';
}

hu_error_t hu_style_fingerprint_update(hu_memory_t *memory, hu_allocator_t *alloc,
                                       const char *contact_id, size_t contact_id_len,
                                       const char *message, size_t message_len) {
    (void)alloc;
    if (!memory || !contact_id || contact_id_len == 0 || !message)
        return HU_ERR_INVALID_ARGUMENT;
    HU_ASSERT_NOT_REENTRANT(style_update);

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db) {
        HU_LEAVE_NOT_REENTRANT(style_update);
        return HU_ERR_NOT_SUPPORTED;
    }

    bool uses_lc = compute_uses_lowercase(message, message_len);
    bool uses_per = compute_uses_periods(message, message_len);
    char laugh[32];
    extract_laugh_style(message, message_len, laugh, sizeof(laugh));
    int msg_len_int = (int)(message_len > (size_t)INT_MAX ? INT_MAX : (int)message_len);

    char new_dist[512];
    extract_distinctive_words(message, message_len, new_dist, sizeof(new_dist));

    int64_t now_ts = (int64_t)time(NULL);

    sqlite3_stmt *sel = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT uses_lowercase, uses_periods, laugh_style, "
                                "avg_message_length, common_phrases, distinctive_words "
                                "FROM style_fingerprints WHERE contact_id=?",
                                -1, &sel, NULL);
    if (rc != SQLITE_OK) {
        HU_LEAVE_NOT_REENTRANT(style_update);
        return HU_ERR_MEMORY_BACKEND;
    }

    sqlite3_bind_text(sel, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);

    int old_lc = 0, old_per = 0;
    int old_avg = 0;
    const char *old_phrases = NULL;
    const char *old_dist = NULL;

    rc = sqlite3_step(sel);
    if (rc == SQLITE_ROW) {
        old_lc = sqlite3_column_int(sel, 0);
        old_per = sqlite3_column_int(sel, 1);
        const char *ls = (const char *)sqlite3_column_text(sel, 2);
        if (!laugh[0] && ls) {
            size_t ls_len = (size_t)sqlite3_column_bytes(sel, 2);
            if (ls_len >= sizeof(laugh))
                ls_len = sizeof(laugh) - 1;
            memcpy(laugh, ls, ls_len);
            laugh[ls_len] = '\0';
        }
        old_avg = sqlite3_column_int(sel, 3);
        old_phrases = (const char *)sqlite3_column_text(sel, 4);
        old_dist = (const char *)sqlite3_column_text(sel, 5);
    }
    sqlite3_finalize(sel);

    int new_lc = (old_lc || uses_lc) ? 1 : 0;
    int new_per = (old_per || uses_per) ? 1 : 0;
    int new_avg = (old_avg > 0) ? (old_avg + msg_len_int) / 2 : msg_len_int;

    char merged_dist[512];
    merge_distinctive_words(old_dist ? old_dist : "", new_dist, merged_dist, sizeof(merged_dist));

    phrase_entry_t phrase_entries[MAX_PHRASE_ENTRIES];
    phrase_entry_t new_entries[MAX_PHRASE_ENTRIES];
    memset(phrase_entries, 0, sizeof(phrase_entries));
    memset(new_entries, 0, sizeof(new_entries));
    size_t n_phrases = parse_phrase_json(old_phrases, phrase_entries, MAX_PHRASE_ENTRIES);
    size_t n_new = extract_bigrams(message, message_len, new_entries, MAX_PHRASE_ENTRIES);
    for (size_t j = 0; j < n_new && n_phrases < MAX_PHRASE_ENTRIES; j++) {
        int found = -1;
        for (size_t k = 0; k < n_phrases; k++) {
            if (strcmp(phrase_entries[k].phrase, new_entries[j].phrase) == 0) {
                phrase_entries[k].count += new_entries[j].count;
                found = (int)k;
                break;
            }
        }
        if (found < 0) {
            strncpy(phrase_entries[n_phrases].phrase, new_entries[j].phrase, MAX_PHRASE_LEN);
            phrase_entries[n_phrases].phrase[MAX_PHRASE_LEN] = '\0';
            phrase_entries[n_phrases].count = new_entries[j].count;
            n_phrases++;
        }
    }
    qsort(phrase_entries, n_phrases, sizeof(phrase_entry_t), phrase_count_cmp);

    char merged_phrases[512];
    build_phrase_json(phrase_entries, n_phrases, merged_phrases, sizeof(merged_phrases));

    sqlite3_stmt *stmt = NULL;
    rc = sqlite3_prepare_v2(db,
                            "INSERT OR REPLACE INTO style_fingerprints("
                            "contact_id, uses_lowercase, uses_periods, laugh_style, "
                            "avg_message_length, common_phrases, distinctive_words, updated_at) "
                            "VALUES(?,?,?,?,?,?,?,?)",
                            -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        HU_LEAVE_NOT_REENTRANT(style_update);
        return HU_ERR_MEMORY_BACKEND;
    }

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 2, new_lc);
    sqlite3_bind_int(stmt, 3, new_per);
    sqlite3_bind_text(stmt, 4, laugh[0] ? laugh : NULL, laugh[0] ? (int)strlen(laugh) : 0,
                      laugh[0] ? SQLITE_STATIC : (sqlite3_destructor_type)0);
    sqlite3_bind_int(stmt, 5, new_avg);
    sqlite3_bind_text(stmt, 6, merged_phrases[0] ? merged_phrases : NULL,
                      merged_phrases[0] ? (int)strlen(merged_phrases) : 0,
                      merged_phrases[0] ? SQLITE_STATIC : (sqlite3_destructor_type)0);
    sqlite3_bind_text(stmt, 7, merged_dist[0] ? merged_dist : NULL,
                      merged_dist[0] ? (int)strlen(merged_dist) : 0,
                      merged_dist[0] ? SQLITE_STATIC : (sqlite3_destructor_type)0);
    sqlite3_bind_int64(stmt, 8, now_ts);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    HU_LEAVE_NOT_REENTRANT(style_update);
    return (rc == SQLITE_DONE) ? HU_OK : HU_ERR_MEMORY_BACKEND;
}

hu_error_t hu_style_fingerprint_get(hu_memory_t *memory, hu_allocator_t *alloc,
                                    const char *contact_id, size_t contact_id_len,
                                    hu_style_fingerprint_t *out) {
    (void)alloc;
    if (!memory || !contact_id || contact_id_len == 0 || !out)
        return HU_ERR_INVALID_ARGUMENT;

    memset(out, 0, sizeof(*out));

    sqlite3 *db = hu_sqlite_memory_get_db(memory);
    if (!db)
        return HU_ERR_NOT_SUPPORTED;

    sqlite3_stmt *stmt = NULL;
    int rc = sqlite3_prepare_v2(db,
                                "SELECT uses_lowercase, uses_periods, laugh_style, "
                                "avg_message_length, common_phrases, distinctive_words "
                                "FROM style_fingerprints WHERE contact_id=?",
                                -1, &stmt, NULL);
    if (rc != SQLITE_OK)
        return HU_ERR_MEMORY_BACKEND;

    sqlite3_bind_text(stmt, 1, contact_id, (int)contact_id_len, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return HU_OK;
    }

    out->uses_lowercase = sqlite3_column_int(stmt, 0) != 0;
    out->uses_periods = sqlite3_column_int(stmt, 1) != 0;
    const char *ls = (const char *)sqlite3_column_text(stmt, 2);
    if (ls) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 2);
        if (len >= sizeof(out->laugh_style))
            len = sizeof(out->laugh_style) - 1;
        memcpy(out->laugh_style, ls, len);
        out->laugh_style[len] = '\0';
    }
    out->avg_message_length = sqlite3_column_int(stmt, 3);
    const char *cp = (const char *)sqlite3_column_text(stmt, 4);
    if (cp) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 4);
        if (len >= sizeof(out->common_phrases))
            len = sizeof(out->common_phrases) - 1;
        memcpy(out->common_phrases, cp, len);
        out->common_phrases[len] = '\0';
    }
    const char *dw = (const char *)sqlite3_column_text(stmt, 5);
    if (dw) {
        size_t len = (size_t)sqlite3_column_bytes(stmt, 5);
        if (len >= sizeof(out->distinctive_words))
            len = sizeof(out->distinctive_words) - 1;
        memcpy(out->distinctive_words, dw, len);
        out->distinctive_words[len] = '\0';
    }

    sqlite3_finalize(stmt);
    return HU_OK;
}

size_t hu_style_fingerprint_build_directive(const hu_style_fingerprint_t *fp, char *buf,
                                            size_t cap) {
    if (!fp || !buf || cap < 32)
        return 0;

    bool has_any = fp->uses_lowercase || fp->uses_periods || fp->laugh_style[0] ||
                   fp->avg_message_length > 0 || fp->distinctive_words[0];

    if (!has_any)
        return 0;

    char parts[4][128];
    size_t n = 0;
    if (fp->uses_lowercase) {
        int w = snprintf(parts[n], sizeof(parts[n]), "lowercase");
        if (w > 0 && (size_t)w < sizeof(parts[n]))
            n++;
    }
    if (fp->uses_periods) {
        int w = snprintf(parts[n], sizeof(parts[n]), "periods");
        if (w > 0 && (size_t)w < sizeof(parts[n]))
            n++;
    } else if (fp->uses_lowercase || fp->laugh_style[0]) {
        int w = snprintf(parts[n], sizeof(parts[n]), "no periods");
        if (w > 0 && (size_t)w < sizeof(parts[n]))
            n++;
    }
    if (fp->laugh_style[0]) {
        int w = snprintf(parts[n], sizeof(parts[n]), "'%s' not 'lol'", fp->laugh_style);
        if (w > 0 && (size_t)w < sizeof(parts[n]))
            n++;
    }

    size_t pos = 0;
    int w = snprintf(buf, cap, "[STYLE: Your recent style with this contact: ");
    if (w <= 0 || (size_t)w >= cap)
        return 0;
    pos = (size_t)w;

    for (size_t i = 0; i < n && pos < cap - 2; i++) {
        if (i > 0)
            buf[pos++] = ',';
        size_t plen = strlen(parts[i]);
        if (pos + plen >= cap)
            break;
        memcpy(buf + pos, parts[i], plen + 1);
        pos += plen;
    }
    if (pos < cap - 20) {
        int r = snprintf(buf + pos, cap - pos, ". Match it.]");
        if (r > 0 && pos + (size_t)r < cap)
            pos += (size_t)r;
    }
    return pos;
}

/* ── Self-tracking for drift detection ────────────────────────────── */

#define HU_SELF_CONTACT_ID     "__self__"
#define HU_SELF_CONTACT_ID_LEN 8

hu_error_t hu_style_fingerprint_update_self(hu_memory_t *memory, hu_allocator_t *alloc,
                                            const char *message, size_t message_len) {
    if (!memory || !message)
        return HU_ERR_INVALID_ARGUMENT;
    return hu_style_fingerprint_update(memory, alloc, HU_SELF_CONTACT_ID, HU_SELF_CONTACT_ID_LEN,
                                       message, message_len);
}

hu_error_t hu_style_drift_check(hu_memory_t *memory, hu_allocator_t *alloc,
                                const hu_style_fingerprint_t *baseline,
                                hu_style_drift_result_t *result) {
    if (!result)
        return HU_ERR_INVALID_ARGUMENT;
    memset(result, 0, sizeof(*result));

    if (!memory || !baseline)
        return HU_ERR_INVALID_ARGUMENT;

    /* Fetch current self fingerprint */
    hu_style_fingerprint_t self;
    hu_error_t err =
        hu_style_fingerprint_get(memory, alloc, HU_SELF_CONTACT_ID, HU_SELF_CONTACT_ID_LEN, &self);
    if (err != HU_OK)
        return err;

    /* If no self data has been tracked yet, we can't measure drift */
    bool has_self_data = self.uses_lowercase || self.uses_periods || self.laugh_style[0] ||
                         self.avg_message_length > 0;
    if (!has_self_data) {
        result->score = 0.0;
        return HU_OK;
    }

    /* Compare dimensions: each mismatch contributes to drift */
    double drift = 0.0;
    int dimensions = 0;

    /* Lowercase preference */
    if (baseline->uses_lowercase || self.uses_lowercase) {
        dimensions++;
        if (baseline->uses_lowercase != self.uses_lowercase)
            drift += 1.0;
    }

    /* Period usage */
    if (baseline->uses_periods || self.uses_periods) {
        dimensions++;
        if (baseline->uses_periods != self.uses_periods)
            drift += 1.0;
    }

    /* Laugh style */
    if (baseline->laugh_style[0] || self.laugh_style[0]) {
        dimensions++;
        if (strcmp(baseline->laugh_style, self.laugh_style) != 0)
            drift += 1.0;
    }

    /* Average message length — ratio-based comparison */
    if (baseline->avg_message_length > 0 && self.avg_message_length > 0) {
        dimensions++;
        int bl = baseline->avg_message_length;
        int sl = self.avg_message_length;
        double ratio = (bl > sl) ? (double)sl / (double)bl : (double)bl / (double)sl;
        /* ratio 1.0 = identical, 0.5 = 2x difference */
        drift += (1.0 - ratio);
    }

    if (dimensions == 0) {
        result->score = 0.0;
        return HU_OK;
    }

    result->score = drift / (double)dimensions;
    if (result->score > 1.0)
        result->score = 1.0;

    if (result->score >= HU_STYLE_DRIFT_THRESHOLD) {
        result->corrective = true;
        snprintf(result->directive, sizeof(result->directive),
                 "[DRIFT: Your voice has drifted from baseline. "
                 "Maintain your natural voice — check capitalization, "
                 "punctuation, laugh style, and message length.]");
    }

    return HU_OK;
}

#else /* !HU_ENABLE_SQLITE */

#include "human/context/style_tracker.h"
#include "human/core/allocator.h"
#include "human/core/error.h"
#include "human/memory.h"
#include <string.h>

hu_error_t hu_style_fingerprint_update(hu_memory_t *memory, hu_allocator_t *alloc,
                                       const char *contact_id, size_t contact_id_len,
                                       const char *message, size_t message_len) {
    (void)memory;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    (void)message;
    (void)message_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_style_fingerprint_get(hu_memory_t *memory, hu_allocator_t *alloc,
                                    const char *contact_id, size_t contact_id_len,
                                    hu_style_fingerprint_t *out) {
    (void)memory;
    (void)alloc;
    (void)contact_id;
    (void)contact_id_len;
    if (out)
        memset(out, 0, sizeof(*out));
    return HU_ERR_NOT_SUPPORTED;
}

size_t hu_style_fingerprint_build_directive(const hu_style_fingerprint_t *fp, char *buf,
                                            size_t cap) {
    (void)fp;
    (void)buf;
    (void)cap;
    return 0;
}

hu_error_t hu_style_fingerprint_update_self(hu_memory_t *memory, hu_allocator_t *alloc,
                                            const char *message, size_t message_len) {
    (void)memory;
    (void)alloc;
    (void)message;
    (void)message_len;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_style_drift_check(hu_memory_t *memory, hu_allocator_t *alloc,
                                const hu_style_fingerprint_t *baseline,
                                hu_style_drift_result_t *result) {
    (void)memory;
    (void)alloc;
    (void)baseline;
    if (result)
        memset(result, 0, sizeof(*result));
    return HU_ERR_NOT_SUPPORTED;
}

#endif /* HU_ENABLE_SQLITE */

/* Shared across SQLite and stub variants: guards here, storage (or the
 * NOT_SUPPORTED stub) in hu_style_fingerprint_update. */
hu_error_t hu_style_fingerprint_update_inbound(hu_memory_t *memory, hu_allocator_t *alloc,
                                               const char *contact_id, size_t contact_id_len,
                                               const char *message, size_t message_len) {
    if (!memory || !contact_id || contact_id_len == 0 || !message)
        return HU_ERR_INVALID_ARGUMENT;
    /* "__self__" is the drift-detection baseline row (update_self); an inbound
     * sender key must never overwrite it. */
    if (contact_id_len == 8 && memcmp(contact_id, "__self__", 8) == 0)
        return HU_ERR_INVALID_ARGUMENT;
    if (message_len == 0)
        return HU_OK;
    return hu_style_fingerprint_update(memory, alloc, contact_id, contact_id_len, message,
                                       message_len);
}
