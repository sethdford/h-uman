/* include/human/doctor/check_ops.h
 *
 * Operational-truth doctor checks (2026-09-02). Every one of these exists
 * because `human doctor` reported "16 ok, 0 errors" on a morning when:
 *   - the product gate (eval-nightly) had not run since 08-08,
 *   - mlx-server had crash-looped 25 times before 03:00,
 *   - the daemon log was 41 MB with no rotation,
 *   - the iMessage cursor had lagged chat.db by ~1,200 rows for two weeks
 *     (the 09-01 replay incident), and
 *   - the served adapter had never been human-rated while the gate said PASS.
 * A doctor that cannot see any of that is not a doctor.
 *
 * Each check takes an explicit ctx of paths/thresholds so tests inject
 * fixtures; a NULL ctx (or NULL field) resolves the production default under
 * $HOME. None of them spawn a process: the only external command any of them
 * would need (`launchctl print`) is read from ctx text, and the registry
 * wrapper supplies it outside HU_IS_TEST. */
#ifndef HU_DOCTOR_CHECK_OPS_H
#define HU_DOCTOR_CHECK_OPS_H

#include "human/core/error.h"
#include "human/doctor/check.h"

#include <stddef.h>
#include <stdint.h>

/* Shared by the checks below: `explicit` wins; else "$HOME/<rel>" into buf;
 * NULL when neither is available. */
const char *hu_doctor_ops_home_path(const char *explicit, char *buf, size_t cap, const char *rel);
hu_doctor_check_result_t hu_doctor_ops_result(hu_doctor_verdict_t verdict, const char *reason,
                                              const char *detail_json);

/* ── eval_freshness: has the product gate run recently? ─────────────── */
typedef struct hu_doctor_eval_freshness_ctx {
    const char *archive_dir; /* ~/.human/logs/eval-archive (eval-<harness>-<date>.json) */
    const char *nightly_log; /* ~/.human/logs/nightly-eval.log */
    int64_t now_unix;        /* 0 → time(NULL) */
    int max_age_days;        /* 0 → 3 */
} hu_doctor_eval_freshness_ctx_t;
extern const hu_doctor_check_t hu_doctor_check_eval_freshness;
/* Newest non-empty artifact (unix mtime) among archive_dir *.json files and nightly_log; 0 if none.
 */
int64_t hu_doctor_eval_newest_artifact_unix(const char *archive_dir, const char *nightly_log);

/* ── serving_stability: is the local model server crash-looping? ────── */
typedef struct hu_doctor_serving_stability_ctx {
    const char *crash_dir;    /* ~/Library/Logs/DiagnosticReports */
    const char *crash_prefix; /* "Python-" (mlx-server is a Python process) */
    const char
        *launchctl_text; /* output of `launchctl print gui/$UID/<label>`; NULL = unavailable */
    int64_t now_unix;    /* 0 → time(NULL) */
    int window_hours;    /* 0 → 24 */
    int max_crashes;     /* 0 → 2 */
} hu_doctor_serving_stability_ctx_t;
extern const hu_doctor_check_t hu_doctor_check_serving_stability;
/* Pure parsers, exposed for tests and for the registry wrapper. */
int64_t hu_doctor_launchctl_runs(const char *text);      /* -1 when absent */
int64_t hu_doctor_launchctl_last_exit(const char *text); /* INT64_MIN when absent */

/* ── log_hygiene: is the daemon log bounded? ───────────────────────── */
typedef struct hu_doctor_log_hygiene_ctx {
    const char *log_path; /* ~/.human/logs/service-loop-error.log */
    int64_t max_bytes;    /* 0 → 50 MB */
} hu_doctor_log_hygiene_ctx_t;
extern const hu_doctor_check_t hu_doctor_check_log_hygiene;
/* Size of `path` into *out_bytes; false when the file is absent. */
bool hu_doctor_log_size(const char *path, int64_t *out_bytes);

/* ── imessage_cursor: would a restart replay old messages? ─────────── */
typedef struct hu_doctor_imessage_cursor_ctx {
    const char *rowid_path;  /* ~/.human/imessage.rowid */
    const char *chatdb_path; /* ~/Library/Messages/chat.db */
    int64_t max_gap;         /* 0 → 50 (HU_IMESSAGE_MAX_REPLAY default) */
} hu_doctor_imessage_cursor_ctx_t;
extern const hu_doctor_check_t hu_doctor_check_imessage_cursor;

/* ── blind_ab_gate: does the human gate vouch for what is served? ───── */
typedef struct hu_doctor_blind_ab_gate_ctx {
    const char *home_gate; /* ~/.human/blind_ab_gate.json (what the C LoRA gate reads) */
    const char *repo_gate; /* docs/evaluation/blind_ab_gate.json (what CI reads); NULL = skip */
    const char
        *served_adapter; /* basename or path of the adapter on the live server; NULL = unknown */
    int64_t now_unix;    /* 0 → time(NULL) */
    int max_age_days;    /* 0 → 45 */
} hu_doctor_blind_ab_gate_ctx_t;
extern const hu_doctor_check_t hu_doctor_check_blind_ab_gate;
/* Gate timestamp ("YYYY-MM-DDTHH:MM:SS", local time) → unix; 0 when unparseable. */
int64_t hu_doctor_gate_parse_ts(const char *ts);

#endif /* HU_DOCTOR_CHECK_OPS_H */
