#include "human/calibration/clone.h"
#include "human/core/io_secure.h"
#include "human/core/json.h"
#include "human/json_util.h"

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>
#endif

#define HU_CLONE_MSG_LIMIT       50000
#define HU_CLONE_SESSION_GAP_SEC 3600
#define HU_CLONE_SLOT_TOPIC      48
#define HU_CLONE_SLOT_SIGNOFF    32

#if defined(HU_IS_TEST) && HU_IS_TEST
static void hu_behavioral_clone_fill_mock(hu_clone_patterns_t *out) {
    memset(out, 0, sizeof(*out));
    strncpy(out->topic_starters[0], "hey — quick question", sizeof(out->topic_starters[0]) - 1);
    strncpy(out->topic_starters[1], "got a sec?", sizeof(out->topic_starters[1]) - 1);
    strncpy(out->topic_starters[2], "random thought:", sizeof(out->topic_starters[2]) - 1);
    out->topic_starter_count = 3;
    out->initiation_frequency_per_day = 2.5;

    strncpy(out->sign_offs[0], "talk soon", sizeof(out->sign_offs[0]) - 1);
    strncpy(out->sign_offs[1], "lmk", sizeof(out->sign_offs[1]) - 1);
    out->sign_off_count = 2;

    for (int i = 0; i < 10; i++)
        out->response_length_by_depth[i] = 1.0 - 0.04 * (double)i;
    out->response_length_by_depth[0] = 1.0;

    out->double_text_probability = 0.18;
    out->double_text_median_delay_sec = 95;

    out->read_to_response_median_sec = 420;
    out->read_to_response_p95_sec = 2700;
    out->avg_message_length = 88.0;
    out->emoji_frequency = 0.22;
}
#endif

#if !defined(HU_IS_TEST) || !HU_IS_TEST
#if defined(HU_ENABLE_SQLITE)

typedef struct {
    char key[256];
    uint32_t count;
} hu_clone_slot256_t;

typedef struct {
    char key[128];
    uint32_t count;
} hu_clone_slot128_t;

typedef struct {
    int64_t *sec;
    size_t n;
    size_t cap;
} hu_clone_delay_vec_t;

static int hu_clone_cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    return 0;
}

static int hu_clone_cmp_i32(const void *a, const void *b) {
    int32_t x = *(const int32_t *)a;
    int32_t y = *(const int32_t *)b;
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    return 0;
}

static double hu_clone_percentile_sorted(const int64_t *sorted, size_t n, double pct) {
    if (n == 0 || !sorted)
        return 0.0;
    if (n == 1)
        return (double)sorted[0];
    double pos = (pct / 100.0) * (double)(n - 1);
    size_t lo = (size_t)pos;
    size_t hi = lo + 1;
    if (hi >= n)
        return (double)sorted[n - 1];
    double frac = pos - (double)lo;
    return (1.0 - frac) * (double)sorted[lo] + frac * (double)sorted[hi];
}

static time_t hu_clone_apple_ns_to_unix(int64_t apple_ns) {
    return (time_t)(apple_ns / 1000000000LL + 978307200LL);
}

static hu_error_t hu_clone_resolve_db_path(const char *db_path, char *out, size_t cap) {
    if (db_path && db_path[0]) {
        size_t len = strlen(db_path);
        if (len + 1 > cap)
            return HU_ERR_INVALID_ARGUMENT;
        memcpy(out, db_path, len + 1);
        return HU_OK;
    }
#if defined(__APPLE__) && defined(__MACH__)
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return HU_ERR_NOT_FOUND;
    int n = snprintf(out, cap, "%s/Library/Messages/chat.db", home);
    if (n < 0 || (size_t)n >= cap)
        return HU_ERR_INVALID_ARGUMENT;
    return HU_OK;
#else
    (void)out;
    return HU_ERR_NOT_SUPPORTED;
#endif
}

static const char *hu_clone_trim_start(const char *s, size_t len, size_t *out_len) {
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r'))
        i++;
    *out_len = len > i ? len - i : 0;
    return s + i;
}

