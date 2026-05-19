/* include/human/memory/pattern_drift.h
 *
 * Pattern-drift detection: surfaces "Alice has been replying shorter and
 * slower than usual for 3 weeks" by comparing a contact's recent message
 * pattern (30-day window) to their long-term baseline (180 days preceding
 * the recent window).
 *
 * Design constraints (see worktree prompt):
 *   - Conservative defaults; false-positives = anxiety-inducing alerts.
 *   - Pure-helper + SQL-scanner split, mirrors imessage_gaps.h.
 *   - SQL scanner gated by !HU_IS_TEST (returns HU_ERR_NOT_SUPPORTED in
 *     test builds + non-Apple/no-SQLite builds — same stub contract as
 *     hu_imessage_scan_stale_contacts).
 *   - Drift fires in BOTH directions (shorter AND longer; faster AND
 *     slower). The signal is anomaly, not a specific cause.
 *
 * This is a COMPUTE layer. No notifications, no proactive integration,
 * no interpretation of cause. Surfacing UX is a Tier-3 follow-up.
 */

#ifndef HU_MEMORY_PATTERN_DRIFT_H
#define HU_MEMORY_PATTERN_DRIFT_H

#include "human/core/error.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HU_DRIFT_HANDLE_MAX 128

/* Severity classification — see hu_drift_classify_severity. */
typedef enum {
    HU_DRIFT_NONE = 0,   /* no signal — below floor or insufficient data */
    HU_DRIFT_NORMAL,     /* within 1 sigma — not flagged */
    HU_DRIFT_NOTICEABLE, /* 1-2 sigma — soft signal */
    HU_DRIFT_PRONOUNCED, /* > 2 sigma — flag for proactive review */
} hu_drift_severity_t;

/* Drift dimensions. Each dimension is computed independently — a contact
 * may drift on length without drifting on latency, etc. */
typedef enum {
    HU_DRIFT_DIM_MESSAGE_LENGTH = 0, /* avg message length, characters */
    HU_DRIFT_DIM_RESPONSE_LATENCY,   /* median response latency, seconds */
    HU_DRIFT_DIM_MESSAGE_FREQUENCY,  /* messages per week */
    HU_DRIFT_DIM_INITIATION_RATIO,   /* fraction of conversations the
                                      * contact (not you) started */
    HU_DRIFT_DIM_COUNT
} hu_drift_dimension_t;

typedef struct {
    char contact_handle[HU_DRIFT_HANDLE_MAX];
    hu_drift_dimension_t dimension;
    hu_drift_severity_t severity;
    double sigma; /* (recent - baseline) / baseline_stddev,
                   * signed. Negative = drift toward less /
                   * slower / lower; positive = more / faster
                   * / higher. */
    double recent_value;
    double baseline_value;
    int32_t recent_window_days;   /* days of "recent" data — typically 30 */
    int32_t baseline_window_days; /* days of "baseline" data — typically 180 */
    int64_t last_observed_unix;
} hu_drift_alert_t;

/* PURE: classify severity from |sigma|.
 *
 *   |sigma| <= 0           → HU_DRIFT_NONE       (sentinel — no data / flat)
 *   0 <  |sigma| <  1      → HU_DRIFT_NORMAL
 *   1 <= |sigma| <  2      → HU_DRIFT_NOTICEABLE
 *   |sigma| >= 2           → HU_DRIFT_PRONOUNCED
 *
 * Sign-agnostic — drift in either direction is the same severity.
 */
hu_drift_severity_t hu_drift_classify_severity(double sigma);

/* PURE: compute z-score (recent - baseline) / stddev.
 *
 * Returns 0 when:
 *   - baseline_stddev < min_stddev (flat baseline → divisor unstable; the
 *     0 return is a sentinel for "insufficient variance, don't classify")
 *   - any input is NaN / non-finite
 *
 * min_stddev is the conservative floor — pass 0.1 * baseline_value to
 * refuse z-score computation when baseline variance is below 10% of the
 * baseline mean (the default convention). */
double hu_drift_compute_zscore(double recent, double baseline, double baseline_stddev,
                               double min_stddev);

/* PURE: compute drift for a single dimension given recent + baseline
 * per-observation arrays. Builds the baseline stddev internally (sample
 * stddev, Bessel-corrected). Conservative thresholds:
 *
 *   - recent_n < min_recent_n  → returns HU_DRIFT_NONE, sigma = 0
 *   - baseline_n < min_baseline_n → returns HU_DRIFT_NONE, sigma = 0
 *   - baseline_stddev < 0.1 * |baseline_mean| → returns NORMAL/NONE
 *
 * `out_sigma`, `out_recent_mean`, `out_baseline_mean` are filled with the
 * computed values (0 when severity is HU_DRIFT_NONE).
 *
 * Returns HU_ERR_INVALID_ARGUMENT if recent or baseline is NULL with
 * non-zero size. */
hu_error_t hu_drift_compute_dimension(const double *recent, size_t recent_n, size_t min_recent_n,
                                      const double *baseline, size_t baseline_n,
                                      size_t min_baseline_n, double *out_sigma,
                                      double *out_recent_mean, double *out_baseline_mean,
                                      hu_drift_severity_t *out_severity);

/* SQL-backed: compute drift alerts for one contact from chat.db.
 *
 * Defaults (encoded internally for callers):
 *   recent_window_days   = 30
 *   baseline_window_days = 180  (preceding the recent window — i.e. days
 *                                30..210 ago)
 *   min_recent_n         = 5    (insufficient signal otherwise)
 *   min_baseline_n       = 30   (no baseline otherwise)
 *   min_total_messages   = 50   (skip computation entirely)
 *
 * Emits ONLY HU_DRIFT_PRONOUNCED alerts by default — one per dimension that
 * exceeds the noise floor (so for a single contact, at most
 * HU_DRIFT_DIM_COUNT alerts in `out`).
 *
 * On HU_IS_TEST / non-Apple / no-SQLite builds: returns HU_ERR_NOT_SUPPORTED
 * with *out_n = 0. */
hu_error_t hu_drift_compute_for_contact(const char *db_path, const char *contact_handle,
                                        int64_t now_unix, hu_drift_alert_t *out, size_t cap,
                                        size_t *out_n);

/* SQL-backed: batch scan — find the top-N most-active contacts (by
 * total message count) and compute drift for each. Same alert-emission
 * rules as hu_drift_compute_for_contact (PRONOUNCED only).
 *
 * Returns HU_ERR_NOT_SUPPORTED in test/non-Apple/no-SQLite builds. */
hu_error_t hu_drift_scan_top_contacts(const char *db_path, int64_t now_unix, size_t top_n,
                                      hu_drift_alert_t *out, size_t cap, size_t *out_n);

#ifdef __cplusplus
}
#endif

#endif /* HU_MEMORY_PATTERN_DRIFT_H */
