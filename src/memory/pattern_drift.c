/* src/memory/pattern_drift.c
 *
 * Pattern-drift compute layer. See header for the contract.
 *
 * Layout mirrors src/channels/imessage_gaps.c:
 *   - Pure helpers compile unconditionally.
 *   - SQL scanner gated by !HU_IS_TEST && Apple && SQLITE.
 *
 * Conservative bias is encoded in:
 *   - hu_drift_compute_zscore returns 0 for flat baselines.
 *   - hu_drift_compute_dimension returns HU_DRIFT_NONE for too-few obs.
 *   - The SQL scanner skips computation when total_messages < 50 OR
 *     recent_n < 5 OR baseline_n < 30.
 *   - Only PRONOUNCED severity alerts are emitted by the SQL scanner. */

#include "human/memory/pattern_drift.h"

#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !HU_IS_TEST && defined(__APPLE__) && defined(__MACH__) && defined(HU_ENABLE_SQLITE)
#include <sqlite3.h>
#endif

/* Defaults — see header docs. */
#define HU_DRIFT_RECENT_WINDOW_DAYS   30
#define HU_DRIFT_BASELINE_WINDOW_DAYS 180
#define HU_DRIFT_MIN_RECENT_N         5
#define HU_DRIFT_MIN_BASELINE_N       30
#define HU_DRIFT_MIN_TOTAL_MESSAGES   50
#define HU_DRIFT_PRONOUNCED_SIGMA     2.0
#define HU_DRIFT_NOTICEABLE_SIGMA     1.0
#define HU_DRIFT_MIN_STDDEV_FRACTION  0.1 /* baseline_stddev floor */

#define HU_DRIFT_SECONDS_PER_DAY  86400LL
#define HU_DRIFT_SECONDS_PER_WEEK (7LL * 86400LL)

/* ---------- Pure helpers (always compiled) ---------- */

hu_drift_severity_t hu_drift_classify_severity(double sigma) {
    if (!isfinite(sigma))
        return HU_DRIFT_NONE;
    double a = fabs(sigma);
    if (a <= 0.0)
        return HU_DRIFT_NONE;
    if (a < HU_DRIFT_NOTICEABLE_SIGMA)
        return HU_DRIFT_NORMAL;
    if (a < HU_DRIFT_PRONOUNCED_SIGMA)
        return HU_DRIFT_NOTICEABLE;
    return HU_DRIFT_PRONOUNCED;
}

double hu_drift_compute_zscore(double recent, double baseline, double baseline_stddev,
                               double min_stddev) {
    if (!isfinite(recent) || !isfinite(baseline) || !isfinite(baseline_stddev) ||
        !isfinite(min_stddev))
        return 0.0;
    if (baseline_stddev < min_stddev)
        return 0.0;
    /* min_stddev should always be > 0 for a real call, but defend anyway. */
    if (baseline_stddev <= 0.0)
        return 0.0;
    return (recent - baseline) / baseline_stddev;
}

static double pd_mean(const double *xs, size_t n) {
    if (!xs || n == 0)
        return 0.0;
    double s = 0.0;
    for (size_t i = 0; i < n; i++)
        s += xs[i];
    return s / (double)n;
}

/* Sample stddev (Bessel-corrected: divisor n-1). Returns 0 for n<2. */
static double pd_stddev(const double *xs, size_t n, double mean) {
    if (!xs || n < 2)
        return 0.0;
    double s = 0.0;
    for (size_t i = 0; i < n; i++) {
        double d = xs[i] - mean;
        s += d * d;
    }
    /* sample stddev */
    return sqrt(s / (double)(n - 1));
}

hu_error_t hu_drift_compute_dimension(const double *recent, size_t recent_n, size_t min_recent_n,
                                      const double *baseline, size_t baseline_n,
                                      size_t min_baseline_n, double *out_sigma,
                                      double *out_recent_mean, double *out_baseline_mean,
                                      hu_drift_severity_t *out_severity) {
    if (out_sigma)
        *out_sigma = 0.0;
    if (out_recent_mean)
        *out_recent_mean = 0.0;
    if (out_baseline_mean)
        *out_baseline_mean = 0.0;
    if (out_severity)
        *out_severity = HU_DRIFT_NONE;

    if ((recent_n > 0 && !recent) || (baseline_n > 0 && !baseline))
        return HU_ERR_INVALID_ARGUMENT;

    /* Insufficient data → NONE. Not an error; an absence of signal. */
    if (recent_n < min_recent_n || baseline_n < min_baseline_n)
        return HU_OK;

    double rmean = pd_mean(recent, recent_n);
    double bmean = pd_mean(baseline, baseline_n);
    double bstd = pd_stddev(baseline, baseline_n, bmean);

    /* Conservative floor: refuse to classify when baseline variance is
     * below 10% of |baseline_mean| (or below 1e-9 absolute for the
     * degenerate baseline-mean-zero case). */
    double floor = fabs(bmean) * HU_DRIFT_MIN_STDDEV_FRACTION;
    if (floor < 1e-9)
        floor = 1e-9;

    double sigma = hu_drift_compute_zscore(rmean, bmean, bstd, floor);

    if (out_sigma)
        *out_sigma = sigma;
    if (out_recent_mean)
        *out_recent_mean = rmean;
    if (out_baseline_mean)
        *out_baseline_mean = bmean;
    if (out_severity)
        *out_severity = hu_drift_classify_severity(sigma);
    return HU_OK;
}

