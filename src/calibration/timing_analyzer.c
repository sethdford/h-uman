#include "calibration_chatdb.h"
#include "human/calibration.h"
#include "human/core/paths.h"
#include "human/core/string.h"

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

#define HU_CALIB_TIMING_MSG_LIMIT 50000
#define HU_CALIB_MAX_CONTACTS     48
#define HU_CALIB_MAX_LAT_PER_CT   400

typedef struct {
    int64_t *sec;
    size_t n;
    size_t cap;
} hu_calib_latency_vec_t;

typedef struct {
    char handle[256];
    int64_t sec[HU_CALIB_MAX_LAT_PER_CT];
    uint32_t n;
} hu_calib_contact_buf_t;

#if (!defined(HU_IS_TEST) || !HU_IS_TEST) && defined(HU_ENABLE_SQLITE)
static int hu_calib_cmp_i64(const void *a, const void *b) {
    int64_t x = *(const int64_t *)a;
    int64_t y = *(const int64_t *)b;
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    return 0;
}

static int hu_calib_cmp_i32(const void *a, const void *b) {
    int32_t x = *(const int32_t *)a;
    int32_t y = *(const int32_t *)b;
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    return 0;
}

static double hu_calib_percentile_sorted(const int64_t *sorted, size_t n, double pct) {
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

static hu_calibration_tod_bucket_t hu_calib_tod_from_hour(int hour) {
    if (hour >= 6 && hour < 12)
        return HU_CALIB_TOD_MORNING;
    if (hour >= 12 && hour < 17)
        return HU_CALIB_TOD_AFTERNOON;
    if (hour >= 17 && hour < 22)
        return HU_CALIB_TOD_EVENING;
    return HU_CALIB_TOD_NIGHT;
}

static time_t hu_calib_apple_ns_to_unix(int64_t apple_ns) {
    return (time_t)(apple_ns / 1000000000LL + 978307200LL);
}

static hu_error_t hu_calib_latency_push(hu_allocator_t *alloc, hu_calib_latency_vec_t *v,
                                        int64_t x) {
    if (v->n >= v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 128;
        size_t old_bytes = v->cap * sizeof(int64_t);
        size_t new_bytes = nc * sizeof(int64_t);
        int64_t *p = (int64_t *)alloc->realloc(alloc->ctx, v->sec, old_bytes, new_bytes);
        if (!p)
            return HU_ERR_OUT_OF_MEMORY;
        v->sec = p;
        v->cap = nc;
    }
    v->sec[v->n++] = x;
    return HU_OK;
}

static void hu_calib_latency_vec_free(hu_allocator_t *alloc, hu_calib_latency_vec_t *v) {
    if (v->sec && v->cap)
        alloc->free(alloc->ctx, v->sec, v->cap * sizeof(int64_t));
    v->sec = NULL;
    v->n = 0;
    v->cap = 0;
}

static void hu_calib_fill_percentiles(hu_calibration_latency_percentiles_t *out,
                                      hu_calib_latency_vec_t *v) {
    memset(out, 0, sizeof(*out));
    out->sample_count = v->n > UINT32_MAX ? UINT32_MAX : (uint32_t)v->n;
    if (v->n == 0)
        return;
    qsort(v->sec, v->n, sizeof(int64_t), hu_calib_cmp_i64);
    out->p25_sec = hu_calib_percentile_sorted(v->sec, v->n, 25.0);
    out->p50_sec = hu_calib_percentile_sorted(v->sec, v->n, 50.0);
    out->p75_sec = hu_calib_percentile_sorted(v->sec, v->n, 75.0);
    out->p95_sec = hu_calib_percentile_sorted(v->sec, v->n, 95.0);
}

static hu_error_t hu_calib_resolve_db_path(const char *db_path, char *out, size_t cap) {
    return hu_calibration_resolve_chatdb(db_path, out, cap);
}

static hu_calib_contact_buf_t *hu_calib_find_contact(hu_calib_contact_buf_t *cts, size_t *n_ct,
                                                     const char *handle) {
    for (size_t i = 0; i < *n_ct; i++) {
        if (strcmp(cts[i].handle, handle) == 0)
            return &cts[i];
    }
    if (*n_ct >= HU_CALIB_MAX_CONTACTS)
        return NULL;
    size_t i = *n_ct;
    memset(&cts[i], 0, sizeof(cts[i]));
    strncpy(cts[i].handle, handle, sizeof(cts[i].handle) - 1);
    (*n_ct)++;
    return &cts[i];
}
#endif /* !HU_IS_TEST */

void hu_timing_report_deinit(hu_allocator_t *alloc, hu_timing_report_t *report) {
    if (!alloc || !report)
        return;
    for (size_t i = 0; i < report->contacts_count; i++) {
        if (report->contacts[i].handle_id)
            hu_str_free(alloc, report->contacts[i].handle_id);
    }
    if (report->contacts) {
        alloc->free(alloc->ctx, report->contacts,
                    report->contacts_count * sizeof(hu_calibration_contact_latency_t));
    }
    memset(report, 0, sizeof(*report));
}

#if defined(HU_IS_TEST) && HU_IS_TEST
static void hu_calibration_timing_fill_mock(hu_timing_report_t *out) {
    memset(out, 0, sizeof(*out));
    for (int b = 0; b < HU_CALIB_TOD_BUCKET_COUNT; b++) {
        out->by_tod[b].sample_count = 40;
        out->by_tod[b].p25_sec = 30.0 + (double)b * 5.0;
        out->by_tod[b].p50_sec = 120.0 + (double)b * 10.0;
        out->by_tod[b].p75_sec = 600.0;
        out->by_tod[b].p95_sec = 3600.0;
    }
    out->active_hours[9] = 12;
    out->active_hours[14] = 20;
    out->active_hours[21] = 8;
    out->messages_per_day.sample_count = 30;
    out->messages_per_day.p25_sec = 5.0;
    out->messages_per_day.p50_sec = 18.0;
    out->messages_per_day.p75_sec = 42.0;
    out->messages_per_day.p95_sec = 90.0;
}
#endif

hu_error_t hu_calibration_analyze_timing(hu_allocator_t *alloc, const char *db_path,
                                         const char *contact_filter,
                                         hu_timing_report_t *out_report) {
    if (!alloc || !out_report)
        return HU_ERR_INVALID_ARGUMENT;
    memset(out_report, 0, sizeof(*out_report));

#if defined(HU_IS_TEST) && HU_IS_TEST
    (void)db_path;
    (void)contact_filter;
    hu_calibration_timing_fill_mock(out_report);
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

    const char *sql_all = "SELECT m.is_from_me, m.date, cmj.chat_id, IFNULL(h.id,'') "
                          "FROM message m "
                          "JOIN chat_message_join cmj ON m.ROWID = cmj.message_id "
                          "LEFT JOIN handle h ON m.handle_id = h.ROWID "
                          "ORDER BY cmj.chat_id ASC, m.date ASC LIMIT ?1";

    const char *sql_filt = "SELECT m.is_from_me, m.date, cmj.chat_id, IFNULL(h.id,'') "
                           "FROM message m "
                           "JOIN chat_message_join cmj ON m.ROWID = cmj.message_id "
                           "LEFT JOIN handle h ON m.handle_id = h.ROWID "
                           "WHERE cmj.chat_id IN ("
                           "  SELECT chj.chat_id FROM chat_handle_join chj "
                           "  JOIN handle h2 ON chj.handle_id = h2.ROWID "
                           "  WHERE h2.id = ?2"
                           ") "
                           "ORDER BY cmj.chat_id ASC, m.date ASC LIMIT ?1";

    sqlite3_stmt *stmt = NULL;
    const char *use_sql = contact_filter && contact_filter[0] ? sql_filt : sql_all;
    if (sqlite3_prepare_v2(db, use_sql, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }

    sqlite3_bind_int(stmt, 1, (int)HU_CALIB_TIMING_MSG_LIMIT);
    char contact_buf[256];
    if (contact_filter && contact_filter[0]) {
        strncpy(contact_buf, contact_filter, sizeof(contact_buf) - 1);
        contact_buf[sizeof(contact_buf) - 1] = '\0';
        sqlite3_bind_text(stmt, 2, contact_buf, -1, SQLITE_STATIC);
    }

    hu_calib_latency_vec_t by_tod[HU_CALIB_TOD_BUCKET_COUNT];
    hu_calib_latency_vec_t day_counts_vec;
    memset(by_tod, 0, sizeof(by_tod));
    memset(&day_counts_vec, 0, sizeof(day_counts_vec));

    hu_calib_contact_buf_t cts[HU_CALIB_MAX_CONTACTS];
    size_t n_ct = 0;

    memset(out_report->active_hours, 0, sizeof(out_report->active_hours));

    int64_t pending_peer_ts = -1;
    int pending_hour = 0;
    char pending_handle[256];
    pending_handle[0] = '\0';
    int64_t last_chat = -1;

    int32_t *day_keys = NULL;
    size_t day_n = 0, day_cap = 0;

    hu_error_t err = HU_OK;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        int from_me = sqlite3_column_int(stmt, 0);
        int64_t adate = sqlite3_column_int64(stmt, 1);
        int64_t chat_id = sqlite3_column_int64(stmt, 2);
        const char *hid = (const char *)sqlite3_column_text(stmt, 3);
        if (!hid)
            hid = "";

        if (last_chat >= 0 && chat_id != last_chat) {
            pending_peer_ts = -1;
            pending_handle[0] = '\0';
        }
        last_chat = chat_id;

        time_t t = hu_calib_apple_ns_to_unix(adate);
        struct tm tm_local;
        memset(&tm_local, 0, sizeof(tm_local));
        if (localtime_r(&t, &tm_local) == NULL)
            continue;
        int hour = tm_local.tm_hour;

        if (from_me) {
            if (pending_peer_ts >= 0) {
                int64_t delta = (int64_t)t - pending_peer_ts;
                if (delta >= 0 && delta < 86400 * 7) {
                    hu_calibration_tod_bucket_t b = hu_calib_tod_from_hour(pending_hour);
                    err = hu_calib_latency_push(alloc, &by_tod[b], delta);
                    if (err != HU_OK)
                        goto cleanup_loop;

                    if (pending_handle[0]) {
                        hu_calib_contact_buf_t *cb =
                            hu_calib_find_contact(cts, &n_ct, pending_handle);
                        if (cb && cb->n < HU_CALIB_MAX_LAT_PER_CT)
                            cb->sec[cb->n++] = delta;
                    }
                }
            }
            pending_peer_ts = -1;
            pending_handle[0] = '\0';

            out_report->active_hours[hour < 0 || hour > 23 ? 0 : (uint32_t)hour]++;

            int dk =
                (tm_local.tm_year + 1900) * 10000 + (tm_local.tm_mon + 1) * 100 + tm_local.tm_mday;
            if (day_n >= day_cap) {
                size_t nc = day_cap ? day_cap * 2 : 256;
                int32_t *p = (int32_t *)alloc->realloc(
                    alloc->ctx, day_keys, day_cap * sizeof(int32_t), nc * sizeof(int32_t));
                if (!p) {
                    err = HU_ERR_OUT_OF_MEMORY;
                    goto cleanup_loop;
                }
                day_keys = p;
                day_cap = nc;
            }
            day_keys[day_n++] = dk;
        } else {
            pending_peer_ts = (int64_t)t;
            pending_hour = hour;
            strncpy(pending_handle, hid, sizeof(pending_handle) - 1);
            pending_handle[sizeof(pending_handle) - 1] = '\0';
        }
    }

cleanup_loop:
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    stmt = NULL;
    db = NULL;

    if (err != HU_OK) {
        for (int b = 0; b < HU_CALIB_TOD_BUCKET_COUNT; b++)
            hu_calib_latency_vec_free(alloc, &by_tod[b]);
        hu_calib_latency_vec_free(alloc, &day_counts_vec);
        if (day_keys)
            alloc->free(alloc->ctx, day_keys, day_cap * sizeof(int32_t));
        return err;
    }

    for (int b = 0; b < HU_CALIB_TOD_BUCKET_COUNT; b++) {
        hu_calib_fill_percentiles(&out_report->by_tod[b], &by_tod[b]);
        hu_calib_latency_vec_free(alloc, &by_tod[b]);
    }

    if (day_n > 0) {
        qsort(day_keys, day_n, sizeof(int32_t), hu_calib_cmp_i32);
        for (size_t i = 0; i < day_n;) {
            int32_t k = day_keys[i];
            size_t j = i + 1;
            while (j < day_n && day_keys[j] == k)
                j++;
            int64_t cnt = (int64_t)(j - i);
            err = hu_calib_latency_push(alloc, &day_counts_vec, cnt);
            if (err != HU_OK)
                break;
            i = j;
        }
        alloc->free(alloc->ctx, day_keys, day_cap * sizeof(int32_t));
        day_keys = NULL;
    }

    if (err != HU_OK) {
        hu_calib_latency_vec_free(alloc, &day_counts_vec);
        return err;
    }

    /* messages_per_day: doubles are message counts (not seconds). */
    hu_calib_fill_percentiles(&out_report->messages_per_day, &day_counts_vec);
    hu_calib_latency_vec_free(alloc, &day_counts_vec);

    size_t out_c = 0;
    for (size_t i = 0; i < n_ct; i++) {
        if (cts[i].n > 0)
            out_c++;
    }
    if (out_c > 0) {
        out_report->contacts = (hu_calibration_contact_latency_t *)alloc->alloc(
            alloc->ctx, out_c * sizeof(hu_calibration_contact_latency_t));
        if (!out_report->contacts)
            return HU_ERR_OUT_OF_MEMORY;
        memset(out_report->contacts, 0, out_c * sizeof(hu_calibration_contact_latency_t));
        size_t w = 0;
        for (size_t i = 0; i < n_ct; i++) {
            if (cts[i].n == 0)
                continue;
            qsort(cts[i].sec, cts[i].n, sizeof(int64_t), hu_calib_cmp_i64);
            double med = hu_calib_percentile_sorted(cts[i].sec, cts[i].n, 50.0);
            out_report->contacts[w].handle_id = hu_strdup(alloc, cts[i].handle);
            if (!out_report->contacts[w].handle_id) {
                for (size_t k = 0; k < w; k++)
                    hu_str_free(alloc, out_report->contacts[k].handle_id);
                alloc->free(alloc->ctx, out_report->contacts,
                            out_c * sizeof(hu_calibration_contact_latency_t));
                out_report->contacts = NULL;
                return HU_ERR_OUT_OF_MEMORY;
            }
            out_report->contacts[w].median_reply_sec = med;
            out_report->contacts[w].sample_count = cts[i].n;
            w++;
        }
        out_report->contacts_count = w;
    }

    return HU_OK;
#endif /* HU_ENABLE_SQLITE */
#endif /* HU_IS_TEST */
}