/* True if text contains a UTF-8 sequence of 3+ bytes (rough proxy for emoji / non-ASCII). */
static bool hu_clone_text_has_emoji(const char *text) {
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        if (*p >= 0xF0) {
            if (p[1] && p[2] && p[3])
                return true;
            break;
        }
        if (*p >= 0xE0 && p[1] && p[2])
            return true;
        if (*p >= 0xC0 && p[1])
            p += 2;
        else
            p++;
    }
    return false;
}

static void hu_clone_extract_opening(const char *text, char *out, size_t cap) {
    size_t len = strlen(text);
    size_t tl;
    const char *t = hu_clone_trim_start(text, len, &tl);
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

static void hu_clone_extract_closing(const char *text, char *out, size_t cap) {
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

static void hu_clone_slot256_bump(hu_clone_slot256_t *slots, size_t nslots, const char *phrase) {
    if (!phrase || !phrase[0])
        return;
    for (size_t i = 0; i < nslots; i++) {
        if (slots[i].key[0] == '\0') {
            strncpy(slots[i].key, phrase, sizeof(slots[i].key) - 1);
            slots[i].key[sizeof(slots[i].key) - 1] = '\0';
            slots[i].count = 1;
            return;
        }
        if (strcmp(slots[i].key, phrase) == 0) {
            slots[i].count++;
            return;
        }
    }
}

static void hu_clone_slot128_bump(hu_clone_slot128_t *slots, size_t nslots, const char *phrase) {
    if (!phrase || !phrase[0])
        return;
    for (size_t i = 0; i < nslots; i++) {
        if (slots[i].key[0] == '\0') {
            strncpy(slots[i].key, phrase, sizeof(slots[i].key) - 1);
            slots[i].key[sizeof(slots[i].key) - 1] = '\0';
            slots[i].count = 1;
            return;
        }
        if (strcmp(slots[i].key, phrase) == 0) {
            slots[i].count++;
            return;
        }
    }
}

static int hu_clone_cmp_slot256_desc(const void *a, const void *b) {
    const hu_clone_slot256_t *x = (const hu_clone_slot256_t *)a;
    const hu_clone_slot256_t *y = (const hu_clone_slot256_t *)b;
    if (x->count < y->count)
        return 1;
    if (x->count > y->count)
        return -1;
    return strcmp(x->key, y->key);
}

static int hu_clone_cmp_slot128_desc(const void *a, const void *b) {
    const hu_clone_slot128_t *x = (const hu_clone_slot128_t *)a;
    const hu_clone_slot128_t *y = (const hu_clone_slot128_t *)b;
    if (x->count < y->count)
        return 1;
    if (x->count > y->count)
        return -1;
    return strcmp(x->key, y->key);
}

static hu_error_t hu_clone_delay_push(hu_allocator_t *alloc, hu_clone_delay_vec_t *v, int64_t x) {
    if (v->n >= v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 256;
        size_t ob = v->cap * sizeof(int64_t);
        size_t nb = nc * sizeof(int64_t);
        int64_t *p = (int64_t *)alloc->realloc(alloc->ctx, v->sec, ob, nb);
        if (!p)
            return HU_ERR_OUT_OF_MEMORY;
        v->sec = p;
        v->cap = nc;
    }
    v->sec[v->n++] = x;
    return HU_OK;
}

static void hu_clone_delay_vec_free(hu_allocator_t *alloc, hu_clone_delay_vec_t *v) {
    if (v->sec && v->cap)
        alloc->free(alloc->ctx, v->sec, v->cap * sizeof(int64_t));
    v->sec = NULL;
    v->n = 0;
    v->cap = 0;
}

static void hu_clone_fill_delays_median_p95(hu_clone_delay_vec_t *v, int *out_med, int *out_p95) {
    *out_med = 0;
    *out_p95 = 0;
    if (v->n == 0)
        return;
    qsort(v->sec, v->n, sizeof(int64_t), hu_clone_cmp_i64);
    double m = hu_clone_percentile_sorted(v->sec, v->n, 50.0);
    double p = hu_clone_percentile_sorted(v->sec, v->n, 95.0);
    *out_med = (int)(m + 0.5);
    *out_p95 = (int)(p + 0.5);
}

static hu_error_t hu_clone_run_sql(hu_allocator_t *alloc, sqlite3 *db, const char *sql, int lim,
                                   const char *contact_filter, int64_t since_ns,
                                   hu_clone_patterns_t *out) {
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    bool has_contact = contact_filter && contact_filter[0];
    bool has_since = since_ns > 0;
    char contact_buf[256];

    sqlite3_bind_int(stmt, 1, lim);
    if (has_contact) {
        strncpy(contact_buf, contact_filter, sizeof(contact_buf) - 1);
        contact_buf[sizeof(contact_buf) - 1] = '\0';
        sqlite3_bind_text(stmt, 2, contact_buf, -1, SQLITE_STATIC);
        if (has_since)
            sqlite3_bind_int64(stmt, 3, since_ns);
    } else if (has_since) {
        sqlite3_bind_int64(stmt, 2, since_ns);
    }

    hu_clone_slot256_t topic_slots[HU_CLONE_SLOT_TOPIC];
    hu_clone_slot128_t sign_slots[HU_CLONE_SLOT_SIGNOFF];
    memset(topic_slots, 0, sizeof(topic_slots));
    memset(sign_slots, 0, sizeof(sign_slots));

    hu_clone_delay_vec_t dbl_delays = {0};
    hu_clone_delay_vec_t read_delays = {0};

    double len_sum[10];
    size_t len_cnt[10];
    memset(len_sum, 0, sizeof(len_sum));
    memset(len_cnt, 0, sizeof(len_cnt));

    int32_t *day_mark = NULL;
    size_t day_n = 0, day_cap = 0;

    int64_t last_chat = -1;
    int64_t last_date_ns = 0;
    bool pending_peer = false;
    bool last_was_me = false;
    uint32_t my_turn_idx = 0;
    int64_t last_me_date_ns = 0;
    size_t my_outbound = 0;
    size_t double_hits = 0;
    size_t initiation_count = 0;
    int64_t peer_read_ns = 0;
    char last_out_text[4096];
    last_out_text[0] = '\0';
    char last_opening[256];
    last_opening[0] = '\0';

    double outbound_len_sum = 0.0;
    size_t outbound_text_n = 0;
    size_t outbound_emoji_n = 0;

    hu_error_t err = HU_OK;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int from_me = sqlite3_column_int(stmt, 0);
        int64_t adate = sqlite3_column_int64(stmt, 1);
        const char *text = (const char *)sqlite3_column_text(stmt, 2);
        if (!text)
            text = "";
        int64_t date_read = sqlite3_column_int64(stmt, 3);
        int64_t chat_id = sqlite3_column_int64(stmt, 4);

        if (from_me != 0 && text[0]) {
            outbound_len_sum += (double)strlen(text);
            outbound_text_n++;
            if (hu_clone_text_has_emoji(text))
                outbound_emoji_n++;
        }

        bool new_session = false;
        if (last_chat >= 0 && chat_id != last_chat)
            new_session = true;
        else if (last_date_ns > 0) {
            int64_t gap = (adate - last_date_ns) / 1000000000LL;
            if (gap > HU_CLONE_SESSION_GAP_SEC)
                new_session = true;
        }
        if (new_session) {
            pending_peer = false;
            last_was_me = false;
            my_turn_idx = 0;
            peer_read_ns = 0;
            last_opening[0] = '\0';
        }
        last_chat = chat_id;

        if (from_me) {
            {
                time_t tu = hu_clone_apple_ns_to_unix(adate);
                struct tm tm_local;
                memset(&tm_local, 0, sizeof(tm_local));
                if (localtime_r(&tu, &tm_local) != NULL) {
                    int32_t dk =
                        (tm_local.tm_year + 1900) * 10000 + (tm_local.tm_mon + 1) * 100 + tm_local.tm_mday;
                    if (day_n >= day_cap) {
                        size_t nc = day_cap ? day_cap * 2 : 64;
                        int32_t *p = (int32_t *)alloc->realloc(alloc->ctx, day_mark, day_cap * sizeof(int32_t),
                                                             nc * sizeof(int32_t));
                        if (!p) {
                            err = HU_ERR_OUT_OF_MEMORY;
                            goto clone_cleanup;
                        }
                        day_mark = p;
                        day_cap = nc;
                    }
                    day_mark[day_n++] = dk;
                }
            }

            if (pending_peer) {
                size_t d = my_turn_idx < 10 ? (size_t)my_turn_idx : 9;
                size_t tl = strlen(text);
                len_sum[d] += (double)tl;
                len_cnt[d]++;
                my_turn_idx++;
                pending_peer = false;
            } else {
                if (!last_was_me) {
                    initiation_count++;
                    char op[256];
                    memset(op, 0, sizeof(op));
                    if (text[0])
                        hu_clone_extract_opening(text, op, sizeof(op));
                    if (op[0])
                        hu_clone_slot256_bump(topic_slots, HU_CLONE_SLOT_TOPIC, op);
                } else {
                    double_hits++;
                    int64_t ds = (adate - last_me_date_ns) / 1000000000LL;
                    if (ds >= 0 && ds < 86400 * 7)
                        err = hu_clone_delay_push(alloc, &dbl_delays, ds);
                    if (err != HU_OK)
                        goto clone_cleanup;

                    char op[256];
                    memset(op, 0, sizeof(op));
                    if (text[0])
                        hu_clone_extract_opening(text, op, sizeof(op));
                    if (op[0] && last_opening[0] && strcmp(op, last_opening) != 0) {
                        /* topic pivot within a burst — reinforces topic_starters diversity */
                        hu_clone_slot256_bump(topic_slots, HU_CLONE_SLOT_TOPIC, op);
                    }
                }
            }

            if (peer_read_ns > 0 && adate > peer_read_ns) {
                int64_t rd = (adate - peer_read_ns) / 1000000000LL;
                if (rd >= 0 && rd < 86400 * 3)
                    err = hu_clone_delay_push(alloc, &read_delays, rd);
                if (err != HU_OK)
                    goto clone_cleanup;
            }
            peer_read_ns = 0;

            last_was_me = true;
            last_me_date_ns = adate;
            my_outbound++;
            strncpy(last_out_text, text, sizeof(last_out_text) - 1);
            last_out_text[sizeof(last_out_text) - 1] = '\0';
            if (text[0]) {
                hu_clone_extract_opening(text, last_opening, sizeof(last_opening));
            } else {
                last_opening[0] = '\0';
            }
        } else {
            if (last_was_me && last_out_text[0]) {
                char cl[128];
                memset(cl, 0, sizeof(cl));
                hu_clone_extract_closing(last_out_text, cl, sizeof(cl));
                if (cl[0])
                    hu_clone_slot128_bump(sign_slots, HU_CLONE_SLOT_SIGNOFF, cl);
            }
            pending_peer = true;
            last_was_me = false;
            if (date_read > 0)
                peer_read_ns = date_read;
            else
                peer_read_ns = 0;
        }

        last_date_ns = adate;
    }

clone_cleanup:
    sqlite3_finalize(stmt);

    if (err != HU_OK) {
        if (day_mark)
            alloc->free(alloc->ctx, day_mark, day_cap * sizeof(int32_t));
        hu_clone_delay_vec_free(alloc, &dbl_delays);
        hu_clone_delay_vec_free(alloc, &read_delays);
        return err;
    }

    /* Unique active days */
    size_t unique_days = 0;
    if (day_n > 0) {
        qsort(day_mark, day_n, sizeof(int32_t), hu_clone_cmp_i32);
        unique_days = 1;
        for (size_t i = 1; i < day_n; i++) {
            if (day_mark[i] != day_mark[i - 1])
                unique_days++;
        }
        alloc->free(alloc->ctx, day_mark, day_cap * sizeof(int32_t));
        day_mark = NULL;
    }

    memset(out, 0, sizeof(*out));
    if (outbound_text_n > 0) {
        out->avg_message_length = outbound_len_sum / (double)outbound_text_n;
        out->emoji_frequency = (double)outbound_emoji_n / (double)outbound_text_n;
    }
    out->initiation_frequency_per_day =
        unique_days > 0 ? (double)initiation_count / (double)unique_days : 0.0;
    out->double_text_probability =
        my_outbound > 0 ? (double)double_hits / (double)my_outbound : 0.0;

    {
        int dbl_p95_unused = 0;
        hu_clone_fill_delays_median_p95(&dbl_delays, &out->double_text_median_delay_sec,
                                        &dbl_p95_unused);
    }
    hu_clone_fill_delays_median_p95(&read_delays, &out->read_to_response_median_sec,
                                    &out->read_to_response_p95_sec);
    hu_clone_delay_vec_free(alloc, &dbl_delays);
    hu_clone_delay_vec_free(alloc, &read_delays);

    double base = 0.0;
    if (len_cnt[0] > 0)
        base = len_sum[0] / (double)len_cnt[0];
    for (int i = 0; i < 10; i++) {
        if (base > 0.0 && len_cnt[i] > 0)
            out->response_length_by_depth[i] = (len_sum[i] / (double)len_cnt[i]) / base;
        else
            out->response_length_by_depth[i] = 1.0;
    }

    hu_clone_slot256_t sorted_topic[HU_CLONE_SLOT_TOPIC];
    memcpy(sorted_topic, topic_slots, sizeof(sorted_topic));
    qsort(sorted_topic, HU_CLONE_SLOT_TOPIC, sizeof(sorted_topic[0]), hu_clone_cmp_slot256_desc);
    size_t tn = 0;
    for (size_t i = 0; i < HU_CLONE_SLOT_TOPIC && tn < 16; i++) {
        if (sorted_topic[i].key[0] == '\0' || sorted_topic[i].count < 2)
            continue;
        strncpy(out->topic_starters[tn], sorted_topic[i].key, sizeof(out->topic_starters[tn]) - 1);
        out->topic_starters[tn][sizeof(out->topic_starters[tn]) - 1] = '\0';
        tn++;
    }
    out->topic_starter_count = tn;

    hu_clone_slot128_t sorted_sign[HU_CLONE_SLOT_SIGNOFF];
    memcpy(sorted_sign, sign_slots, sizeof(sorted_sign));
    qsort(sorted_sign, HU_CLONE_SLOT_SIGNOFF, sizeof(sorted_sign[0]), hu_clone_cmp_slot128_desc);
    size_t sn = 0;
    for (size_t i = 0; i < HU_CLONE_SLOT_SIGNOFF && sn < 8; i++) {
        if (sorted_sign[i].key[0] == '\0' || sorted_sign[i].count < 2)
            continue;
        strncpy(out->sign_offs[sn], sorted_sign[i].key, sizeof(out->sign_offs[sn]) - 1);
        out->sign_offs[sn][sizeof(out->sign_offs[sn]) - 1] = '\0';
        sn++;
    }
    out->sign_off_count = sn;

    return HU_OK;
}

static hu_error_t hu_clone_analyze_db(hu_allocator_t *alloc, const char *db_path,
                                      const char *contact_filter, int64_t since_ns,
                                      hu_clone_patterns_t *out) {
    char path[512];
    hu_error_t perr = hu_clone_resolve_db_path(db_path, path, sizeof(path));
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

    const char *sql_all =
        "SELECT m.is_from_me, m.date, IFNULL(m.text,''), IFNULL(m.date_read,0), cmj.chat_id "
        "FROM message m "
        "JOIN chat_message_join cmj ON m.ROWID = cmj.message_id "
        "LEFT JOIN handle h ON m.handle_id = h.ROWID "
        "WHERE m.item_type = 0 AND m.associated_message_type = 0 ";

    const char *sql_all_delta =
        "SELECT m.is_from_me, m.date, IFNULL(m.text,''), IFNULL(m.date_read,0), cmj.chat_id "
        "FROM message m "
        "JOIN chat_message_join cmj ON m.ROWID = cmj.message_id "
        "LEFT JOIN handle h ON m.handle_id = h.ROWID "
        "WHERE m.item_type = 0 AND m.associated_message_type = 0 AND m.date > ?2 ";

    const char *tail_all = "ORDER BY cmj.chat_id ASC, m.date ASC LIMIT ?1";
    const char *tail_filt =
        "AND cmj.chat_id IN ("
        "  SELECT chj.chat_id FROM chat_handle_join chj "
        "  JOIN handle h2 ON chj.handle_id = h2.ROWID "
        "  WHERE h2.id = ?2"
        ") "
        "ORDER BY cmj.chat_id ASC, m.date ASC LIMIT ?1";

    const char *tail_filt_delta =
        "AND cmj.chat_id IN ("
        "  SELECT chj.chat_id FROM chat_handle_join chj "
        "  JOIN handle h2 ON chj.handle_id = h2.ROWID "
        "  WHERE h2.id = ?2"
        ") AND m.date > ?3 "
        "ORDER BY cmj.chat_id ASC, m.date ASC LIMIT ?1";

    char sqlbuf[2048];
    const char *use = NULL;
    if (contact_filter && contact_filter[0]) {
        if (since_ns > 0) {
            snprintf(sqlbuf, sizeof(sqlbuf), "%s%s", sql_all_delta, tail_filt_delta);
            use = sqlbuf;
        } else {
            snprintf(sqlbuf, sizeof(sqlbuf), "%s%s", sql_all, tail_filt);
            use = sqlbuf;
        }
    } else {
        if (since_ns > 0) {
            snprintf(sqlbuf, sizeof(sqlbuf), "%s%s", sql_all_delta, tail_all);
            use = sqlbuf;
        } else {
            snprintf(sqlbuf, sizeof(sqlbuf), "%s%s", sql_all, tail_all);
            use = sqlbuf;
        }
    }

    hu_error_t err = hu_clone_run_sql(alloc, db, use, (int)HU_CLONE_MSG_LIMIT, contact_filter,
                                      since_ns, out);
    sqlite3_close(db);
    return err;
}
#endif /* HU_ENABLE_SQLITE */
#endif /* !HU_IS_TEST */

hu_error_t hu_behavioral_clone_extract(hu_allocator_t *alloc, const char *db_path,
                                       const char *contact_filter, hu_clone_patterns_t *out_clone) {
    if (!alloc || !out_clone)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)db_path;
    (void)contact_filter;
    hu_behavioral_clone_fill_mock(out_clone);
    return HU_OK;
#else
#if !defined(HU_ENABLE_SQLITE)
    (void)db_path;
    (void)contact_filter;
    memset(out_clone, 0, sizeof(*out_clone));
    return HU_ERR_NOT_SUPPORTED;
#else
    return hu_clone_analyze_db(alloc, db_path, contact_filter, 0, out_clone);
#endif
#endif
}