/* ---------- SQL scanner (test/non-Apple/no-SQLite stub) ---------- */

#if HU_IS_TEST || !defined(__APPLE__) || !defined(__MACH__) || !defined(HU_ENABLE_SQLITE)

hu_error_t hu_drift_compute_for_contact(const char *db_path, const char *contact_handle,
                                        int64_t now_unix, hu_drift_alert_t *out, size_t cap,
                                        size_t *out_n) {
    (void)db_path;
    (void)contact_handle;
    (void)now_unix;
    (void)out;
    (void)cap;
    if (out_n)
        *out_n = 0;
    return HU_ERR_NOT_SUPPORTED;
}

hu_error_t hu_drift_scan_top_contacts(const char *db_path, int64_t now_unix, size_t top_n,
                                      hu_drift_alert_t *out, size_t cap, size_t *out_n) {
    (void)db_path;
    (void)now_unix;
    (void)top_n;
    (void)out;
    (void)cap;
    if (out_n)
        *out_n = 0;
    return HU_ERR_NOT_SUPPORTED;
}

#else /* real implementation */

#define HU_MAC_EPOCH_OFFSET 978307200LL

/* Per-message observation extracted from chat.db. Latency is computed
 * pairwise (incoming message N's latency = N.date - previous_outgoing.date)
 * during the scan loop. */
typedef struct {
    int64_t unix_ts;
    int32_t text_len; /* characters (NULL/empty → 0; we filter those out) */
    bool is_from_me;
    double latency_sec; /* response latency for incoming msgs, -1 if N/A */
} pd_msg_t;

/* Fill `out_obs` arrays from a message list. Splits by recent vs baseline
 * windows. Computes per-dimension observation values:
 *   length:    text length of contact's messages (is_from_me=0)
 *   latency:   gap from your last outbound to contact's reply (is_from_me=0)
 *   frequency: not per-message; computed as count / weeks
 *   initiation: not per-message; computed as ratio at aggregation time */
static void pd_split_window(const pd_msg_t *msgs, size_t n, int64_t now_unix, int32_t recent_days,
                            int32_t baseline_days, double *recent_len, size_t *recent_len_n,
                            double *baseline_len, size_t *baseline_len_n, double *recent_lat,
                            size_t *recent_lat_n, double *baseline_lat, size_t *baseline_lat_n,
                            size_t cap) {
    int64_t recent_cutoff = now_unix - (int64_t)recent_days * HU_DRIFT_SECONDS_PER_DAY;
    int64_t baseline_start =
        now_unix - (int64_t)(recent_days + baseline_days) * HU_DRIFT_SECONDS_PER_DAY;

    *recent_len_n = *baseline_len_n = *recent_lat_n = *baseline_lat_n = 0;
    for (size_t i = 0; i < n; i++) {
        const pd_msg_t *m = &msgs[i];
        if (m->is_from_me)
            continue; /* dimensions tracked are about the CONTACT */
        if (m->text_len <= 0)
            continue;
        if (m->unix_ts >= recent_cutoff) {
            if (*recent_len_n < cap)
                recent_len[(*recent_len_n)++] = (double)m->text_len;
            if (m->latency_sec >= 0 && *recent_lat_n < cap)
                recent_lat[(*recent_lat_n)++] = m->latency_sec;
        } else if (m->unix_ts >= baseline_start) {
            if (*baseline_len_n < cap)
                baseline_len[(*baseline_len_n)++] = (double)m->text_len;
            if (m->latency_sec >= 0 && *baseline_lat_n < cap)
                baseline_lat[(*baseline_lat_n)++] = m->latency_sec;
        }
    }
}

