#include "calibration_chatdb.h"
#include "human/calibration.h"
#include "human/core/paths.h"
#include "human/core/string.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>
#endif

#define HU_CALIB_STYLE_MSG_LIMIT 8000
#define HU_CALIB_PHRASE_SLOTS    64
#define HU_CALIB_PHRASE_OUT      8
#define HU_CALIB_WORD_CAP        6000
#define HU_CALIB_WORD_LEN        48

typedef struct {
    char key[96];
    uint32_t count;
} hu_calib_phrase_slot_t;

#if (!defined(HU_IS_TEST) || !HU_IS_TEST) && defined(HU_ENABLE_SQLITE)
static size_t hu_calib_count_emoji_utf8(const char *s) {
    size_t count = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (*p >= 0xF0) {
            count++;
            if (p[1] && p[2] && p[3])
                p += 4;
            else
                break;
        } else if (*p >= 0xE0 && p[1] && p[2]) {
            p += 3;
        } else if (*p >= 0xC0 && p[1]) {
            p += 2;
        } else {
            p++;
        }
    }
    return count;
}

static void hu_calib_phrase_bump(hu_calib_phrase_slot_t *slots, const char *phrase) {
    if (!phrase || !phrase[0])
        return;
    size_t plen = strlen(phrase);
    if (plen >= sizeof(slots[0].key))
        plen = sizeof(slots[0].key) - 1;

    for (size_t i = 0; i < HU_CALIB_PHRASE_SLOTS; i++) {
        if (slots[i].key[0] == '\0') {
            memcpy(slots[i].key, phrase, plen);
            slots[i].key[plen] = '\0';
            slots[i].count = 1;
            return;
        }
        if (strcmp(slots[i].key, phrase) == 0) {
            slots[i].count++;
            return;
        }
    }
}

static const char *hu_calib_trim_start(const char *s, size_t len, size_t *out_len) {
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        i++;
    *out_len = len > i ? len - i : 0;
    return s + i;
}

static void hu_calib_extract_opening(const char *text, char *out, size_t cap) {
    size_t len = strlen(text);
    size_t tl;
    const char *t = hu_calib_trim_start(text, len, &tl);
    size_t take = tl < 56 ? tl : 56;
    size_t i = 0;
    while (i < take && t[i] && t[i] != '\n' && t[i] != '\r')
        i++;
    if (i < 3)
        return;
    size_t cpy = i < cap - 1 ? i : cap - 1;
    memcpy(out, t, cpy);
    out[cpy] = '\0';
    for (size_t j = 0; j < cpy; j++)
        out[j] = (char)tolower((unsigned char)out[j]);
}

static void hu_calib_extract_closing(const char *text, char *out, size_t cap) {
    size_t len = strlen(text);
    if (len < 3)
        return;
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\n' ||
                       text[len - 1] == '\r'))
        len--;
    if (len < 3)
        return;
    size_t start = len > 56 ? len - 56 : 0;
    while (start < len && (text[start] == ' ' || text[start] == '\t'))
        start++;
    size_t cpy = len - start;
    if (cpy < 3)
        return;
    if (cpy >= cap)
        cpy = cap - 1;
    memcpy(out, text + start, cpy);
    out[cpy] = '\0';
    for (size_t j = 0; j < cpy; j++)
        out[j] = (char)tolower((unsigned char)out[j]);
}

static hu_error_t hu_calib_resolve_db_path(const char *db_path, char *out, size_t cap) {
    return hu_calibration_resolve_chatdb(db_path, out, cap);
}

static int hu_calib_cmp_slot_desc(const void *a, const void *b) {
    const hu_calib_phrase_slot_t *x = (const hu_calib_phrase_slot_t *)a;
    const hu_calib_phrase_slot_t *y = (const hu_calib_phrase_slot_t *)b;
    if (x->count < y->count)
        return 1;
    if (x->count > y->count)
        return -1;
    return strcmp(x->key, y->key);
}

static int hu_calib_cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}
#endif /* !HU_IS_TEST */

void hu_style_report_deinit(hu_allocator_t *alloc, hu_style_report_t *report) {
    if (!alloc || !report)
        return;
    for (size_t i = 0; i < report->opening_count; i++) {
        if (report->opening_phrases[i].phrase)
            hu_str_free(alloc, report->opening_phrases[i].phrase);
    }
    for (size_t i = 0; i < report->closing_count; i++) {
        if (report->closing_phrases[i].phrase)
            hu_str_free(alloc, report->closing_phrases[i].phrase);
    }
    if (report->opening_phrases)
        alloc->free(alloc->ctx, report->opening_phrases,
                    report->opening_count * sizeof(hu_style_phrase_stat_t));
    if (report->closing_phrases)
        alloc->free(alloc->ctx, report->closing_phrases,
                    report->closing_count * sizeof(hu_style_phrase_stat_t));
    memset(report, 0, sizeof(*report));
}