hu_error_t hu_behavioral_clone_delta(hu_allocator_t *alloc, const char *db_path,
                                     int64_t since_timestamp, hu_clone_patterns_t *out_delta) {
    if (!alloc || !out_delta)
        return HU_ERR_INVALID_ARGUMENT;

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)db_path;
    (void)since_timestamp;
    hu_behavioral_clone_fill_mock(out_delta);
    out_delta->initiation_frequency_per_day = 0.8;
    return HU_OK;
#else
#if !defined(HU_ENABLE_SQLITE)
    (void)db_path;
    (void)since_timestamp;
    memset(out_delta, 0, sizeof(*out_delta));
    return HU_ERR_NOT_SUPPORTED;
#else
    return hu_clone_analyze_db(alloc, db_path, NULL, since_timestamp, out_delta);
#endif
#endif
}

hu_error_t hu_behavioral_clone_update_persona(hu_allocator_t *alloc,
                                              const hu_clone_patterns_t *clone_data,
                                              const char *persona_path) {
    if (!alloc || !clone_data || !persona_path || !persona_path[0])
        return HU_ERR_INVALID_ARGUMENT;

    hu_json_buf_t buf;
    hu_error_t err = hu_json_buf_init(&buf, alloc);
    if (err != HU_OK)
        return err;

    err = hu_json_buf_append_raw(&buf, "{", 1);
    if (err != HU_OK)
        goto fail;

    err = hu_json_util_append_key_value(&buf, "schema", "human.behavioral_clone.v1");
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err != HU_OK)
        goto fail;

    {
        time_t now = time(NULL);
        err = hu_json_util_append_key_int(&buf, "generated_unix", (int64_t)now);
        if (err != HU_OK)
            goto fail;
    }
    err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err != HU_OK)
        goto fail;

    err = hu_json_buf_append_raw(&buf, "\"topic_starters\":[", 18);
    if (err != HU_OK)
        goto fail;
    for (size_t i = 0; i < clone_data->topic_starter_count; i++) {
        if (i > 0) {
            err = hu_json_buf_append_raw(&buf, ",", 1);
            if (err != HU_OK)
                goto fail;
        }
        err = hu_json_util_append_string(&buf, clone_data->topic_starters[i]);
        if (err != HU_OK)
            goto fail;
    }
    err = hu_json_buf_append_raw(&buf, "],", 2);
    if (err != HU_OK)
        goto fail;

    err = hu_json_buf_append_raw(&buf, "\"sign_offs\":[", 13);
    if (err != HU_OK)
        goto fail;
    for (size_t i = 0; i < clone_data->sign_off_count; i++) {
        if (i > 0) {
            err = hu_json_buf_append_raw(&buf, ",", 1);
            if (err != HU_OK)
                goto fail;
        }
        err = hu_json_util_append_string(&buf, clone_data->sign_offs[i]);
        if (err != HU_OK)
            goto fail;
    }
    err = hu_json_buf_append_raw(&buf, "],", 2);
    if (err != HU_OK)
        goto fail;

    err = hu_json_buf_append_raw(&buf, "\"response_length_by_depth\":[", 28);
    if (err != HU_OK)
        goto fail;
    for (int i = 0; i < 10; i++) {
        if (i > 0) {
            err = hu_json_buf_append_raw(&buf, ",", 1);
            if (err != HU_OK)
                goto fail;
        }
        char nb[48];
        int nn = snprintf(nb, sizeof(nb), "%.4f", clone_data->response_length_by_depth[i]);
        if (nn < 0 || (size_t)nn >= sizeof(nb)) {
            err = HU_ERR_INTERNAL;
            goto fail;
        }
        err = hu_json_buf_append_raw(&buf, nb, (size_t)nn);
        if (err != HU_OK)
            goto fail;
    }
    err = hu_json_buf_append_raw(&buf, "],", 2);
    if (err != HU_OK)
        goto fail;

    {
        char tmp[64];
        int n = snprintf(tmp, sizeof(tmp), "%.4f", clone_data->initiation_frequency_per_day);
        if (n < 0 || (size_t)n >= sizeof(tmp)) {
            err = HU_ERR_INTERNAL;
            goto fail;
        }
        err = hu_json_buf_append_raw(&buf, "\"initiation_frequency_per_day\":", 31);
        if (err != HU_OK)
            goto fail;
        err = hu_json_buf_append_raw(&buf, tmp, (size_t)n);
        if (err != HU_OK)
            goto fail;
        err = hu_json_buf_append_raw(&buf, ",", 1);
        if (err != HU_OK)
            goto fail;
    }

    {
        char tmp[64];
        int n = snprintf(tmp, sizeof(tmp), "%.4f", clone_data->double_text_probability);
        if (n < 0 || (size_t)n >= sizeof(tmp)) {
            err = HU_ERR_INTERNAL;
            goto fail;
        }
        err = hu_json_buf_append_raw(&buf, "\"double_text_probability\":", 26);
        if (err != HU_OK)
            goto fail;
        err = hu_json_buf_append_raw(&buf, tmp, (size_t)n);
        if (err != HU_OK)
            goto fail;
        err = hu_json_buf_append_raw(&buf, ",", 1);
        if (err != HU_OK)
            goto fail;
    }

    err = hu_json_util_append_key_int(&buf, "double_text_median_delay_sec",
                                      (int64_t)clone_data->double_text_median_delay_sec);
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err != HU_OK)
        goto fail;

    err = hu_json_util_append_key_int(&buf, "read_to_response_median_sec",
                                      (int64_t)clone_data->read_to_response_median_sec);
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err != HU_OK)
        goto fail;

    err = hu_json_util_append_key_int(&buf, "read_to_response_p95_sec",
                                      (int64_t)clone_data->read_to_response_p95_sec);
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err != HU_OK)
        goto fail;

    {
        char tmp[64];
        int n = snprintf(tmp, sizeof(tmp), "%.2f", clone_data->avg_message_length);
        if (n < 0 || (size_t)n >= sizeof(tmp)) {
            err = HU_ERR_INTERNAL;
            goto fail;
        }
        err = hu_json_buf_append_raw(&buf, "\"avg_message_length\":", 21);
        if (err != HU_OK)
            goto fail;
        err = hu_json_buf_append_raw(&buf, tmp, (size_t)n);
        if (err != HU_OK)
            goto fail;
        err = hu_json_buf_append_raw(&buf, ",", 1);
        if (err != HU_OK)
            goto fail;
    }
    {
        char tmp[64];
        int n = snprintf(tmp, sizeof(tmp), "%.4f", clone_data->emoji_frequency);
        if (n < 0 || (size_t)n >= sizeof(tmp)) {
            err = HU_ERR_INTERNAL;
            goto fail;
        }
        err = hu_json_buf_append_raw(&buf, "\"emoji_frequency\":", 18);
        if (err != HU_OK)
            goto fail;
        err = hu_json_buf_append_raw(&buf, tmp, (size_t)n);
        if (err != HU_OK)
            goto fail;
        err = hu_json_buf_append_raw(&buf, ",", 1);
        if (err != HU_OK)
            goto fail;
    }
    {
        char tmp[64];
        int n = snprintf(tmp, sizeof(tmp), "%.0f",
                         (double)clone_data->read_to_response_median_sec);
        if (n < 0 || (size_t)n >= sizeof(tmp)) {
            err = HU_ERR_INTERNAL;
            goto fail;
        }
        err = hu_json_buf_append_raw(&buf, "\"avg_response_time_sec\":", 24);
        if (err != HU_OK)
            goto fail;
        err = hu_json_buf_append_raw(&buf, tmp, (size_t)n);
        if (err != HU_OK)
            goto fail;
        err = hu_json_buf_append_raw(&buf, ",", 1);
        if (err != HU_OK)
            goto fail;
    }

    err = hu_json_buf_append_raw(&buf, "\"persona_recommendations\":{", 27);
    if (err != HU_OK)
        goto fail;
    err = hu_json_util_append_key_value(&buf, "avg_length_curve",
                                        "scale_outbound_length_by_depth_multipliers");
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err != HU_OK)
        goto fail;
    err = hu_json_util_append_key_value(&buf, "conversation_initiation",
                                        "bias_proactive_opener_phrases_toward_topic_starters");
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err != HU_OK)
        goto fail;
    err = hu_json_util_append_key_value(&buf, "conversation_closure",
                                        "prefer_sign_off_phrases_before_thread_pause");
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err != HU_OK)
        goto fail;
    err = hu_json_util_append_key_value(&buf, "double_text", "model_burst_probability_and_delay");
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, ",", 1);
    if (err != HU_OK)
        goto fail;
    err = hu_json_util_append_key_value(&buf, "read_receipts",
                                        "delay_after_marked_read_before_reply_distribution");
    if (err != HU_OK)
        goto fail;
    err = hu_json_buf_append_raw(&buf, "}", 1);
    if (err != HU_OK)
        goto fail;

    err = hu_json_buf_append_raw(&buf, "}", 1);
    if (err != HU_OK)
        goto fail;

    /* Cloned persona output — non-secret persona JSON; 0644. */
    FILE *fp = NULL;
    if (hu_io_secure_open(persona_path, HU_IO_PERM_USER, "w", &fp) != HU_OK || !fp) {
        err = HU_ERR_IO;
        goto fail;
    }
    size_t wlen = buf.len;
    if (fwrite(buf.ptr, 1, wlen, fp) != wlen) {
        fclose(fp);
        err = HU_ERR_IO;
        goto fail;
    }
    if (fclose(fp) != 0) {
        err = HU_ERR_IO;
        goto fail;
    }

    hu_json_buf_free(&buf);
    return HU_OK;

fail:
    hu_json_buf_free(&buf);
    return err;
}