/* Per-week frequency: count of contact messages, divided by window weeks. */
static void pd_window_frequency(const pd_msg_t *msgs, size_t n, int64_t now_unix,
                                int32_t recent_days, int32_t baseline_days, double *out_recent,
                                double *out_baseline) {
    int64_t recent_cutoff = now_unix - (int64_t)recent_days * HU_DRIFT_SECONDS_PER_DAY;
    int64_t baseline_start =
        now_unix - (int64_t)(recent_days + baseline_days) * HU_DRIFT_SECONDS_PER_DAY;
    size_t rc = 0, bc = 0;
    for (size_t i = 0; i < n; i++) {
        const pd_msg_t *m = &msgs[i];
        if (m->is_from_me)
            continue;
        if (m->unix_ts >= recent_cutoff)
            rc++;
        else if (m->unix_ts >= baseline_start)
            bc++;
    }
    *out_recent = (double)rc / ((double)recent_days / 7.0);
    *out_baseline = (double)bc / ((double)baseline_days / 7.0);
}

/* Load up to `cap` messages between a contact and self, sorted ASC by date.
 * Computes pairwise latency in-place. Returns 0 on success, HU_ERR_IO on
 * sqlite error. */
static hu_error_t pd_load_messages(sqlite3 *db, const char *handle, int64_t window_start_unix,
                                   pd_msg_t *out, size_t cap, size_t *out_n) {
    *out_n = 0;
    const char *sql = "SELECT m.date, m.is_from_me, LENGTH(COALESCE(m.text,'')) "
                      "FROM message m "
                      "JOIN handle h ON h.ROWID = m.handle_id "
                      "WHERE h.id = ? "
                      "  AND m.associated_message_type = 0 "
                      "  AND m.date >= ? "
                      "ORDER BY m.date ASC "
                      "LIMIT ?";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;

    int64_t mac_start = (window_start_unix - HU_MAC_EPOCH_OFFSET) * 1000000000LL;
    sqlite3_bind_text(st, 1, handle, -1, SQLITE_STATIC);
    sqlite3_bind_int64(st, 2, mac_start);
    sqlite3_bind_int(st, 3, (int)cap);

    int64_t last_outbound_unix = -1;
    while (sqlite3_step(st) == SQLITE_ROW && *out_n < cap) {
        int64_t mac_ns = sqlite3_column_int64(st, 0);
        int is_from_me = sqlite3_column_int(st, 1);
        int text_len = sqlite3_column_int(st, 2);
        int64_t unix_ts = (mac_ns / 1000000000) + HU_MAC_EPOCH_OFFSET;
        pd_msg_t *row = &out[(*out_n)++];
        row->unix_ts = unix_ts;
        row->is_from_me = (is_from_me != 0);
        row->text_len = text_len;
        row->latency_sec = -1.0;
        if (row->is_from_me) {
            last_outbound_unix = unix_ts;
        } else if (last_outbound_unix > 0) {
            double dt = (double)(unix_ts - last_outbound_unix);
            if (dt >= 0)
                row->latency_sec = dt;
        }
    }
    sqlite3_finalize(st);
    return HU_OK;
}

/* For one contact: total count first (cheap), then load if >= 50. */
static hu_error_t pd_contact_total(sqlite3 *db, const char *handle, int *out_total) {
    *out_total = 0;
    const char *sql = "SELECT COUNT(*) FROM message m "
                      "JOIN handle h ON h.ROWID = m.handle_id "
                      "WHERE h.id = ? AND m.associated_message_type = 0";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK)
        return HU_ERR_IO;
    sqlite3_bind_text(st, 1, handle, -1, SQLITE_STATIC);
    if (sqlite3_step(st) == SQLITE_ROW)
        *out_total = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return HU_OK;
}

#define PD_MAX_OBS 4096

static void pd_emit_alert_if_pronounced(hu_drift_alert_t *out, size_t cap, size_t *out_n,
                                        const char *handle, hu_drift_dimension_t dim, double sigma,
                                        double recent, double baseline, int64_t last_unix) {
    if (*out_n >= cap)
        return;
    hu_drift_severity_t sev = hu_drift_classify_severity(sigma);
    if (sev != HU_DRIFT_PRONOUNCED)
        return;
    hu_drift_alert_t *row = &out[*out_n];
    memset(row, 0, sizeof(*row));
    strncpy(row->contact_handle, handle, HU_DRIFT_HANDLE_MAX - 1);
    row->contact_handle[HU_DRIFT_HANDLE_MAX - 1] = '\0';
    row->dimension = dim;
    row->severity = sev;
    row->sigma = sigma;
    row->recent_value = recent;
    row->baseline_value = baseline;
    row->recent_window_days = HU_DRIFT_RECENT_WINDOW_DAYS;
    row->baseline_window_days = HU_DRIFT_BASELINE_WINDOW_DAYS;
    row->last_observed_unix = last_unix;
    (*out_n)++;
}