#if defined(HU_IS_TEST) && HU_IS_TEST
static void hu_calibration_style_fill_mock(hu_style_report_t *out) {
    memset(out, 0, sizeof(*out));
    out->avg_message_length = 42.0;
    out->emoji_per_message = 0.35;
    out->exclamation_per_message = 0.12;
    out->question_per_message = 0.18;
    out->vocabulary_richness = 0.62;
    out->messages_analyzed = 120;
}
#endif

hu_error_t hu_calibration_analyze_style(hu_allocator_t *alloc, const char *db_path,
                                        const char *contact_filter, hu_style_report_t *out_report) {
    if (!alloc || !out_report)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_report, 0, sizeof(*out_report));

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)db_path;
    (void)contact_filter;
    hu_calibration_style_fill_mock(out_report);
    return HU_OK;
#else

#if !defined(HU_ENABLE_SQLITE)
    (void)db_path;
    (void)contact_filter;
    return HU_ERR_NOT_SUPPORTED;
#else

    char path[512];
    hu_error_t perr = hu_calib_resolve_db_path(db_path, path, sizeof(path));
    if (perr != HU_OK)
        return perr;

    if (access(path, R_OK) != 0)
        return HU_ERR_NOT_FOUND;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }

    const char *sql_all = "SELECT text FROM message WHERE is_from_me = 1 AND text IS NOT NULL AND "
                          "text != '' ORDER BY date DESC LIMIT ?1";

    const char *sql_filt =
        "SELECT text FROM message WHERE is_from_me = 1 AND text IS NOT NULL AND text != '' "
        "AND handle_id = (SELECT ROWID FROM handle WHERE id = ?2) "
        "ORDER BY date DESC LIMIT ?1";

    sqlite3_stmt *stmt = NULL;
    const char *use_sql = contact_filter && contact_filter[0] ? sql_filt : sql_all;
    if (sqlite3_prepare_v2(db, use_sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    sqlite3_bind_int(stmt, 1, (int)HU_CALIB_STYLE_MSG_LIMIT);
    char contact_buf[256];
    if (contact_filter && contact_filter[0]) {
        strncpy(contact_buf, contact_filter, sizeof(contact_buf) - 1);
        contact_buf[sizeof(contact_buf) - 1] = '\0';
        sqlite3_bind_text(stmt, 2, contact_buf, -1, SQLITE_STATIC);
    }

    double len_sum = 0.0;
    size_t excl = 0, quest = 0, msg_n = 0;
    double emoji_sum = 0.0;

    hu_calib_phrase_slot_t open_slots[HU_CALIB_PHRASE_SLOTS];
    hu_calib_phrase_slot_t close_slots[HU_CALIB_PHRASE_SLOTS];
    memset(open_slots, 0, sizeof(open_slots));
    memset(close_slots, 0, sizeof(close_slots));

    char **words = NULL;
    size_t word_n = 0, word_cap = 0;

    char obuf[96];
    char cbuf[96];

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *text = (const char *)sqlite3_column_text(stmt, 0);
        if (!text || !text[0])
            continue;
        size_t tl = strlen(text);
        len_sum += (double)tl;
        msg_n++;
        for (size_t i = 0; i < tl; i++) {
            if (text[i] == '!')
                excl++;
            if (text[i] == '?')
                quest++;
        }
        emoji_sum += (double)hu_calib_count_emoji_utf8(text);

        memset(obuf, 0, sizeof(obuf));
        memset(cbuf, 0, sizeof(cbuf));
        hu_calib_extract_opening(text, obuf, sizeof(obuf));
        hu_calib_extract_closing(text, cbuf, sizeof(cbuf));
        if (obuf[0])
            hu_calib_phrase_bump(open_slots, obuf);
        if (cbuf[0] && strcmp(obuf, cbuf) != 0)
            hu_calib_phrase_bump(close_slots, cbuf);

        const char *p = text;
        while (*p && word_n < HU_CALIB_WORD_CAP) {
            while (*p && !isalpha((unsigned char)*p))
                p++;
            if (!*p)
                break;
            const char *w = p;
            while (*p && isalpha((unsigned char)*p))
                p++;
            size_t wl = (size_t)(p - w);
            if (wl < 2 || wl > HU_CALIB_WORD_LEN)
                continue;
            if (word_n >= word_cap) {
                size_t nc = word_cap ? word_cap * 2 : 512;
                char **nw = (char **)alloc->realloc(alloc->ctx, words, word_cap * sizeof(char *),
                                                    nc * sizeof(char *));
                if (!nw) {
                    sqlite3_finalize(stmt);
                    sqlite3_close(db);
                    for (size_t i = 0; i < word_n; i++)
                        hu_str_free(alloc, words[i]);
                    if (words)
                        alloc->free(alloc->ctx, words, word_cap * sizeof(char *));
                    return HU_ERR_OUT_OF_MEMORY;
                }
                words = nw;
                word_cap = nc;
            }
            char *tok = (char *)alloc->alloc(alloc->ctx, wl + 1);
            if (!tok) {
                sqlite3_finalize(stmt);
                sqlite3_close(db);
                for (size_t i = 0; i < word_n; i++)
                    hu_str_free(alloc, words[i]);
                if (words)
                    alloc->free(alloc->ctx, words, word_cap * sizeof(char *));
                return HU_ERR_OUT_OF_MEMORY;
            }
            for (size_t i = 0; i < wl; i++)
                tok[i] = (char)tolower((unsigned char)w[i]);
            tok[wl] = '\0';
            words[word_n++] = tok;
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    if (msg_n == 0) {
        if (words) {
            for (size_t i = 0; i < word_n; i++)
                hu_str_free(alloc, words[i]);
            alloc->free(alloc->ctx, words, word_cap * sizeof(char *));
        }
        return HU_OK;
    }

    out_report->avg_message_length = len_sum / (double)msg_n;
    out_report->emoji_per_message = emoji_sum / (double)msg_n;
    out_report->exclamation_per_message = (double)excl / (double)msg_n;
    out_report->question_per_message = (double)quest / (double)msg_n;
    out_report->messages_analyzed = msg_n > UINT32_MAX ? UINT32_MAX : (uint32_t)msg_n;

    if (word_n > 0) {
        qsort(words, word_n, sizeof(char *), hu_calib_cmp_str);
        size_t unique = 0;
        for (size_t i = 0; i < word_n; i++) {
            if (i == 0 || strcmp(words[i], words[i - 1]) != 0)
                unique++;
        }
        out_report->vocabulary_richness = (double)unique / (double)word_n;
        for (size_t i = 0; i < word_n; i++)
            hu_str_free(alloc, words[i]);
        alloc->free(alloc->ctx, words, word_cap * sizeof(char *));
        words = NULL;
    }

    hu_calib_phrase_slot_t sorted_open[HU_CALIB_PHRASE_SLOTS];
    hu_calib_phrase_slot_t sorted_close[HU_CALIB_PHRASE_SLOTS];
    memcpy(sorted_open, open_slots, sizeof(sorted_open));
    memcpy(sorted_close, close_slots, sizeof(sorted_close));
    qsort(sorted_open, HU_CALIB_PHRASE_SLOTS, sizeof(sorted_open[0]), hu_calib_cmp_slot_desc);
    qsort(sorted_close, HU_CALIB_PHRASE_SLOTS, sizeof(sorted_close[0]), hu_calib_cmp_slot_desc);

    size_t on = 0, cn = 0;
    for (size_t i = 0; i < HU_CALIB_PHRASE_SLOTS && on < HU_CALIB_PHRASE_OUT; i++) {
        if (sorted_open[i].key[0] == '\0' || sorted_open[i].count < 2)
            continue;
        on++;
    }
    for (size_t i = 0; i < HU_CALIB_PHRASE_SLOTS && cn < HU_CALIB_PHRASE_OUT; i++) {
        if (sorted_close[i].key[0] == '\0' || sorted_close[i].count < 2)
            continue;
        cn++;
    }

    if (on > 0) {
        out_report->opening_phrases =
            (hu_style_phrase_stat_t *)alloc->alloc(alloc->ctx, on * sizeof(hu_style_phrase_stat_t));
        if (!out_report->opening_phrases)
            return HU_ERR_OUT_OF_MEMORY;
        memset(out_report->opening_phrases, 0, on * sizeof(hu_style_phrase_stat_t));
        size_t w = 0;
        for (size_t i = 0; i < HU_CALIB_PHRASE_SLOTS && w < on; i++) {
            if (sorted_open[i].key[0] == '\0' || sorted_open[i].count < 2)
                continue;
            out_report->opening_phrases[w].phrase = hu_strdup(alloc, sorted_open[i].key);
            if (!out_report->opening_phrases[w].phrase) {
                hu_style_report_deinit(alloc, out_report);
                return HU_ERR_OUT_OF_MEMORY;
            }
            out_report->opening_phrases[w].count = sorted_open[i].count;
            w++;
        }
        out_report->opening_count = w;
    }

    if (cn > 0) {
        out_report->closing_phrases =
            (hu_style_phrase_stat_t *)alloc->alloc(alloc->ctx, cn * sizeof(hu_style_phrase_stat_t));
        if (!out_report->closing_phrases) {
            hu_style_report_deinit(alloc, out_report);
            return HU_ERR_OUT_OF_MEMORY;
        }
        memset(out_report->closing_phrases, 0, cn * sizeof(hu_style_phrase_stat_t));
        size_t w = 0;
        for (size_t i = 0; i < HU_CALIB_PHRASE_SLOTS && w < cn; i++) {
            if (sorted_close[i].key[0] == '\0' || sorted_close[i].count < 2)
                continue;
            out_report->closing_phrases[w].phrase = hu_strdup(alloc, sorted_close[i].key);
            if (!out_report->closing_phrases[w].phrase) {
                hu_style_report_deinit(alloc, out_report);
                return HU_ERR_OUT_OF_MEMORY;
            }
            out_report->closing_phrases[w].count = sorted_close[i].count;
            w++;
        }
        out_report->closing_count = w;
    }

    return HU_OK;
#endif /* HU_ENABLE_SQLITE */
#endif /* HU_IS_TEST */
}