/* Internal: compute and emit alerts for one already-opened db + handle. */
static hu_error_t pd_compute_for_handle(sqlite3 *db, const char *handle, int64_t now_unix,
                                        hu_drift_alert_t *out, size_t cap, size_t *out_n) {
    int total = 0;
    hu_error_t err = pd_contact_total(db, handle, &total);
    if (err != HU_OK)
        return err;
    if (total < HU_DRIFT_MIN_TOTAL_MESSAGES)
        return HU_OK; /* insufficient history → silently skip */

    pd_msg_t *msgs = (pd_msg_t *)calloc(PD_MAX_OBS, sizeof(pd_msg_t));
    if (!msgs)
        return HU_ERR_IO;
    double *r_len = (double *)calloc(PD_MAX_OBS, sizeof(double));
    double *b_len = (double *)calloc(PD_MAX_OBS, sizeof(double));
    double *r_lat = (double *)calloc(PD_MAX_OBS, sizeof(double));
    double *b_lat = (double *)calloc(PD_MAX_OBS, sizeof(double));
    if (!r_len || !b_len || !r_lat || !b_lat) {
        free(msgs);
        free(r_len);
        free(b_len);
        free(r_lat);
        free(b_lat);
        return HU_ERR_IO;
    }

    int64_t baseline_start =
        now_unix - (int64_t)(HU_DRIFT_RECENT_WINDOW_DAYS + HU_DRIFT_BASELINE_WINDOW_DAYS) *
                       HU_DRIFT_SECONDS_PER_DAY;

    size_t n_msgs = 0;
    err = pd_load_messages(db, handle, baseline_start, msgs, PD_MAX_OBS, &n_msgs);
    if (err != HU_OK) {
        free(msgs);
        free(r_len);
        free(b_len);
        free(r_lat);
        free(b_lat);
        return err;
    }

    /* Last observed (most recent unix in window). */
    int64_t last_unix = 0;
    for (size_t i = 0; i < n_msgs; i++)
        if (msgs[i].unix_ts > last_unix)
            last_unix = msgs[i].unix_ts;

    size_t rln = 0, bln = 0, rlat = 0, blat = 0;
    pd_split_window(msgs, n_msgs, now_unix, HU_DRIFT_RECENT_WINDOW_DAYS,
                    HU_DRIFT_BASELINE_WINDOW_DAYS, r_len, &rln, b_len, &bln, r_lat, &rlat, b_lat,
                    &blat, PD_MAX_OBS);

    double sigma = 0, rmean = 0, bmean = 0;
    hu_drift_severity_t sev = HU_DRIFT_NONE;

    /* Length */
    hu_drift_compute_dimension(r_len, rln, HU_DRIFT_MIN_RECENT_N, b_len, bln,
                               HU_DRIFT_MIN_BASELINE_N, &sigma, &rmean, &bmean, &sev);
    pd_emit_alert_if_pronounced(out, cap, out_n, handle, HU_DRIFT_DIM_MESSAGE_LENGTH, sigma, rmean,
                                bmean, last_unix);

    /* Latency */
    hu_drift_compute_dimension(r_lat, rlat, HU_DRIFT_MIN_RECENT_N, b_lat, blat,
                               HU_DRIFT_MIN_BASELINE_N, &sigma, &rmean, &bmean, &sev);
    pd_emit_alert_if_pronounced(out, cap, out_n, handle, HU_DRIFT_DIM_RESPONSE_LATENCY, sigma,
                                rmean, bmean, last_unix);

    /* Frequency: derive single values (recent per-week, baseline per-week)
     * and treat as a 1-vs-1 z-score using baseline stddev derived from
     * weekly bucket counts. Simpler approach: bucket by week, then run
     * compute_dimension over the per-week counts. */
    {
        /* Bucket per-week counts across baseline window. */
        size_t weeks_b = HU_DRIFT_BASELINE_WINDOW_DAYS / 7;
        size_t weeks_r = HU_DRIFT_RECENT_WINDOW_DAYS / 7;
        if (weeks_b < 1)
            weeks_b = 1;
        if (weeks_r < 1)
            weeks_r = 1;
        double *bw = (double *)calloc(weeks_b, sizeof(double));
        double *rw = (double *)calloc(weeks_r, sizeof(double));
        if (bw && rw) {
            int64_t recent_cutoff =
                now_unix - (int64_t)HU_DRIFT_RECENT_WINDOW_DAYS * HU_DRIFT_SECONDS_PER_DAY;
            for (size_t i = 0; i < n_msgs; i++) {
                const pd_msg_t *m = &msgs[i];
                if (m->is_from_me)
                    continue;
                if (m->unix_ts >= recent_cutoff) {
                    int64_t age = (now_unix - m->unix_ts) / HU_DRIFT_SECONDS_PER_WEEK;
                    if (age < 0)
                        age = 0;
                    if ((size_t)age < weeks_r)
                        rw[age] += 1.0;
                } else {
                    int64_t age =
                        (now_unix - HU_DRIFT_RECENT_WINDOW_DAYS * HU_DRIFT_SECONDS_PER_DAY -
                         m->unix_ts) /
                        HU_DRIFT_SECONDS_PER_WEEK;
                    if (age >= 0 && (size_t)age < weeks_b)
                        bw[age] += 1.0;
                }
            }
            /* Frequency dimension needs min_recent_n = 1 (we always have N
             * weekly buckets in the window) and min_baseline_n = 4 weeks of
             * data to be a real baseline. */
            hu_drift_compute_dimension(rw, weeks_r, 1, bw, weeks_b, 4, &sigma, &rmean, &bmean,
                                       &sev);
            pd_emit_alert_if_pronounced(out, cap, out_n, handle, HU_DRIFT_DIM_MESSAGE_FREQUENCY,
                                        sigma, rmean, bmean, last_unix);
        }
        free(bw);
        free(rw);
    }

    /* Initiation ratio is tracked but not emitted unless we have a
     * reliable conversation-segmentation primitive — skip for now to
     * keep false-positive rate low. The dimension is reserved in the
     * enum so callers can extend later without breaking ABI. */
    (void)pd_window_frequency;

    free(msgs);
    free(r_len);
    free(b_len);
    free(r_lat);
    free(b_lat);
    return HU_OK;
}

hu_error_t hu_drift_compute_for_contact(const char *db_path, const char *contact_handle,
                                        int64_t now_unix, hu_drift_alert_t *out, size_t cap,
                                        size_t *out_n) {
    if (!db_path || !contact_handle || !out || !out_n)
        return HU_ERR_INVALID_ARGUMENT;
    *out_n = 0;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }
    hu_error_t err = pd_compute_for_handle(db, contact_handle, now_unix, out, cap, out_n);
    sqlite3_close(db);
    return err;
}

hu_error_t hu_drift_scan_top_contacts(const char *db_path, int64_t now_unix, size_t top_n,
                                      hu_drift_alert_t *out, size_t cap, size_t *out_n) {
    if (!db_path || !out || !out_n)
        return HU_ERR_INVALID_ARGUMENT;
    *out_n = 0;

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db)
            sqlite3_close(db);
        return HU_ERR_IO;
    }

    /* Pick top-N most-active contacts. Use the same "is_from_me=0" filter
     * as imessage_gaps to count what the contact sent. */
    const char *sql = "SELECT h.id, COUNT(*) AS c FROM message m "
                      "JOIN handle h ON h.ROWID = m.handle_id "
                      "WHERE m.is_from_me = 0 AND m.associated_message_type = 0 "
                      "GROUP BY h.id "
                      "HAVING COUNT(*) >= ? "
                      "ORDER BY c DESC LIMIT ?";
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return HU_ERR_IO;
    }
    sqlite3_bind_int(st, 1, HU_DRIFT_MIN_TOTAL_MESSAGES);
    sqlite3_bind_int(st, 2, (int)top_n);

    /* Buffer handles first so we can release the statement before nested
     * queries (avoids re-entrant prepare on the same connection). */
    char handles[64][HU_DRIFT_HANDLE_MAX];
    size_t hn = 0;
    while (sqlite3_step(st) == SQLITE_ROW && hn < 64 && hn < top_n) {
        const unsigned char *h = sqlite3_column_text(st, 0);
        if (!h)
            continue;
        strncpy(handles[hn], (const char *)h, HU_DRIFT_HANDLE_MAX - 1);
        handles[hn][HU_DRIFT_HANDLE_MAX - 1] = '\0';
        hn++;
    }
    sqlite3_finalize(st);

    for (size_t i = 0; i < hn && *out_n < cap; i++) {
        (void)pd_compute_for_handle(db, handles[i], now_unix, out, cap, out_n);
    }
    sqlite3_close(db);
    return HU_OK;
}

#endif /* SQL gate */
